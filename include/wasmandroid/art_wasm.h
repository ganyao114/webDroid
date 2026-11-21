#pragma once

#include "wasmandroid/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WA_ART_ABI_VERSION 1u
#define WA_ART_NO_INDEX UINT32_MAX

// ref_stack points to uint32_t object references in the chunk's wasm32 memory.
// Native hosts use the same four-byte layout to access that memory.
typedef struct wa_art_thread {
#ifdef __wasm32__
    uint32_t* ref_stack;
#else
    uint32_t ref_stack;
#endif
    uint32_t ref_top;
    uint32_t suspend_flag;
    uint32_t pending_exception;
    uint32_t ref_capacity;
} wa_art_thread_t;

typedef enum wa_art_element_kind {
    WA_ART_OBJECT = 0,
    WA_ART_BOOLEAN,
    WA_ART_BYTE,
    WA_ART_CHAR,
    WA_ART_SHORT,
    WA_ART_INT,
    WA_ART_LONG,
    WA_ART_FLOAT,
    WA_ART_DOUBLE,
    WA_ART_REFERENCE,
    WA_ART_STRING,
} wa_art_element_kind_t;

// References are offsets of eight-byte-aligned headers in the arena. Zero is null.
// field_ref_bitmap indexes the heap's immutable class bitmaps. Array metadata
// describes the inline payload following the header. size includes the header.
typedef struct wa_art_obj_header {
    uint32_t class_id;
    uint32_t mark_bit;
    uint32_t size;
    uint32_t field_ref_bitmap;
    uint32_t array_len;
    uint32_t element_kind;
} wa_art_obj_header_t;

typedef struct wa_art_gc_stats {
    uint64_t live_objects;
    uint64_t reclaimed_objects; // Cumulative, since heap creation.
    uint64_t live_bytes;
    uint64_t gc_cycles;
    uint64_t safepoints;
} wa_art_gc_stats_t;

typedef enum wa_art_exception_kind {
    WA_ART_NULL_POINTER = 0,
    WA_ART_ARITHMETIC,
    WA_ART_ARRAY_BOUNDS,
    WA_ART_NEGATIVE_ARRAY_SIZE,
    WA_ART_CLASS_CAST,
    WA_ART_ARRAY_STORE,
    WA_ART_OUT_OF_MEMORY,
    WA_ART_STACK_OVERFLOW,
    WA_ART_UNSATISFIED_LINK,
    WA_ART_NO_SUCH_METHOD,
    WA_ART_NO_SUCH_FIELD,
    WA_ART_EXCEPTION_COUNT,
} wa_art_exception_kind_t;

typedef struct wa_art_dex wa_art_dex_t;
typedef struct wa_art_heap wa_art_heap_t;

// Open one logical DEX. header_offset is zero for standalone DEX and the first
// DEX in a v041 container. Strings are stored as their original modified UTF-8.
wa_art_dex_t* wa_art_dex_open(const uint8_t* data, size_t size, uint32_t header_offset,
                              char* error, size_t error_size);
void wa_art_dex_close(wa_art_dex_t* dex);
uint32_t wa_art_dex_find_type(const wa_art_dex_t* dex, const char* descriptor);
uint32_t wa_art_dex_find_field(const wa_art_dex_t* dex, const char* descriptor);
uint32_t wa_art_dex_find_method(const wa_art_dex_t* dex, const char* descriptor);

// profile is newline-separated DEX class descriptors or complete method
// descriptors (one per line). NULL selects every method with code and every
// declared native stub. The caller releases output with wa_art_free_buffer.
int32_t wa_art_compile(const wa_art_dex_t* dex, const char* profile, uint8_t** output,
                       size_t* output_size, char* error, size_t error_size);
void wa_art_free_buffer(void* buffer);

// The DEX metadata must outlive the heap. Mutators sharing a heap are scheduled
// cooperatively. The owner parks other mutators before allocating or collecting.
wa_art_heap_t* wa_art_heap_create(const wa_art_dex_t* dex, uint32_t capacity);
void wa_art_heap_destroy(wa_art_heap_t* heap);
int32_t wa_art_heap_bind_memory(wa_art_heap_t* heap, void* linear_memory, size_t size);
int32_t wa_art_thread_attach(wa_art_heap_t* heap, uint32_t thread, uint32_t stack,
                             uint32_t stack_capacity);
