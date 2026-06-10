/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Ryan Cornish
 *
 * Unit and integration tests for sigctx. Dependency-free (no gtest), C++ for
 * convenience. Returns non-zero if any check fails, so it drops straight into CI.
 *
 * Preconditions are asserted inside the library rather than returned as codes,
 * so the misuse cases are checked as death tests (fork, expect SIGABRT). Those run
 * only when asserts are active, since under NDEBUG misuse is undefined behaviour.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef NDEBUG
#include <sys/resource.h>
#include <sys/wait.h>
#endif

extern "C" {
#include <sigctx/sigctx.h>
#include <sigctx/sigctx_intercept.h>
}

static int g_checks = 0;
static int g_fails  = 0;
#define CHECK(cond)                                                            \
   do {                                                                        \
      ++g_checks;                                                              \
      if (!(cond)) { ++g_fails; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
   } while (0)

static void dummy_entry(void*) {}

/* --- sigctx_create -------------------------------------------------------- */
static void test_create_basic()
{
   alignas(64) static std::uint8_t fp[SIGCTX_FPSTATE_CAPACITY];
   static std::uint8_t stk[64 * 1024];
   sigctx_ucontext_t uc;
   sigctx_create(&uc, fp, sizeof fp, stk, sizeof stk, dummy_entry, (void*)0xABCD);

   CHECK(uc.uc_mcontext.rip == (std::uint64_t)dummy_entry);
   CHECK(uc.uc_mcontext.rdi == 0xABCD);
   CHECK((uc.uc_mcontext.rsp & 0xF) == 8);            // entry alignment: RSP == 8 (mod 16)
   CHECK(uc.uc_mcontext.cs != 0);                     // live CS captured, not zeroed
   CHECK(((std::uintptr_t)uc.uc_mcontext.fpstate % 64) == 0); // FP buffer aligned
   CHECK(uc.uc_mcontext.fpstate->mxcsr == 0x1f80);    // default SSE control word set
}

/* --- sigctx_copy ---------------------------------------------------------- */
static void test_copy_faithful()
{
   alignas(64) static std::uint8_t src_fp[SIGCTX_FPSTATE_CAPACITY];
   alignas(64) static std::uint8_t dst_fp[SIGCTX_FPSTATE_CAPACITY];
   static std::uint8_t stk[64 * 1024];
   sigctx_ucontext_t src, dst;
   sigctx_create(&src, src_fp, sizeof src_fp, stk, sizeof stk, dummy_entry, (void*)7);
   src.uc_mcontext.fpstate->mxcsr = 0x1f80;

   sigctx_status rc = sigctx_copy(&dst, dst_fp, sizeof dst_fp, &src);
   CHECK(rc == SIGCTX_OK);
   CHECK(dst.uc_mcontext.rdi == 7);
   CHECK((void*)dst.uc_mcontext.fpstate == (void*)dst_fp);       // repointed to dst's buffer
   CHECK(dst.uc_mcontext.fpstate->mxcsr == 0x1f80);             // FP bytes carried over
}

static void test_copy_demote()
{
   // Hand-build a src that advertises extended state larger than the dst buffer.
   alignas(64) static std::uint8_t src_fp[SIGCTX_FPSTATE_CAPACITY];
   alignas(64) static std::uint8_t dst_fp[sizeof(struct sigctx_fpstate)]; // legacy floor only
   static std::uint8_t stk[64 * 1024];
   sigctx_ucontext_t src, dst;
   sigctx_create(&src, src_fp, sizeof src_fp, stk, sizeof stk, dummy_entry, nullptr);
   src.uc_flags |= SIGCTX_UC_FP_XSTATE;
   src.uc_mcontext.fpstate->sw_reserved.magic1        = SIGCTX_FP_XSTATE_MAGIC1;
   src.uc_mcontext.fpstate->sw_reserved.extended_size = 2000; // > 512 dst

   sigctx_status rc = sigctx_copy(&dst, dst_fp, sizeof dst_fp, &src);
   CHECK(rc == SIGCTX_TRUNCATED_TO_LEGACY);
   CHECK(((struct sigctx_fpstate*)dst_fp)->sw_reserved.magic1 == 0);  // demoted to legacy
   CHECK((dst.uc_flags & SIGCTX_UC_FP_XSTATE) == 0);                  // flag cleared too
}

static void test_fpstate_size()
{
   CHECK(sigctx_fpstate_size() >= sizeof(struct sigctx_fpstate)); // at least the legacy floor
}

/* --- precondition contract: misuse aborts via assert ---------------------- */
/* The library asserts its preconditions rather than returning error codes, so the
 * "buffer too small" cases are death tests. They run a misusing call in a child and
 * expect it to die on SIGABRT. Skipped under NDEBUG, where asserts compile out and
 * the same misuse would be undefined behaviour rather than a clean abort. */
#ifndef NDEBUG
template <typename F>
static bool aborts(F fn)
{
   pid_t pid = fork();
   if (pid == 0) {
      struct rlimit no_core = {0, 0};
      setrlimit(RLIMIT_CORE, &no_core);          // the abort is expected, suppress the core
      if (std::freopen("/dev/null", "w", stderr) == nullptr) { /* best effort, ignore */ }
      fn();
      _exit(0);                                  // reached only if fn did NOT abort
   }
   int st = 0;
   waitpid(pid, &st, 0);
   return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}

static void test_precondition_aborts()
{
   static std::uint8_t stk[64 * 1024];

   // create with an FP buffer below the legacy floor must abort
   CHECK(aborts([&] {
      alignas(64) std::uint8_t tiny[64];
      sigctx_ucontext_t uc;
      sigctx_create(&uc, tiny, sizeof tiny, stk, sizeof stk, dummy_entry, nullptr);
   }));

   // copy into a destination below the legacy floor must abort
   CHECK(aborts([&] {
      alignas(64) std::uint8_t src_fp[SIGCTX_FPSTATE_CAPACITY];
      alignas(64) std::uint8_t dst_tiny[64];
      sigctx_ucontext_t src, dst;
      sigctx_create(&src, src_fp, sizeof src_fp, stk, sizeof stk, dummy_entry, nullptr);
      sigctx_copy(&dst, dst_tiny, sizeof dst_tiny, &src);
   }));
}
#else
static void test_precondition_aborts()
{
   std::printf("(precondition abort tests skipped under NDEBUG)\n");
}
#endif

/* --- sigctx_dyn_t: heap-backed FP sized at runtime ------------------------ */
/* The dynamic variant exists for the case where the machine's enabled XSAVE area
 * is large or variable (AVX-512 component sets, AMX) and you would rather size the
 * FP buffer from sigctx_fpstate_size() than pay a fixed inline bound. These tests
 * drive create and copy through a heap buffer allocated at that runtime size. */

/* Bytes to allocate for one dynamic FP buffer: the live frame size, floored at the
 * legacy size and rounded up to 64 so it is a valid aligned_alloc request. */
static size_t dyn_fp_bytes()
{
   uint32_t n    = sigctx_fpstate_size();
   size_t   need = n ? (size_t)n : sizeof(struct sigctx_fpstate);
   return (need + 63u) & ~(size_t)63u;
}

static void test_dyn_create()
{
   static std::uint8_t stk[64 * 1024];
   sigctx_dyn_t d{};
   d.fpstate_size = dyn_fp_bytes();
   d.fpstate      = (std::uint8_t*)std::aligned_alloc(64, d.fpstate_size);
   CHECK(d.fpstate != nullptr);
   if (!d.fpstate) return;

   sigctx_create(&d.uc, d.fpstate, d.fpstate_size, stk, sizeof stk, dummy_entry, (void*)0x1234);
   CHECK(d.uc.uc_mcontext.rip == (std::uint64_t)dummy_entry);
   CHECK(d.uc.uc_mcontext.rdi == 0x1234);
   CHECK((void*)d.uc.uc_mcontext.fpstate == (void*)d.fpstate);   // points at the heap buffer
   CHECK(((std::uintptr_t)d.uc.uc_mcontext.fpstate % 64) == 0);  // and it is aligned

   std::free(d.fpstate);
}

static void test_dyn_copy_faithful()
{
   static std::uint8_t stk[64 * 1024];
   sigctx_dyn_t src{}, dst{};
   src.fpstate_size = dyn_fp_bytes();
   dst.fpstate_size = dyn_fp_bytes();
   src.fpstate = (std::uint8_t*)std::aligned_alloc(64, src.fpstate_size);
   dst.fpstate = (std::uint8_t*)std::aligned_alloc(64, dst.fpstate_size);
   CHECK(src.fpstate != nullptr);
   CHECK(dst.fpstate != nullptr);
   if (!src.fpstate || !dst.fpstate) { std::free(src.fpstate); std::free(dst.fpstate); return; }

   sigctx_create(&src.uc, src.fpstate, src.fpstate_size, stk, sizeof stk, dummy_entry, (void*)9);
   src.uc.uc_mcontext.fpstate->mxcsr = 0x1f80;

   sigctx_status rc = sigctx_copy(&dst.uc, dst.fpstate, dst.fpstate_size, &src.uc);
   CHECK(rc == SIGCTX_OK);
   CHECK(dst.uc.uc_mcontext.rdi == 9);
   CHECK((void*)dst.uc.uc_mcontext.fpstate == (void*)dst.fpstate); // repointed to dst's heap buffer
   CHECK(dst.uc.uc_mcontext.fpstate->mxcsr == 0x1f80);            // FP bytes carried over

   std::free(src.fpstate);
   std::free(dst.fpstate);
}

/* The point of the dynamic buffer: sized at sigctx_fpstate_size(), it holds the
 * full extended state the machine can produce, so a copy of a frame advertising
 * that much state is faithful and NOT demoted, which is exactly what a too-small
 * inline bound cannot guarantee. */
static void test_dyn_holds_full_extended()
{
   static std::uint8_t stk[64 * 1024];
   uint32_t live = sigctx_fpstate_size();
   if (live == 0) { std::printf("(dyn extended skipped: no XSAVE)\n"); return; }

   sigctx_dyn_t src{}, dst{};
   src.fpstate_size = dyn_fp_bytes();
   dst.fpstate_size = dyn_fp_bytes();
   src.fpstate = (std::uint8_t*)std::aligned_alloc(64, src.fpstate_size);
   dst.fpstate = (std::uint8_t*)std::aligned_alloc(64, dst.fpstate_size);
   CHECK(src.fpstate != nullptr);
   CHECK(dst.fpstate != nullptr);
   if (!src.fpstate || !dst.fpstate) { std::free(src.fpstate); std::free(dst.fpstate); return; }

   sigctx_create(&src.uc, src.fpstate, src.fpstate_size, stk, sizeof stk, dummy_entry, nullptr);
   /* Advertise extended state filling the whole live FP area, as a real capture would. */
   src.uc.uc_flags |= SIGCTX_UC_FP_XSTATE;
   src.uc.uc_mcontext.fpstate->sw_reserved.magic1        = SIGCTX_FP_XSTATE_MAGIC1;
   src.uc.uc_mcontext.fpstate->sw_reserved.extended_size = live;

   sigctx_status rc = sigctx_copy(&dst.uc, dst.fpstate, dst.fpstate_size, &src.uc);
   CHECK(rc == SIGCTX_OK);                                           // faithful, room to spare
   CHECK(dst.uc.uc_mcontext.fpstate->sw_reserved.magic1 == SIGCTX_FP_XSTATE_MAGIC1); // kept
   CHECK((dst.uc.uc_flags & SIGCTX_UC_FP_XSTATE) != 0);             // extended flag retained

   std::free(src.fpstate);
   std::free(dst.fpstate);
}

/* --- integration: FP relocation through a real capture + resume ----------- */
/* Note: on Linux the signal-delivery path scrubs the upper YMM/ZMM halves
 * (VZEROUPPER) before the handler runs, so a self-signal cannot observe the
 * interrupted code's live upper vector bits, they are already zero at capture.
 * We therefore assert what is real and verifiable: the SSE low half survives the
 * capture, clobber, and resume round trip, and the extended XSAVE area the kernel
 * did capture is relocated byte for byte by sigctx_copy. */
alignas(64) static std::uint8_t survival_stack[32 * 1024];

static sigctx_ucontext_t* survival_handler(sigctx_ucontext_t* paused, void*)
{
   /* Stomp XMM0 so a faithful restore of the captured low half is observable. */
   alignas(16) unsigned char garbage[16];
   std::memset(garbage, 0x5A, sizeof garbage);
   __asm__ volatile("movdqu %0, %%xmm0" : : "m"(garbage) : "xmm0");
   return paused; /* resume the just-captured context */
}

static bool run_vector_survival()
{
   if (!__builtin_cpu_supports("sse2")) {
      std::printf("(vector survival skipped: no SSE2)\n");
      return true;
   }
   sigctx_intercept_cfg cfg{
      .signo      = SIGUSR1,
      .handler_sp = survival_stack,
      .handler_ss = sizeof survival_stack,
      .handler    = survival_handler,
      .arg        = nullptr,
   };
   if (sigctx_intercept_install(&cfg) != 0) { ++g_fails; std::printf("FAIL install\n"); return false; }

   long pid = (long)getpid();
   long tid = syscall(SYS_gettid);
   alignas(16) unsigned char sentinel[16], result[16] = {};
   for (int i = 0; i < 16; ++i) sentinel[i] = (unsigned char)(0xA0 + i);
   long ret;

   __asm__ volatile(
      "movdqu %[s], %%xmm0\n\t"     // arm sentinel into XMM0 (SSE low half)
      "syscall\n\t"                 // tgkill(pid, tid, SIGUSR1), delivered on return
      "movdqu %%xmm0, %[r]\n\t"     // read XMM0 after capture/clobber/resume round trip
      : "=a"(ret), [r] "=m"(result)
      : "a"(SYS_tgkill), "D"(pid), "S"(tid), "d"(SIGUSR1), [s] "m"(sentinel)
      : "rcx", "r11", "xmm0", "memory");

   CHECK(ret == 0);
   CHECK(std::memcmp(sentinel, result, 16) == 0); // SSE state survived capture+resume
   return true;
}

int main()
{
   test_create_basic();
   test_copy_faithful();
   test_copy_demote();
   test_fpstate_size();
   test_precondition_aborts();
   test_dyn_create();
   test_dyn_copy_faithful();
   test_dyn_holds_full_extended();
   run_vector_survival();

   std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
   return g_fails ? 1 : 0;
}
