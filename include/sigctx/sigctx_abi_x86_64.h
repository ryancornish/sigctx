/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Layouts transcribed from Linux UAPI (see banner below) - transcription (C) 2026 Ryan Cornish
 *
 * sigctx_abi_x86_64.h
 *
 * Vendored mirror of the Linux x86-64 signal-frame ABI - just enough layout to
 * build and resume rt_sigreturn-compatible contexts. Uses sigctx_-prefixed names
 * so it coexists with glibc's <signal.h>/<ucontext.h> (unlike <asm/sigcontext.h>,
 * which collides with glibc's signal types).
 *
 * The struct layouts below are transcribed verbatim from the kernel UAPI headers:
 *   struct sigcontext -> arch/x86/include/uapi/asm/sigcontext.h  (x86_64 branch)
 *   struct ucontext   -> include/uapi/asm-generic/ucontext.h
 * Those headers carry "GPL-2.0 WITH Linux-syscall-note" - this file inherits that
 * licence because it reproduces those definitions. The syscall-note exception is
 * what permits userspace to use them. Original sigctx code (everything outside this
 * file) is LGPL-2.1-or-later.
 *
 * The static_asserts pin every load-bearing offset to the documented ABI values,
 * so a transcription slip fails the build rather than producing a garbage frame.
 */
#ifndef SIGCTX_ABI_X86_64_H
#define SIGCTX_ABI_X86_64_H

#if !defined(__x86_64__) || !defined(__linux__)
#  error "sigctx_abi_x86_64.h targets x86-64 Linux only"
#endif

#include <stddef.h>   /* offsetof */
#include <stdint.h>
#include <signal.h>   /* stack_t, sigset_t (glibc's - ABI-compatible) */
#include <assert.h>

/* Bumped only on an incompatible change to the structs/contract below. */
#define SIGCTX_ABI_VERSION 1

/* --- Saved CPU register file (mirrors x86_64 struct sigcontext) ------------ */
/* GPR order IS the ABI. rt_sigreturn reloads rip/rsp/rdi/eflags/cs/ss verbatim
 * with no cross-validation, which is why patching them is safe. fpstate points at
 * the FP/XSAVE area (16- or 64-byte aligned). err/trapno/oldmask/cr2 are fault
 * reporting fields, ignored on the restore path. reserved is tail growth space. */
struct sigctx_sigcontext
{
   uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
   uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip;
   uint64_t eflags;
   uint16_t cs;
   uint16_t gs;   /* vestigial on x86_64 (TLS via FS_BASE/GS_BASE MSRs) */
   uint16_t fs;   /* vestigial on x86_64 */
   uint16_t ss;
   uint64_t err;
   uint64_t trapno;
   uint64_t oldmask;
   uint64_t cr2;
   struct sigctx_fpstate* fpstate; /* == glibc uc_mcontext.fpregs */
   uint64_t reserved[8];
};

/* --- The wrapper rt_sigreturn actually consumes (mirrors struct ucontext) -- */
/* uc_flags carries SIGCTX_UC_FP_XSTATE when extended FP is advertised. uc_link is
 * unused by rt_sigreturn. uc_stack is informational. uc_sigmask is the mask
 * installed on resume (kernel reads only its low 8 bytes - glibc sigset_t is wider,
 * harmlessly). */
struct sigctx_ucontext
{
   uint64_t                 uc_flags;
   struct sigctx_ucontext*  uc_link;
   stack_t                  uc_stack;
   struct sigctx_sigcontext uc_mcontext;
   sigset_t                 uc_sigmask;
};
typedef struct sigctx_ucontext sigctx_ucontext_t;

/* --- FP scaffolding -------------------------------------------------------- */
/* sw_reserved sits in the tail of the 512-byte legacy FXSAVE area, at +464. */
struct sigctx_fpx_sw_bytes
{
   uint32_t magic1;        /* SIGCTX_FP_XSTATE_MAGIC1 when extended state attached */
   uint32_t extended_size; /* total bytes of the FP region from fpstate onward */
   uint64_t xstate_bv;
   uint32_t xstate_size;
   uint32_t padding[7];
};

/* Legacy 512-byte FXSAVE area. For extended state, the XSAVE components follow
 * this struct in memory and sw_reserved describes the full extent. */
struct sigctx_fpstate
{
   uint16_t cwd, swd, twd, fop;
   uint64_t rip, rdp;
   uint32_t mxcsr, mxcr_mask;
   uint32_t st_space[32];   /* 8 x87/MMX registers, 16 bytes each */
   uint32_t xmm_space[64];  /* 16 XMM registers, 16 bytes each */
   uint32_t reserved[12];   /* padding up to the sw_reserved trailer */
   struct sigctx_fpx_sw_bytes sw_reserved; /* at offset 464 */
};

#define SIGCTX_FP_XSTATE_MAGIC1       0x46505853u
#define SIGCTX_FP_XSTATE_MAGIC2       0x46505845u
#define SIGCTX_FP_XSTATE_MAGIC2_SIZE  4u
#define SIGCTX_FPX_SW_RESERVED_OFFSET 464u
#define SIGCTX_UC_FP_XSTATE           0x1u  /* uc_flags bit: extended FP present */

/* --- Offset pins against the documented x86_64 ABI ------------------------- */
static_assert(sizeof(struct sigctx_sigcontext) == 256,         "sigcontext size");
static_assert(offsetof(struct sigctx_sigcontext, rip)     == 128, "rip @128");
static_assert(offsetof(struct sigctx_sigcontext, eflags)  == 136, "eflags @136");
static_assert(offsetof(struct sigctx_sigcontext, cs)      == 144, "cs @144");
static_assert(offsetof(struct sigctx_sigcontext, fpstate) == 184, "fpstate @184");
static_assert(offsetof(struct sigctx_ucontext, uc_stack)    == 16, "uc_stack @16");
static_assert(offsetof(struct sigctx_ucontext, uc_mcontext) == 40, "uc_mcontext @40");
static_assert(sizeof(struct sigctx_fpx_sw_bytes) == 48,            "sw_bytes size");
static_assert(sizeof(struct sigctx_fpstate) == 512,               "fpstate size");
static_assert(offsetof(struct sigctx_fpstate, sw_reserved) == 464, "sw_reserved @464");

#endif /* SIGCTX_ABI_X86_64_H */