int32_t wa_art_thread_detach(wa_art_heap_t* heap, uint32_t thread);
void wa_art_gc_collect(wa_art_heap_t* heap);
void wa_art_gc_get_stats(const wa_art_heap_t* heap, wa_art_gc_stats_t* out);
int32_t wa_art_heap_get_header(const wa_art_heap_t* heap, uint32_t ref, wa_art_obj_header_t* out);

// Chunk runtime helpers. The JS or VM host binds heap and exposes the remaining
// arguments and results as Wasm imports. Field/element values use raw bits.
// Java failures set thread.pending_exception for the generated Wasm throw.
uint32_t wa_art_rt_new_object(wa_art_heap_t*, uint32_t thread, uint32_t type);
uint32_t wa_art_rt_new_array(wa_art_heap_t*, uint32_t thread, uint32_t type, int32_t length);
uint32_t wa_art_rt_const_string(wa_art_heap_t*, uint32_t thread, uint32_t string);
uint64_t wa_art_rt_get_field(wa_art_heap_t*, uint32_t thread, uint32_t ref, uint32_t field);
void wa_art_rt_put_field(wa_art_heap_t*, uint32_t thread, uint32_t ref, uint32_t field,
                         uint64_t value);
uint64_t wa_art_rt_get_static(wa_art_heap_t*, uint32_t thread, uint32_t field);
void wa_art_rt_put_static(wa_art_heap_t*, uint32_t thread, uint32_t field, uint64_t value);
uint32_t wa_art_rt_array_length(wa_art_heap_t*, uint32_t thread, uint32_t ref);
uint64_t wa_art_rt_array_get(wa_art_heap_t*, uint32_t thread, uint32_t ref, int32_t index);
void wa_art_rt_array_put(wa_art_heap_t*, uint32_t thread, uint32_t ref, int32_t index,
                         uint64_t value);
uint32_t wa_art_rt_is_instance(wa_art_heap_t*, uint32_t thread, uint32_t ref, uint32_t type);
uint32_t wa_art_rt_virtual_method(wa_art_heap_t*, uint32_t thread, uint32_t ref, uint32_t method);
uint32_t wa_art_rt_raise(wa_art_heap_t*, uint32_t thread, uint32_t kind);
void wa_art_rt_safepoint(wa_art_heap_t*, uint32_t thread);

// JNI reentry invokes an already-installed Wasm function by DEX method ID.
// A callback returns WA_OK or a WA_ERR_* status. For a Java throw it leaves
// pending_exception set and returns WA_OK with a zero result.
typedef int32_t (*wa_art_invoke_java_fn)(void* user, uint32_t thread, uint32_t method,
                                         const uint64_t* args, uint32_t arg_count,
                                         uint64_t* result);
typedef struct wa_art_jni_stats {
    uint64_t registered_methods;
    uint64_t native_calls;
    uint64_t java_callbacks;
    uint64_t guest_instructions;
    uint64_t guest_jni_calls;
    uint64_t guest_reentries;
} wa_art_jni_stats_t;

// registration_symbol is a guest function with signature jint(JNIEnv*),
// responsible for calling RegisterNatives on the generated ARM64 JNIEnv.
int32_t wa_art_jni_load(wa_art_heap_t*, const uint8_t* elf, size_t size,
                        const char* registration_symbol, wa_art_invoke_java_fn invoke, void* user);
void wa_art_jni_get_stats(const wa_art_heap_t*, wa_art_jni_stats_t* out);
uint64_t wa_art_rt_native(wa_art_heap_t*, uint32_t thread, uint32_t method,
                          uint64_t arg0, uint64_t arg1, uint64_t arg2,
                          uint64_t arg3, uint64_t arg4, uint64_t arg5);

#ifdef __cplusplus
}
#endif
