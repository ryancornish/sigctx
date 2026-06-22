# sigctx

Synthesize and resume Linux `rt_sigreturn`-compatible CPU contexts from scratch,
without `makecontext` or `setcontext`. It builds a signal-frame-shaped context that
the kernel accepts on `rt_sigreturn`, which is the primitive underneath a cooperative
or preemptive userspace thread scheduler. It targets **x86-64 Linux only**, needs
glibc 2.34 or newer, and requires gcc or clang with GNU C extensions, that is the
`-std=gnu11` through `-std=gnu23` dialects. It compiles as either C or C++.

## Why

A `makecontext` context is restored by `setcontext`, which reloads only callee-saved
state and ignores the segment registers and the full FP file. A context interrupted
at an arbitrary instruction, which is what a preemption is, needs everything
restored. That is exactly what `rt_sigreturn` does. The catch is that `rt_sigreturn`
demands a kernel-shaped frame with a valid CS selector, a 64-byte-aligned
XSAVE-capable FP area, and the right magic markers. `sigctx` builds that frame, so a
single `rt_sigreturn` path resumes both freshly created and preempted threads through
one uniform mechanism.

This is low-level kernel-ABI code. You are hand-constructing signal frames. It stays
safe because it only writes the fields the kernel reloads without cross-validation,
namely RIP, RSP, RDI, EFLAGS, CS, SS, and the signal mask, and it mirrors the
kernel's frozen sigframe layout for everything else. See the banner in
`sigctx_abi_x86_64.h` for the provenance of those layouts.

## Design, and how it relates to prior art

I built this from the `rt_sigreturn` ABI up, by working out what the kernel needs in a
frame and then constructing one that satisfies it. The key choice is to let the kernel
do the full save and restore, and to keep the scheduler out of the signal handler. The
handler does the minimum. It captures the interrupted frame, then rewrites the saved
RIP, RSP, and RDI so that returning from it lands on a trampoline on a private stack.
The scheduler then runs as ordinary code with no async-signal-safety constraints, and
a resume is a direct `rt_sigreturn` against whichever frame it picks. One path resumes
a freshly created thread and a preempted one, because to the kernel both are just
frames to load.

The same ideas turn up in a few other places, which I found only after the fact.

Go's runtime makes the same core move for asynchronous preemption. Its handler edits
the saved context so the thread resumes into scheduler code, and its design notes
mention letting the kernel save and restore registers as the alternative to spilling
them by hand in the handler. That alternative is the path here, using `rt_sigreturn`
for the resume. The security world's Sigreturn-Oriented Programming builds the same
kind of fake frame and calls `rt_sigreturn` to load every register at once, which is
good proof the kernel behaviour is dependable, aimed at a benign end here. glibc has
gone the other way and moved `setcontext` off `rt_sigreturn` toward restoring
registers in user code, keeping the syscall for contexts that really came from a
signal. This library uses `rt_sigreturn` for every resume on purpose, so one
full-state path covers both fresh creation and preemption.

Where I think this library shines are the choices around all that. A hand-built
`rt_sigreturn` is treated as a documented resume API, as opposed to an exploit gadget
or a runtime's private detail. The trampoline onto a clean stack keeps the scheduler
in ordinary code, which is what avoids the async-safety problems that bite the usual
`setcontext` and `longjmp`-from-handler libraries. Leveraging `rt_sigreturn` as a
means to atomically resume a context **and** unmask the signal is key.

## Files and licensing

| File | Role | Licence |
|------|------|---------|
| `include/sigctx/sigctx_abi.h` | architecture dispatch | LGPL-2.1+ |
| `include/sigctx/sigctx_abi_x86_64.h` | vendored kernel sigframe layout for x86-64 | GPL-2.0 WITH Linux-syscall-note |
| `include/sigctx/sigctx.h`, `src/sigctx.c` | create, copy, resume, FP sizing | LGPL-2.1+ |
| `include/sigctx/sigctx_intercept.h`, `src/sigctx_intercept.c` | turn a signal into a context switch | LGPL-2.1+ |
| `examples/` | runnable reference, this is policy not API | LGPL-2.1+ |
| `tests/` | unit and integration tests | LGPL-2.1+ |

The project is LGPL-2.1-or-later, with one exception. `sigctx_abi_x86_64.h` inherits
`GPL-2.0 WITH Linux-syscall-note` from the kernel UAPI headers whose struct layouts
it transcribes. The Linux-syscall-note exception is what lets userspace use those
layouts, so it places no GPL obligation on code that merely uses the library.
Per-file `SPDX-License-Identifier` tags are authoritative. The full licence texts
live in `COPYING` (GPL-2.0) and `COPYING.LESSER` (LGPL-2.1). Both ship together
because LGPL-2.1 is defined as additional permissions layered on GPL-2.0. See
`NOTICE` for a short summary of the split.

## Building and installing

sigctx builds with CMake and installs as a normal system library.

```
cmake -B build
cmake --build build
cmake --install build            # add --prefix to choose a location
```

That installs the shared library, the headers under `include/sigctx/`, a pkg-config
file, and a CMake package config, so a consumer can find sigctx with either tool.

To build and run the tests, or build the example, enable the matching option. They
are off by default so a packaging build compiles only the library.

