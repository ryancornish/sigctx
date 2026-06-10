/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Ryan Cornish
 *
 * sigctx - synthesize and resume Linux rt_sigreturn-compatible CPU contexts.
 *
 * Builds signal-frame-shaped contexts that the kernel will accept on rt_sigreturn,
 * for cooperative or preemptive userspace thread switching on x86-64 Linux. The
 * bare API (sigctx_create / sigctx_copy) keeps the FP buffer external and
 * caller-owned, which is the flexible primitive. Two wrapper types bundle a context
 * with its FP backing for the common cases:
 *
 *   sigctx_inl_t  fixed-capacity, FP storage inline. Constant sizeof, no
 *                 allocation - lay it out directly inside a TCB. The capacity is
 *                 a compile-time BOUND, not the live frame size.
 *   sigctx_dyn_t  heap-backed FP, sized at runtime via sigctx_fpstate_size().
 *                 For AMX or other large/variable state, or to avoid paying the
 *                 inline bound when only legacy state is used.
 *
 * Minimal usage (inline):
 *   sigctx_inl_t c;
 *   sigctx_create(&c.uc, c.fpstate, sizeof c.fpstate, stk, sizeof stk, entry, arg);
 *   // ... later, from a signal handler, resume by pointing rt_sigreturn at &c.uc.
 *
 * Contract notes that apply throughout:
 *   - An FP buffer passed to any function must be 64-byte aligned and live at least
 *     as long as the context that points into it.
 *   - Always relocate FP state with sigctx_copy. A raw struct assignment copies only
 *     the fpstate pointer. sigctx_rebind backstops the inline case if you must.
 *   - x86-64 Linux only, glibc >= 2.34. Requires gcc or clang with GNU C extensions
 *     (the -std=gnu11 through -std=gnu23 dialects). Compiles as C or C++.
 */
#ifndef SIGCTX_H
#define SIGCTX_H

#include <stddef.h>
#include <stdint.h>

#include <sigctx/sigctx_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIGCTX_VERSION_MAJOR 1
#define SIGCTX_VERSION_MINOR 0
#define SIGCTX_VERSION_PATCH 0

/*
 * Result of sigctx_copy. A faithful copy is SIGCTX_OK. When the source carries
 * extended (XSAVE) FP state that the destination buffer cannot hold, the frame is
 * demoted to a valid legacy frame and SIGCTX_TRUNCATED_TO_LEGACY is returned.
 *
 * Misuse of the API (null pointers, a misaligned FP buffer, a buffer below the
 * legacy floor) is a caller bug, not a runtime condition, so it is checked with
 * assert() rather than a return code. Build with NDEBUG to compile those checks
 * out, in which case misuse is undefined behaviour.
 */
typedef enum
{
   SIGCTX_OK                  = 0,
   SIGCTX_TRUNCATED_TO_LEGACY = 1
} sigctx_status;

/*
 * Inline FP capacity for sigctx_inl_t. A compile-time BOUND on the FP area, not
 * the live frame size. It MUST be >= sigctx_fpstate_size() on the target machine, or
 * sigctx_intercept_install returns -ERANGE rather than silently dropping vector
 * state. The default below suits SSE/AVX/AVX-512 on typical configurations, but the
 * enabled XSAVE area is machine-dependent and can be much larger (e.g. ~11 KB with a
 * wide AVX-512 component set, or +8 KB again under AMX). If sigctx_fpstate_size()
 * exceeds this on your target, either raise SIGCTX_FPSTATE_CAPACITY to cover it or
 * use sigctx_dyn_t, whose buffer is sized at runtime. Override before including.
 */
#ifndef SIGCTX_FPSTATE_CAPACITY
#define SIGCTX_FPSTATE_CAPACITY 3072u
#endif

#define SIGCTX_FPSTATE_ALIGN 64u

/* Fixed-capacity context with FP storage inline. sizeof is a compile-time constant,
 * which is what makes it suitable for placing inline inside a TCB without malloc. */
