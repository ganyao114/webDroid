#pragma once

// AOSP public C headers use these linkage macros. Emscripten's musl has no sys/cdefs.h.
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
