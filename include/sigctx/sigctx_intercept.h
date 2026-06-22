/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Ryan Cornish
 *
 * sigctx_intercept - hand control to user code when a chosen signal lands.
 *
 * Installs a handler for a caller-chosen signal. When the signal arrives, it captures
 * the interrupted context (registers + FP) and calls a caller-supplied 'handler'
 * callback. The handler is given the just-paused context and returns a context to run.
 * The interceptor then resumes whichever context the handler returned.
 *
 * The caller supplies everything through the config: the signal number, the stack the
 * handler runs on (handler_sp, handler_ss), the handler callback, and an opaque user
 * pointer passed through to it. The API makes no assumption about what the handler does
 * with the paused context. Returning a different context turns this into a context
 * switch, which is the common use, but returning the same context makes it a pure
 * inspection point, and anything in between is the caller's policy. Scheduling order,
 * how contexts are created, and whether this is a scheduler at all are not decided
 * here.
 *
 * The handler runs as ordinary code on its own stack with the trigger signal masked, so
 * it is free of async-signal-safety constraints and cannot be re-entered.
 *
 * The trigger signal is held blocked for the whole interception. block_extra extends
 * that to a caller-chosen set of additional signals, for a caller with a separate timer
 * or IPI signal that must not nest into the capture or the handler.
 *
 * Per-OS-thread: sigaltstack is per-OS-thread, so call sigctx_intercept_install once on
 * each OS thread that should be interceptible. The sigaction itself is process-wide.
 * Requires gcc or clang with GNU C extensions (the -std=gnu11 through -std=gnu23
 * dialects). x86-64 Linux, glibc >= 2.34. Compiles as C or C++.
 */
#ifndef SIGCTX_INTERCEPT_H
#define SIGCTX_INTERCEPT_H

#include <stddef.h>
#include <stdint.h>
#include <signal.h>

#include <sigctx/sigctx.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called with the captured context of whatever was running when the signal landed.
 * Returns the context to resume, which must be non-null and must outlive the resume.
 * arg is the opaque pointer supplied in the config, passed through unchanged.
 */
typedef sigctx_ucontext_t* (*sigctx_handler_fn)(sigctx_ucontext_t* paused, void* arg);

typedef struct
{
   int               signo;       /* signal that triggers a switch, e.g. SIGURG/SIGUSR1 */
   uint8_t*          handler_sp;  /* base of the stack the handler and capture run on */
   size_t            handler_ss;  /* its size, see SIGCTX_INTERCEPT_MIN_FRAME below */
   sigctx_handler_fn handler;     /* picks the context to resume */
   void*             arg;         /* opaque, passed through to handler */
   sigset_t const*   block_extra; /* optional extra signals to hold blocked for the duration of an interception. NULL for none. */
} sigctx_intercept_cfg;

/*
 * Conservative compile-time lower bound for handler_ss, derived from the inline FP
 * capacity. Use it to size a handler stack at compile time. The authoritative check
 * is at runtime inside sigctx_intercept_install, which measures this machine's actual
 * XSAVE extent with sigctx_fpstate_size() and returns -ERANGE if handler_ss is too
 * small. Whenever SIGCTX_FPSTATE_CAPACITY covers the runtime size, a stack sized by
 * this macro clears that check with room to spare.
 */
#define SIGCTX_INTERCEPT_MIN_FRAME (sizeof(sigctx_ucontext_t) + SIGCTX_FPSTATE_CAPACITY + 4096u)

/*
 * Install the interceptor on the calling thread. A malformed config (null cfg,
 * null handler, null handler_sp, or signo <= 0) is a caller bug and asserts.
 * Returns 0 on success, or a negative errno for a genuine runtime condition:
 * -ENOMEM if this CPU's signal frame exceeds the internal altstack, -ERANGE if
 * handler_ss is too small for the runtime FP area, or a sigaltstack or sigaction
 * errno. The caller decides how to handle failure.
 */
int sigctx_intercept_install(sigctx_intercept_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif /* SIGCTX_INTERCEPT_H */