typedef struct
{
   sigctx_ucontext_t uc;
   __attribute__((aligned(SIGCTX_FPSTATE_ALIGN))) uint8_t fpstate[SIGCTX_FPSTATE_CAPACITY];
} sigctx_inl_t;

/*
 * Heap-backed layout convenience. sigctx does not allocate or free this buffer.
 * Callers choose their allocator and lifetime policy.
 * fpstate block must be aligned to SIGCTX_FPSTATE_ALIGN.
 */
typedef struct
{
   sigctx_ucontext_t uc;
   uint8_t*          fpstate;
   size_t            fpstate_size;
} sigctx_dyn_t;

/*
 * Synthesize a fresh, resumable context that will enter entry(entry_arg) on the
 * given stack. Synthesizes a legacy FP frame only. Extended (XSAVE) state is
 * preserved across capture, not creation.
 *
 * Preconditions, all asserted: out, fpstate, stack_base, and entry are non-null,
 * fpstate is 64-byte aligned and at least sizeof(struct sigctx_fpstate) bytes (the
 * legacy floor), and stack_size is at least the minimum usable stack. Violating
 * any of these aborts via assert() unless built with NDEBUG.
 *
 * entry must not return. As a debugging containment measure, sigctx installs a trap
 * return address, so a returning entry is expected to terminate the process rather
 * than continue into arbitrary memory. Callers must not rely on this as normal control flow.
 * entry_arg is an optional caller-supplied argument passed as entry's first parameter.
 */
void sigctx_create(sigctx_ucontext_t* out,
                   void* fpstate, size_t fpstate_size,
                   void* stack_base, size_t stack_size,
                   void (*entry)(void*), void* entry_arg);

/*
 * Deep-copy src into dst, relocating the FP bytes into dst's own aligned buffer and
 * repointing dst->uc_mcontext.fpstate at it. A plain struct assignment copies only
 * the pointer, so this is the correct primitive for moving a captured frame into a
 * TCB. Returns SIGCTX_OK on a faithful copy. If src carries extended state that
 * dst_fpstate cannot hold, the frame is demoted to a valid legacy frame (the wide
 * vector bits are dropped, the result is still resumable) and the return is
 * SIGCTX_TRUNCATED_TO_LEGACY.
 *
 * Preconditions, all asserted: dst and src are non-null, and when src actually
 * carries FP state, dst_fpstate is non-null, 64-byte aligned, and at least the
 * legacy floor. Violations abort via assert() unless built with NDEBUG.
 */
sigctx_status sigctx_copy(sigctx_ucontext_t* dst,
                          void* dst_fpstate, size_t dst_fpstate_size,
                          sigctx_ucontext_t const* src);

/*
 * Resume a context: atomically reload the CPU from *ctx and unmask whatever its
 * uc_sigmask permits, via rt_sigreturn. Does not return. This is the counterpart to
 * sigctx_create/sigctx_copy and is how a handler hands control to the chosen context.
 * ctx must outlive the call (the kernel reads it after this function's frame is gone).
 */
__attribute__((noreturn))
void sigctx_resume(sigctx_ucontext_t const* ctx);

/*
 * Byte size of the FP/XSAVE area the kernel will produce for THIS process's enabled
 * XCR0 (CPUID leaf 0x0D sub-leaf 0, EBX). Use it to size a dynamic buffer and to
 * check against SIGCTX_FPSTATE_CAPACITY at startup. Fixed for the life of the
 * process but NOT a compile-time constant. Returns 0 if XSAVE is unsupported.
 */
uint32_t sigctx_fpstate_size(void);

/*
 * Point an inline context's fpstate at its OWN buffer. Needed only if you bypass
 * sigctx_copy and raw-assign the struct, which duplicates the pointer rather than
 * the self-relationship.
 */
void sigctx_rebind(sigctx_inl_t* c);

#ifdef __cplusplus
}
#endif

#endif /* SIGCTX_H */