```
cmake -B build -DSIGCTX_BUILD_TESTS=ON -DSIGCTX_BUILD_EXAMPLES=ON
cmake --build build
ctest --test-dir build
./build/preempt_demo
```

A `debian/` directory is included for building `.deb` packages on Debian and Ubuntu
with `dpkg-buildpackage`, producing a `libsigctx1` runtime package and a
`libsigctx-dev` package.

## Using it from another project

After installing, with CMake:

```cmake
find_package(sigctx REQUIRED)
target_link_libraries(your_target PRIVATE sigctx::sigctx)
```

Or with pkg-config:

```
cc your.c $(pkg-config --cflags --libs sigctx)
```

Or vendor the tree and pull it in directly with `add_subdirectory(sigctx)`, then link
`sigctx::sigctx` as above.

A minimal sketch of the API in use:

```c
#include <sigctx/sigctx.h>
#include <sigctx/sigctx_intercept.h>

sigctx_inl_t a;                       /* a context bundled with an inline FP buffer */
sigctx_create(&a.uc, a.fpstate, sizeof a.fpstate,
              stack, sizeof stack, entry, arg);

sigctx_intercept_cfg cfg = {
    .signo = SIGUSR1, .handler_sp = handler_stack, .handler_ss = sizeof handler_stack,
    .handler = pick_next, .arg = sched,
};
sigctx_intercept_install(&cfg);          /* returns 0 or a negative errno */
```

The handler callback receives the captured context of whatever was running when the
signal arrived. It snapshots that context with `sigctx_copy` and returns the context
to resume. The interceptor then resumes it with `sigctx_resume`. See
`examples/preempt_demo.c` for a complete round trip between a worker and main.

The trigger signal is held blocked for the whole interception. The optional
`block_extra` field in the config holds an additional set of signals blocked over the
same window, which a caller with a separate timer or IPI signal needs so those cannot
nest into the capture or the handler. See the header for the details.

### Preconditions and return values

Misuse of the API, meaning null pointers or a misaligned or undersized FP buffer, is a
caller bug rather than a runtime condition, so it is checked with `assert()` rather
than a return code. As a result `sigctx_create` returns nothing. `sigctx_copy` returns
a `sigctx_status`, which is `SIGCTX_OK`, or `SIGCTX_TRUNCATED_TO_LEGACY` when the
destination FP buffer cannot hold captured extended state and the frame is demoted to
a valid legacy frame. Only `sigctx_intercept_install` returns an `int`, zero on success
or a negative errno for a genuine runtime or OS condition such as a handler stack too
small for this machine's FP area. Building with `NDEBUG` compiles the assertions out,
in which case misuse becomes undefined behaviour.

### Inline versus dynamic contexts

The core functions take a context plus a `(buffer, size)` pair for the FP area and do
not care where that buffer lives. Two wrapper types build on that.

`sigctx_inl_t` carries a fixed-capacity FP buffer as a struct member. Its `sizeof`
is a compile-time constant, so you can place it inline inside a thread control block
with no allocation. The capacity is `SIGCTX_FPSTATE_CAPACITY`, a compile-time bound
you can override before including the header.

`sigctx_dyn_t` carries a pointer and a size for a heap buffer sized at runtime from
`sigctx_fpstate_size()`. Use it when the enabled XSAVE area is large, for example with
AVX-512 or AMX, or when you prefer not to pay the inline bound.

Switching between the two is a matter of the wrapper type and which expressions you
pass for the buffer and size. Nothing in the library below changes.

## Scope and limitations

x86-64 Linux only. The architecture dispatch header raises a compile error on any
other target. aarch64 would be a separate transcription of its own sigframe layout
and is not implemented.

glibc 2.34 or newer, because the interceptor checks the runtime `_SC_SIGSTKSZ` value,
which earlier glibc reports as a stale fixed constant.

Synthesis with `sigctx_create` builds a legacy FP frame. Full XSAVE state such as AVX
is preserved when a running context is captured with `sigctx_copy`, not when a fresh
context is created, because a fresh thread has no live vector state to carry.

On capture of an arbitrary preempted context, the kernel runs `VZEROUPPER` on the
signal-delivery path before any handler can observe the registers. The upper halves
of the YMM and ZMM registers of the interrupted code are therefore already zero at
the moment of capture. The low 128 bits, which are the SSE state, are preserved and
relocated faithfully. This is a property of Linux signal delivery, not of `sigctx`,
and the test suite verifies the SSE low half rather than asserting upper-half survival
that the platform does not allow.

The interceptor sizes its FP capture buffer from the running machine at install time.
If the handler stack you supply cannot hold a captured context plus that FP area plus
the handler's own frame, `sigctx_intercept_install` returns a negative errno.

## Sharp edges

`sigctx` is Linux x86-64 kernel-ABI code. It constructs signal-frame-shaped
contexts and resumes them with `rt_sigreturn`. It is intended for runtimes,
green-thread schedulers, experiments, and systems code where the caller controls
the signal, stacks, and thread model.

It is not a drop-in replacement for pthreads, C++ coroutines, makecontext, or
setjmp/longjmp. Invalid stacks, invalid FP buffers, returning from an entry
function, or delivering the configured signal to an uninstalled OS thread can
crash the process.
