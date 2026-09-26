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
#include <cerrno>

#include <pthread.h>
#include <atomic>
#include <thread>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <sys/wait.h>          // waitpid: used by the black-box create-run test (all builds)

#ifndef NDEBUG
#include <sys/resource.h>      // setrlimit: death tests only
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

   CHECK(uc.uc_mcontext.rdi == (std::uint64_t)dummy_entry); // trampoline will call entry
   CHECK(uc.uc_mcontext.rsi == 0xABCD);               // with this arg
   CHECK((uc.uc_mcontext.rsp & 0xF) == 0);            // 16-aligned; the trampoline's call yields 8 (mod 16)
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
   CHECK(dst.uc_mcontext.rsi == 7);                              // arg (now in rsi) carried by the copy
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

   // install with a null altstack is a caller-contract violation and must abort,
   // distinct from an undersized altstack, which is a runtime -ENOMEM return.
   CHECK(aborts([&] {
      alignas(64) static std::uint8_t hs[128 * 1024];
      sigctx_intercept_cfg cfg{};
      cfg.signo = SIGUSR1;
      cfg.altstack_sp = nullptr;      // the violation
      cfg.altstack_ss = 256 * 1024;
      cfg.handler_sp = hs;
      cfg.handler_ss = sizeof hs;
      cfg.handler = [](sigctx_ucontext_t* p, void*) { return p; };
      (void)sigctx_intercept_install(&cfg);
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
   CHECK(d.uc.uc_mcontext.rdi == (std::uint64_t)dummy_entry); // trampoline will call entry
   CHECK(d.uc.uc_mcontext.rsi == 0x1234);                     // with this arg
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
   CHECK(dst.uc.uc_mcontext.rsi == 9);                          // arg (now in rsi) carried by the copy
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
alignas(64) static std::uint8_t survival_altstack[128 * 1024];

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
      .signo       = SIGUSR1,
      .altstack_sp = survival_altstack,
      .altstack_ss = sizeof survival_altstack,
      .handler_sp  = survival_stack,
      .handler_ss  = sizeof survival_stack,
      .handler     = survival_handler,
      .arg         = nullptr,
      .block_extra = nullptr,
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

/* --- block_extra: hold a second signal masked across the whole interception --- */
/* The trigger signal is masked through both phases of an interception already. This
 * checks that block_extra extends that guarantee to an additional signal, in the
 * handler phase (the trampoline, where a scheduler does its real work), and that the
 * extra signal is NOT carried into the resumed context, whose mask is its own. */
alignas(64) static std::uint8_t block_extra_stack[128 * 1024];
alignas(64) static std::uint8_t block_extra_altstack[128 * 1024];
static int g_extra_blocked_in_handler = -1;

static sigctx_ucontext_t* block_extra_handler(sigctx_ucontext_t* paused, void*)
{
   sigset_t m;
   pthread_sigmask(SIG_BLOCK, nullptr, &m);              // the trampoline runs as ordinary code
   g_extra_blocked_in_handler = sigismember(&m, SIGUSR2);
   return paused;
}

static void run_block_extra()
{
   sigset_t want_unblocked;
   sigemptyset(&want_unblocked);
   sigaddset(&want_unblocked, SIGUSR2);
   pthread_sigmask(SIG_UNBLOCK, &want_unblocked, nullptr); // start with SIGUSR2 open

   sigctx_intercept_cfg cfg{};
   cfg.signo = SIGUSR1;
   cfg.altstack_sp = block_extra_altstack;
   cfg.altstack_ss = sizeof block_extra_altstack;
   cfg.handler_sp = block_extra_stack;
   cfg.handler_ss = sizeof block_extra_stack;
   cfg.handler = block_extra_handler;

   // With block_extra set, SIGUSR2 is held blocked through the handler phase.
   sigset_t extra;
   sigemptyset(&extra);
   sigaddset(&extra, SIGUSR2);
   cfg.block_extra = &extra;
   g_extra_blocked_in_handler = -1;
   if (sigctx_intercept_install(&cfg) != 0) { ++g_fails; std::printf("FAIL be install\n"); return; }
   raise(SIGUSR1);

   sigset_t after;
   pthread_sigmask(SIG_BLOCK, nullptr, &after);
   CHECK(g_extra_blocked_in_handler == 1);              // extra masked during the handler
   CHECK(sigismember(&after, SIGUSR2) == 0);            // and not leaked into the resumed context

   // Control: with block_extra left NULL, the same signal stays open in the handler,
   // proving the masking is the field's effect and the default path is unchanged.
   cfg.block_extra = nullptr;
   g_extra_blocked_in_handler = -1;
   if (sigctx_intercept_install(&cfg) != 0) { ++g_fails; std::printf("FAIL be install (control)\n"); return; }
   raise(SIGUSR1);
   CHECK(g_extra_blocked_in_handler == 0);              // default behaviour unchanged
}

/* Black-box: a created context, when resumed, actually runs its entry with the given
 * arg. It runs in a child so the entry can end the run with _exit rather than needing a
 * return path, and the parent reads the outcome from the exit status. This is the
 * functional coverage that does not depend on the register-layout details the entry
 * trampoline (sigctx_context_start) owns. */
static void bb_entry(void* a)
{
   _exit((unsigned long)(std::uintptr_t)a == 0xBEEFu ? 42 : 7);
}

static void test_create_runs_entry()
{
   pid_t pid = fork();
   if (pid == 0) {
      alignas(64) static std::uint8_t fp[SIGCTX_FPSTATE_CAPACITY];
      static std::uint8_t stk[64 * 1024];
      sigctx_ucontext_t c;
      sigctx_create(&c, fp, sizeof fp, stk, sizeof stk, bb_entry, (void*)0xBEEFu);
      sigctx_resume(&c);
      _exit(9); /* unreachable if resume works */
   }
   int st = 0;
   waitpid(pid, &st, 0);
   CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42);       // entry ran with the right arg
}

/* --- altstack sizing queries ---------------------------------------------- */
/* The install-time sizing math is now caller-facing via two query functions, so
 * a caller can size a buffer up front instead of trial-and-error against errno.
 * These assert the contract the docs promise: a slot covers one runtime frame,
 * and min(depth) scales it, with depth 0 treated as 1. */
static void test_altstack_sizing()
{
   size_t slot = sigctx_altstack_slot_min();
   long   want = sysconf(_SC_SIGSTKSZ);

   CHECK(slot > 0);
   if (want > 0) {
      CHECK(slot >= (size_t)want);          // covers one runtime signal frame
   }
   CHECK(sigctx_altstack_min(1) == slot);   // depth 1 is one slot
   CHECK(sigctx_altstack_min(4) == 4 * slot); // scales linearly with depth
   CHECK(sigctx_altstack_min(0) == slot);   // depth 0 clamps to 1, never a zero-size stack
}

/* --- install rejects undersized stacks with the documented codes ----------- */
/* Sizing failures are runtime returns (not asserts) because a caller cannot
 * precompute them. These confirm each code fires for its buffer and that a
 * correctly sized config still installs, so the checks are guarding the
 * boundary, not a blanket rejection. */
alignas(64) static std::uint8_t good_handler_stack[128 * 1024];
alignas(64) static std::uint8_t good_altstack[256 * 1024];

static sigctx_ucontext_t* noop_handler(sigctx_ucontext_t* paused, void*) { return paused; }

static void test_install_size_rejects()
{
   sigctx_intercept_cfg cfg{
      .signo       = SIGUSR1,
      .altstack_sp = good_altstack,
      .altstack_ss = sizeof good_altstack,
      .handler_sp  = good_handler_stack,
      .handler_ss  = sizeof good_handler_stack,
      .handler     = noop_handler,
      .arg         = nullptr,
      .block_extra = nullptr,
   };

   // Undersized altstack -> -ENOMEM.
   {
      sigctx_intercept_cfg bad = cfg;
      bad.altstack_ss = 64; // far below one signal frame
      CHECK(sigctx_intercept_install(&bad) == -ENOMEM);
   }

   // Undersized handler stack -> -ERANGE.
   {
      sigctx_intercept_cfg bad = cfg;
      bad.handler_ss = 64;
      CHECK(sigctx_intercept_install(&bad) == -ERANGE);
   }

   // Correctly sized -> success. Proves the rejections above are boundary
   // checks, not a blanket failure of the new field.
   CHECK(sigctx_intercept_install(&cfg) == 0);
}

/* --- nesting: a second SA_ONSTACK signal preempts a live handler ----------- */
/* The property stage 2 depends on and that the resized, caller-owned altstack
 * exists to serve: while the interceptor's handler runs on the altstack, a
 * different, higher-priority signal can be delivered, nest its own frame ABOVE
 * the current one, run to completion, pop, and let the interrupted handler
 * finish, all on one altstack. The nesting signal is a plain SA_ONSTACK handler
 * (not a second interceptor), which is exactly the timer/peripheral shape.
 *
 * A failure here (altstack too small, or nesting mishandled) manifests as
 * corruption or a crash inside the nested delivery, so reaching the post-raise
 * checks with both flags set is the proof. */
alignas(64) static std::uint8_t nest_handler_stack[128 * 1024];
alignas(64) static std::uint8_t nest_altstack[256 * 1024];
static volatile int g_nest_ran       = 0;
static volatile int g_outer_resumed  = 0;
static constexpr int kNestSignal     = SIGUSR2;

static void nest_plain_handler(int) // plain SA_ONSTACK handler, nests on the altstack
{
   g_nest_ran = 1;
}

static sigctx_ucontext_t* nest_outer_handler(sigctx_ucontext_t* paused, void*)
{
   // We are running on the altstack. Raise the nesting signal at ourselves and
   // unblock it: it must be delivered NOW, above this frame, run, and return
   // here so we can resume the captured context.
   raise(kNestSignal);
   sigset_t unblock;
   sigemptyset(&unblock);
   sigaddset(&unblock, kNestSignal);
   pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);
   // If nesting works, g_nest_ran is set by the time we get here.
   g_outer_resumed = 1;
   return paused;
}

static void test_altstack_nesting()
{
   // Plain nesting handler on the altstack.
   struct sigaction sa;
   std::memset(&sa, 0, sizeof sa);
   sa.sa_handler = nest_plain_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_ONSTACK;
   if (sigaction(kNestSignal, &sa, nullptr) != 0) { ++g_fails; std::printf("FAIL nest sigaction\n"); return; }

   sigctx_intercept_cfg cfg{
      .signo       = SIGUSR1,
      .altstack_sp = nest_altstack,
      .altstack_ss = sizeof nest_altstack,
      .handler_sp  = nest_handler_stack,
      .handler_ss  = sizeof nest_handler_stack,
      .handler     = nest_outer_handler,
      .arg         = nullptr,
      .block_extra = nullptr, // do NOT block the nesting signal: we want it to nest
   };
   if (sigctx_intercept_install(&cfg) != 0) { ++g_fails; std::printf("FAIL nest install\n"); return; }

   g_nest_ran = 0;
   g_outer_resumed = 0;
   raise(SIGUSR1); // enters nest_outer_handler, which triggers the nested delivery

   CHECK(g_nest_ran == 1);      // the nested handler ran above the interceptor handler
   CHECK(g_outer_resumed == 1); // and the interceptor handler resumed cleanly afterward
}

/* --- uninstall: hand the thread back as it was ---------------------------- */
/* A caller that frees its handler stack or altstack must first stop the thread
 * from pointing at them. Install leaves three things behind: the thread's
 * interceptor config (naming the handler stack), the registered altstack, and
 * the process-wide disposition. These pin that uninstall undoes all three, and
 * only undoes the process-wide one when the LAST installed thread leaves. */
alignas(64) static std::uint8_t un_handler_stack[128 * 1024];
alignas(64) static std::uint8_t un_altstack[256 * 1024];
alignas(64) static std::uint8_t un_prior_altstack[256 * 1024];
static volatile int g_prior_handler_ran = 0;
static volatile int g_interceptor_ran   = 0;

static void prior_handler(int) { g_prior_handler_ran = 1; }

static sigctx_ucontext_t* counting_handler(sigctx_ucontext_t* paused, void*)
{
   g_interceptor_ran = 1;
   return paused;
}

static sigctx_intercept_cfg uninstall_cfg()
{
   sigctx_intercept_cfg cfg{};
   cfg.signo       = SIGUSR1;
   cfg.altstack_sp = un_altstack;
   cfg.altstack_ss = sizeof un_altstack;
   cfg.handler_sp  = un_handler_stack;
   cfg.handler_ss  = sizeof un_handler_stack;
   cfg.handler     = counting_handler;
   return cfg;
}

/* Run fn in a child whose verdict is its exit code. A forked child inherits the
 * forking thread's sigctx state along with its altstack and dispositions, and
 * earlier tests installed on this thread, so the child first hands all of that
 * back. Each case then really does start from "nothing installed". */
template<typename F>
static bool in_child(F fn)
{
   pid_t pid = fork();
   if (pid == 0) {
      if (sigctx_intercept_uninstall() != 0) _exit(2);
      _exit(fn() ? 0 : 1);
   }
   int st = 0;
   waitpid(pid, &st, 0);
   return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static void test_uninstall()
{
   // Nothing installed on this thread: a no-op that succeeds.
   CHECK(in_child([] { return sigctx_intercept_uninstall() == 0; }));

   // The altstack the thread had BEFORE install is the one it gets back.
   CHECK(in_child([] {
      stack_t prior{};
      prior.ss_sp   = un_prior_altstack;
      prior.ss_size = sizeof un_prior_altstack;
      if (sigaltstack(&prior, nullptr) != 0) return false;

      sigctx_intercept_cfg cfg = uninstall_cfg();
      if (sigctx_intercept_install(&cfg) != 0) return false;
      if (sigctx_intercept_install(&cfg) != 0) return false;   // re-install keeps the original
      if (sigctx_intercept_uninstall() != 0) return false;

      stack_t now{};
      sigaltstack(nullptr, &now);
      return now.ss_sp == un_prior_altstack && (now.ss_flags & SS_DISABLE) == 0;
   }));

   // A thread that had no altstack gets none back, rather than a dangling one.
   CHECK(in_child([] {
      sigctx_intercept_cfg cfg = uninstall_cfg();
      if (sigctx_intercept_install(&cfg) != 0) return false;
      if (sigctx_intercept_uninstall() != 0) return false;
      stack_t now{};
      sigaltstack(nullptr, &now);
      return (now.ss_flags & SS_DISABLE) != 0;
   }));

   // The prior disposition is restored, so the signal reaches the application's
   // own handler again and never the interceptor.
   CHECK(in_child([] {
      struct sigaction sa;
      std::memset(&sa, 0, sizeof sa);
      sa.sa_handler = prior_handler;
      sigaction(SIGUSR1, &sa, nullptr);

      sigctx_intercept_cfg cfg = uninstall_cfg();
      if (sigctx_intercept_install(&cfg) != 0) return false;
      if (sigctx_intercept_uninstall() != 0) return false;

      g_prior_handler_ran = 0;
      g_interceptor_ran = 0;
      raise(SIGUSR1);
      return g_prior_handler_ran == 1 && g_interceptor_ran == 0;
   }));

   // Two threads installed: the first to leave must NOT take the disposition
   // away from the one still intercepting. Only the last restores it.
   CHECK(in_child([] {
      struct sigaction sa;
      std::memset(&sa, 0, sizeof sa);
      sa.sa_handler = prior_handler;
      sigaction(SIGUSR1, &sa, nullptr);

      sigctx_intercept_cfg cfg = uninstall_cfg();
      if (sigctx_intercept_install(&cfg) != 0) return false;

      // A second thread installs and uninstalls on its own stacks.
      std::thread other([] {
         alignas(64) static std::uint8_t h[128 * 1024];
         alignas(64) static std::uint8_t a[256 * 1024];
         sigctx_intercept_cfg c = uninstall_cfg();
         c.handler_sp = h; c.handler_ss = sizeof h;
         c.altstack_sp = a; c.altstack_ss = sizeof a;
         sigctx_intercept_install(&c);
         sigctx_intercept_uninstall();
      });
      other.join();

      g_interceptor_ran = 0;
      raise(SIGUSR1);                                    // still intercepted here
      bool const kept = g_interceptor_ran == 1;

      if (sigctx_intercept_uninstall() != 0) return false;
      g_prior_handler_ran = 0;
      raise(SIGUSR1);                                    // and now the prior handler
      return kept && g_prior_handler_ran == 1;
   }));

   // A thread that has uninstalled while ANOTHER still holds the disposition
   // must ignore the signal, not run the interceptor on stacks its caller may
   // already have freed. That is what clearing the thread's config is for.
   CHECK(in_child([] {
      sigctx_intercept_cfg cfg = uninstall_cfg();
      if (sigctx_intercept_install(&cfg) != 0) return false;

      std::atomic<int> stage{0};
      std::thread holder([&stage] {
         alignas(64) static std::uint8_t h[128 * 1024];
         alignas(64) static std::uint8_t a[256 * 1024];
         sigctx_intercept_cfg c = uninstall_cfg();
         c.handler_sp = h; c.handler_ss = sizeof h;
         c.altstack_sp = a; c.altstack_ss = sizeof a;
         sigctx_intercept_install(&c);
         stage.store(1);
         while (stage.load() != 2) { }                   // hold it while main tests
         sigctx_intercept_uninstall();
      });
      while (stage.load() != 1) { }

      if (sigctx_intercept_uninstall() != 0) return false;
      g_interceptor_ran = 0;
      raise(SIGUSR1);                                    // disposition is still sigctx's
      bool const ignored = g_interceptor_ran == 0;

      stage.store(2);
      holder.join();
      return ignored;
   }));

   // Uninstalling from a handler that is running on the altstack cannot restore
   // the altstack underneath itself, so it refuses and changes nothing.
   CHECK(in_child([] {
      static volatile int rc = 1;
      struct sigaction sa;
      std::memset(&sa, 0, sizeof sa);
      sa.sa_handler = [](int) { rc = sigctx_intercept_uninstall(); };
      sa.sa_flags = SA_ONSTACK;
      sigaction(SIGUSR2, &sa, nullptr);

      sigctx_intercept_cfg cfg = uninstall_cfg();
      if (sigctx_intercept_install(&cfg) != 0) return false;
      raise(SIGUSR2);                                    // runs on the altstack

      g_interceptor_ran = 0;
      raise(SIGUSR1);                                    // still installed
      return rc == -EPERM && g_interceptor_ran == 1;
   }));
}

int main()
{
   test_create_basic();
   test_create_runs_entry();
   test_copy_faithful();
   test_copy_demote();
   test_fpstate_size();
   test_precondition_aborts();
   test_dyn_create();
   test_dyn_copy_faithful();
   test_dyn_holds_full_extended();
   run_vector_survival();
   run_block_extra();
   test_altstack_sizing();
   test_install_size_rejects();
   test_altstack_nesting();
   test_uninstall();

   std::printf("\n%d checks, %d failures\n", g_checks, g_fails);
   return g_fails ? 1 : 0;
}
