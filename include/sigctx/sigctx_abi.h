/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Ryan Cornish
 *
 * sigctx - architecture dispatch for the vendored signal-frame ABI mirror.
 *
 * Downstream code includes this header - the correct per-architecture mirror is
 * selected below. Each per-arch header is a separate transcription of that
 * architecture's kernel sigframe layout (they share no fields in common).
 */
#ifndef SIGCTX_ABI_H
#define SIGCTX_ABI_H

#if defined(__x86_64__)
#  include <sigctx/sigctx_abi_x86_64.h>
#elif defined(__aarch64__)
#  error "sigctx: aarch64 support is not yet implemented"
#else
#  error "sigctx: unsupported architecture (x86-64 only at present)"
#endif

#endif /* SIGCTX_ABI_H */
