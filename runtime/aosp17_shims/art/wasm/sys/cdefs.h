#pragma once

// Emscripten's musl headers do not provide the BSD linkage macros.
#ifdef __cplusplus
#define __BEGIN_DECLS extern "C" {
#define __END_DECLS }
#else
#define __BEGIN_DECLS
#define __END_DECLS
#endif
