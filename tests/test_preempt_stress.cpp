/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Ryan Cornish
 *
 * Directed preemption stress test for sigctx_intercept.
 *
 * A worker thread installs the interceptor and then runs ordinary compute code.
 * A second pthread repeatedly sends a thread-directed signal to the worker while
 * it is inside that compute loop. The handler deliberately clobbers some register
 * state and returns the captured context, so the worker should continue as if the
 * asynchronous preemption had not happened.
 */
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <unistd.h>

extern "C" {
#include <sigctx/sigctx.h>
#include <sigctx/sigctx_intercept.h>
}

static constexpr int      kSignal          = SIGUSR2;
static constexpr unsigned kTargetPreempts  = 2000;
static constexpr unsigned kMaxRounds       = 200;
static constexpr uint64_t kItersPerRound   = 250000;

/* A heavier set of parameters */
#if 0
static constexpr unsigned kTargetPreempts  = 2000000;
static constexpr unsigned kMaxRounds       = 25000;
static constexpr uint64_t kItersPerRound   = 2000000;
#endif

static std::atomic<bool>     g_ready{false};
static std::atomic<bool>     g_done{false};
static std::atomic<unsigned> g_preempts{0};
static std::atomic<int>      g_install_rc{0};
static pthread_t             g_worker_thread{};

static uint8_t* g_handler_stack      = nullptr;
static size_t   g_handler_stack_size = 0;

static uint64_t rotl64(uint64_t x, unsigned r)
{
   return (x << r) | (x >> (64u - r));
}

static uint64_t round_up_64(uint64_t n)
{
   return (n + 63u) & ~uint64_t{63u};
}

/* Keep several live values in registers. If the captured register file is not
 * restored faithfully across arbitrary signal delivery, this checksum tends to
 * diverge quickly rather than merely crashing. */
