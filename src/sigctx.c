/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Ryan Cornish
 *
 * sigctx - implementation. Synthesis and relocation of rt_sigreturn-compatible
 * contexts over the vendored sigctx_abi layout.
 */

#include <sigctx/sigctx.h>

#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>

#define MIN_USER_STACK_SIZE 128

static __attribute__((noreturn)) void sigctx_entry_returned(void)
{
   __builtin_trap();
}

static __attribute__((unused)) int sigctx_is_aligned(void const* p, size_t align)
{
   return ((uintptr_t)p & (align - 1u)) == 0u;
}

void sigctx_create(sigctx_ucontext_t* out,
                   void* fpstate, size_t fpstate_size,
                   void* stack_base, size_t stack_size,
                   void (*entry)(void*), void* entry_arg)
{
   /* Preconditions are the caller's contract, not runtime conditions, so they
    * assert rather than return a code. Build with NDEBUG to strip these. */
   assert(out && fpstate && stack_base && entry);
   assert(sigctx_is_aligned(fpstate, SIGCTX_FPSTATE_ALIGN));
   assert(fpstate_size >= sizeof(struct sigctx_fpstate));
   assert(stack_size >= MIN_USER_STACK_SIZE);
   assert((uintptr_t)stack_base + stack_size >= (uintptr_t)stack_base); /* no wrap */

   memset(out, 0, sizeof *out);

   /* Stack top aligned so entry sees RSP == 8 (mod 16). We arrive via a jump
    * (sigreturn) with no pushed return address, so we fake the post-call residue:
    * align DOWN to 16 first, THEN subtract 8. Fallback to a stub return point if entry returns. */
   uintptr_t top = ((uintptr_t)stack_base + stack_size) & ~(uintptr_t)0xF;
   top -= 8;
   *(uintptr_t*)top = (uintptr_t)sigctx_entry_returned;

   out->uc_mcontext.rsp    = top;
   out->uc_mcontext.rip    = (uint64_t)entry;
   out->uc_mcontext.rdi    = (uint64_t)entry_arg;
   out->uc_mcontext.eflags = 0x202; /* reserved bit1 set, IF set, DF clear */

   /* rt_sigreturn reloads CS and SS, where setcontext ignores them. A zeroed CS is
    * selector 3 (invalid) and the kernel faults returning to user mode. Capture the
    * live selectors. FS/GS stay zero since 64-bit TLS uses the FS_BASE MSR. */
   uint16_t cs, ss;
   __asm__ volatile("mov %%cs, %0" : "=rm"(cs));
   __asm__ volatile("mov %%ss, %0" : "=rm"(ss));
   out->uc_mcontext.cs = cs;
   out->uc_mcontext.ss = ss;

   /* FP state in the caller's 64-byte-aligned buffer. The kernel restores the whole
    * FP file with FXRSTOR/XRSTOR, which #GP on a misaligned operand, so this can
    * never be a glibc ucontext_t's 8-mod-16 embedded __fpregs_mem. */
   memset(fpstate, 0, fpstate_size);
   struct sigctx_fpstate* fx = (struct sigctx_fpstate*)fpstate;
   fx->cwd   = 0x037f; /* default x87 control word */
   fx->mxcsr = 0x1f80; /* default SSE control/status */
   out->uc_mcontext.fpstate = fx;

   /* Legacy FP frame only: uc_flags stays 0 and sw_reserved.magic1 stays 0, so the
    * kernel does a plain FXRSTOR. Extended synthesis would set magic1, append the
    * XSAVE area, write MAGIC2 at the tail, set extended_size, and OR in
    * SIGCTX_UC_FP_XSTATE. */
   sigemptyset(&out->uc_sigmask);
}

sigctx_status sigctx_copy(sigctx_ucontext_t* dst,
                          void* dst_fpstate, size_t dst_fpstate_size,
                          sigctx_ucontext_t const* src)
{
   assert(dst && src);

   if (!src->uc_mcontext.fpstate) {
      *dst = *src; /* nothing to relocate */
      return SIGCTX_OK;
   }

   /* src carries FP state, so the destination buffer must be real and well-formed.
    * These are caller-contract checks, so they assert (stripped under NDEBUG). */
   assert(dst_fpstate);
   assert(((uintptr_t)dst_fpstate & (SIGCTX_FPSTATE_ALIGN - 1u)) == 0u);
   assert(dst_fpstate_size >= sizeof(struct sigctx_fpstate));

   struct sigctx_fpstate const* sfp = src->uc_mcontext.fpstate;
   int const has_extended = (sfp->sw_reserved.magic1 == SIGCTX_FP_XSTATE_MAGIC1);
   assert(!has_extended || sfp->sw_reserved.extended_size >= sizeof(struct sigctx_fpstate));

   *dst = *src;

   size_t const needed = has_extended ? sfp->sw_reserved.extended_size : sizeof(struct sigctx_fpstate);

   sigctx_status status = SIGCTX_OK;
   if (needed <= dst_fpstate_size) {
      memcpy(dst_fpstate, sfp, needed);
   } else {
      /* Cannot hold the full extended frame. Copy the legacy floor and clear the
       * extended markers so the kernel does a bare FXRSTOR instead of reading past
       * our buffer and rejecting the frame. The wide vector bits are lost. */
      memcpy(dst_fpstate, sfp, sizeof(struct sigctx_fpstate));
      ((struct sigctx_fpstate*)dst_fpstate)->sw_reserved.magic1 = 0;
      dst->uc_flags &= ~(uint64_t)SIGCTX_UC_FP_XSTATE;
      status = SIGCTX_TRUNCATED_TO_LEGACY;
   }
   dst->uc_mcontext.fpstate = (struct sigctx_fpstate*)dst_fpstate;
   return status;
}

static void sigctx_cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t* a, uint32_t* b,
                         uint32_t* c, uint32_t* d)
{
   __asm__ volatile("cpuid"
                    : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                    : "a"(leaf), "c"(subleaf));
}

uint32_t sigctx_fpstate_size(void)
{
   uint32_t a, b, c, d;

   sigctx_cpuid(0, 0, &a, &b, &c, &d);
   if (a < 0x0Du) {
      return 0;
   }

   sigctx_cpuid(1, 0, &a, &b, &c, &d);
   if ((c & (1u << 26)) == 0) { /* XSAVE */
      return 0;
   }
   if ((c & (1u << 27)) == 0) { /* OSXSAVE */
      return 0;
   }

   sigctx_cpuid(0x0D, 0, &a, &b, &c, &d);
   return b;
}

void sigctx_rebind(sigctx_inl_t* c)
{
   c->uc.uc_mcontext.fpstate = (struct sigctx_fpstate*)c->fpstate;
}

__attribute__((naked, noreturn))
void sigctx_resume(sigctx_ucontext_t const* ctx __attribute__((unused)))
{
   /* rt_sigreturn rebuilds the full CPU from an rt_sigframe read off RSP. The frame's
    * ucontext sits 8 bytes in, so pointing RSP straight at ctx makes the kernel's
    * (rsp - 8) + 8 == rsp arithmetic land on it. ctx is already in RDI per the ABI.
    * Naked: no prologue, the parameter is consumed only through RDI. */
   __asm__ volatile(
      "mov %rdi, %rsp\n\t"
      "mov $15, %rax\n\t"
      "syscall\n\t"
   );
}
