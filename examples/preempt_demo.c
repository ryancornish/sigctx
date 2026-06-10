/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Ryan Cornish
 *
 * Example: cooperative switching between a worker and main via sigctx_intercept.
 *
 * Reference and policy, not part of the library. It picks SIGUSR1, a round-robin
 * handler, and self-signals to yield. The library supplies only the mechanism.
 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <sigctx/sigctx.h>
#include <sigctx/sigctx_intercept.h>

/* Generous fixed handler stack, large enough for the runtime XSAVE area on AVX-512
 * machines too. install returns -ERANGE if it is somehow still too small. */
static uint8_t handler_stack[32 * 1024];

struct thread
{
   sigctx_inl_t context;
   uint8_t      stack[64 * 1024];
};

static struct thread thread_a;
static struct thread thread_main;
static int turn = 0;

static void yield_now(void)
{
   if (pthread_kill(pthread_self(), SIGUSR1) != 0) {
      perror("pthread_kill");
      exit(EXIT_FAILURE);
   }
}

static void thread_a_entry(void* arg)
{
   for (int i = 0; i < 3; ++i) {
      printf("  [A] tick %d (arg=%lu)\n", i, (unsigned long)(uintptr_t)arg);
      yield_now();
   }
   printf("  [A] done, yielding forever\n");
   for (;;) {
      yield_now(); /* example never tears A down (a real scheduler would) */
   }
}

static sigctx_ucontext_t* handler(sigctx_ucontext_t* paused, void* arg)
{
   (void)arg;
   if (turn++ % 2 == 0) {
      printf("[handler] -> A\n");
      sigctx_copy(&thread_main.context.uc,
                  thread_main.context.fpstate, sizeof thread_main.context.fpstate,
                  paused);
      return &thread_a.context.uc;
   } else {
      printf("[handler] -> main\n");
      sigctx_copy(&thread_a.context.uc,
                  thread_a.context.fpstate, sizeof thread_a.context.fpstate,
                  paused);
      return &thread_main.context.uc;
   }
}

int main(void)
{
   sigctx_create(&thread_a.context.uc,
                 thread_a.context.fpstate, sizeof thread_a.context.fpstate,
                 thread_a.stack, sizeof thread_a.stack,
                 thread_a_entry, (void*)51);

   sigctx_intercept_cfg cfg = {
      .signo      = SIGUSR1,
      .handler_sp = handler_stack,
      .handler_ss = sizeof handler_stack,
      .handler    = handler,
      .arg        = NULL,
   };
   int rc = sigctx_intercept_install(&cfg);
   if (rc != 0) {
      printf("install failed: %d\n", rc);
      return 1;
   }

   printf("[main] starting, will interleave with A\n");
   for (int i = 0; i < 4; ++i) {
      printf("[main] step %d\n", i);
      yield_now();
   }
   printf("[main] exit\n");
   return 0;
}