__attribute__((noinline))
static uint64_t preemptible_work(uint64_t seed, uint64_t n)
{
   uint64_t a = 0x243f6a8885a308d3ull ^ seed;
   uint64_t b = 0x13198a2e03707344ull + seed;
   uint64_t c = 0xa4093822299f31d0ull ^ (seed << 1);
   uint64_t d = 0x082efa98ec4e6c89ull + (seed << 7);
   uint64_t e = 0x452821e638d01377ull ^ (seed >> 3);
   uint64_t f = 0xbe5466cf34e90c6cull + (seed * 3u);

   for (uint64_t i = 0; i < n; ++i) {
      asm volatile("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(f) :: "memory");
      a += rotl64(b ^ i, 7);
      b ^= rotl64(c + a, 11);
      c += d ^ (i * 0x9e3779b97f4a7c15ull);
      d = rotl64(d + e + i, 17);
      e ^= a + rotl64(f, 23);
      f += b ^ rotl64(c, 31);
   }

   asm volatile("" : "+r"(a), "+r"(b), "+r"(c), "+r"(d), "+r"(e), "+r"(f) :: "memory");
   return a ^ rotl64(b, 3) ^ rotl64(c, 13) ^ rotl64(d, 29) ^ rotl64(e, 41) ^ rotl64(f, 53);
}

static sigctx_ucontext_t* preempt_handler(sigctx_ucontext_t* paused, void*)
{
   g_preempts.fetch_add(1, std::memory_order_relaxed);

#if defined(__x86_64__)
   /* Make the resumed context prove that sigreturn restored the interrupted
    * register state, not that the handler happened to preserve it. */
   asm volatile(
      "pxor %%xmm0, %%xmm0\n\t"
      "pxor %%xmm1, %%xmm1\n\t"
      "pxor %%xmm2, %%xmm2\n\t"
      "pxor %%xmm3, %%xmm3\n\t"
      ::: "xmm0", "xmm1", "xmm2", "xmm3", "memory");
#endif

   volatile uint64_t x = 0xfeedfacecafebeefull;
   for (unsigned i = 0; i < 64; ++i) {
      x = rotl64(x + i, 9) ^ 0xd6e8feb86659fd93ull;
   }
   (void)x;

   return paused;
}

struct worker_result
{
   uint64_t checksum;
   unsigned rounds;
};

static void* worker_main(void* arg)
{
   worker_result* result = static_cast<worker_result*>(arg);

   sigctx_intercept_cfg cfg{};
   cfg.signo      = kSignal;
   cfg.handler_sp = g_handler_stack;
   cfg.handler_ss = g_handler_stack_size;
   cfg.handler    = preempt_handler;
   cfg.arg        = nullptr;

   int rc = sigctx_intercept_install(&cfg);
   g_install_rc.store(rc, std::memory_order_release);
   g_ready.store(true, std::memory_order_release);
   if (rc != 0) {
      g_done.store(true, std::memory_order_release);
      return nullptr;
   }

   uint64_t checksum = 0;
   unsigned rounds = 0;
   while (rounds < kMaxRounds && g_preempts.load(std::memory_order_relaxed) < kTargetPreempts) {
      uint64_t seed = 0x100000001b3ull + rounds;
      checksum ^= rotl64(preemptible_work(seed, kItersPerRound), rounds % 63u + 1u);
      ++rounds;
   }

   result->checksum = checksum;
   result->rounds   = rounds;
   g_done.store(true, std::memory_order_release);
   return nullptr;
}

static void* kicker_main(void*)
{
   while (!g_ready.load(std::memory_order_acquire)) {
      sched_yield();
   }

   while (!g_done.load(std::memory_order_acquire)) {
      int rc = pthread_kill(g_worker_thread, kSignal);
      if (rc != 0 && rc != ESRCH) {
         std::printf("pthread_kill failed: %d\n", rc);
         break;
      }
      sched_yield();
   }
   return nullptr;
}

static uint64_t reference_checksum(unsigned rounds)
{
   uint64_t checksum = 0;
   for (unsigned r = 0; r < rounds; ++r) {
      uint64_t seed = 0x100000001b3ull + r;
      checksum ^= rotl64(preemptible_work(seed, kItersPerRound), r % 63u + 1u);
   }
   return checksum;
}

int main()
{
   uint32_t xsave = sigctx_fpstate_size();
   if (xsave == 0) {
      xsave = sizeof(struct sigctx_fpstate);
   }
   g_handler_stack_size = static_cast<size_t>(round_up_64(sizeof(sigctx_ucontext_t) + xsave + 64u * 1024u));
   g_handler_stack = static_cast<uint8_t*>(std::aligned_alloc(SIGCTX_FPSTATE_ALIGN, g_handler_stack_size));
   if (!g_handler_stack) {
      std::perror("aligned_alloc");
      return 1;
   }

   worker_result got{};
   pthread_t kicker{};

   int rc = pthread_create(&g_worker_thread, nullptr, worker_main, &got);
   if (rc != 0) {
      std::printf("pthread_create(worker) failed: %d\n", rc);
      std::free(g_handler_stack);
      return 1;
   }
   rc = pthread_create(&kicker, nullptr, kicker_main, nullptr);
   if (rc != 0) {
      std::printf("pthread_create(kicker) failed: %d\n", rc);
      g_done.store(true, std::memory_order_release);
      pthread_join(g_worker_thread, nullptr);
      std::free(g_handler_stack);
      return 1;
   }

   pthread_join(g_worker_thread, nullptr);
   g_done.store(true, std::memory_order_release);
   pthread_join(kicker, nullptr);

   std::free(g_handler_stack);

   int install_rc = g_install_rc.load(std::memory_order_acquire);
   if (install_rc != 0) {
      std::printf("sigctx_intercept_install failed: %d\n", install_rc);
      return 1;
   }

   uint64_t expect = reference_checksum(got.rounds);
   unsigned preempts = g_preempts.load(std::memory_order_relaxed);

   std::printf("preemptions=%u rounds=%u checksum=%016llx expect=%016llx\n",
               preempts, got.rounds,
               static_cast<unsigned long long>(got.checksum),
               static_cast<unsigned long long>(expect));

   if (preempts < kTargetPreempts) {
      std::printf("FAIL: too few preemptions\n");
      return 1;
   }
   if (got.rounds == 0 || got.rounds > kMaxRounds) {
      std::printf("FAIL: invalid round count\n");
      return 1;
   }
   if (got.checksum != expect) {
      std::printf("FAIL: checksum mismatch after directed preemption\n");
      return 1;
   }

   return 0;
}
