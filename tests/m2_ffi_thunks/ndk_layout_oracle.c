#include "generated/abi_layouts.h"

#include <signal.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <time.h>

#if defined(WA_FFI_ORACLE_WASM32)
// Use Emscripten's libc headers and the same JNI declarations as Android.
#include WA_NDK_JNI_HEADER
#if !defined(__wasm32__) || !defined(__EMSCRIPTEN__)
#error "Use the Emscripten wasm32 compiler for the Wasm oracle"
#endif
#else
#include <jni.h>
_Static_assert(__ANDROID_API__ == 21, "Use the NDK's Android API 21 drivers");
_Static_assert(sizeof(sigset_t) == sizeof(unsigned long), "Bionic signal mask width");
_Static_assert(sizeof(sigset64_t) == 8, "Bionic full signal mask width");
#endif

#if (defined(WA_FFI_ORACLE_AARCH64) && !defined(__aarch64__)) ||                                   \
    (defined(WA_FFI_ORACLE_ARMV7) && !defined(__ARM_ARCH_7A__)) ||                                 \
    (defined(WA_FFI_ORACLE_X86_64) && !defined(__x86_64__))
#error "The layout oracle must use its corresponding NDK target"
#endif

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "Expected a little-endian ABI");

#define WA_FIELD(type, field) (((type*)0)->field)
#define WA_CHECK_LAYOUT(mirror, actual)                                                           \
    _Static_assert(sizeof(mirror) == sizeof(actual), #mirror " size");                            \
    _Static_assert(_Alignof(mirror) == _Alignof(actual), #mirror " alignment")
#define WA_CHECK_MEMBER(mirror, member, actual, field)                                            \
    _Static_assert(offsetof(mirror, member) == offsetof(actual, field), #mirror "." #member);     \
    _Static_assert(sizeof(WA_FIELD(mirror, member)) == sizeof(WA_FIELD(actual, field)),             \
                   #mirror "." #member " width")
#define WA_FIELD_END(type, field) (offsetof(type, field) + sizeof(WA_FIELD(type, field)))
#define WA_CHECK_PADDING(mirror, member, offset, end)                                            \
    _Static_assert(offsetof(mirror, member) == (offset), #mirror "." #member);                     \
    _Static_assert(WA_FIELD_END(mirror, member) == (end), #mirror "." #member " end")
#define WA_CHECK_GAP(mirror, member, actual, previous, next)                                     \
    WA_CHECK_PADDING(mirror, member, WA_FIELD_END(actual, previous), offsetof(actual, next))
#define WA_CHECK_TAIL(mirror, member, actual, previous)                                          \
    WA_CHECK_PADDING(mirror, member, WA_FIELD_END(actual, previous), sizeof(actual))
#define WA_CHECK_ENV(mirror, member, actual)                                                      \
    _Static_assert(_Generic((actual)0, const struct JNINativeInterface*: 1, default: 0),           \
                   "C JNIEnv is a pointer to the interface");                                     \
    _Static_assert(offsetof(mirror, member) == 0 && sizeof(WA_FIELD(mirror, member)) ==             \
                       sizeof(actual),                                                           \
                   #mirror "." #member)

#define WA_CHECK_SIGNED(type, field)                                                             \
    _Static_assert(_Generic(WA_FIELD(type, field), signed char: 1, short: 1, int: 1, long: 1,       \
                            long long: 1, default: 0),                                            \
                   #type "." #field " must be signed")
#define WA_CHECK_UNSIGNED(type, field)                                                           \
    _Static_assert(_Generic(WA_FIELD(type, field), unsigned char: 1, unsigned short: 1,            \
                            unsigned int: 1, unsigned long: 1, unsigned long long: 1, default: 0), \
                   #type "." #field " must be unsigned")
#define WA_CHECK_POINTER(type, field)                                                            \
    _Static_assert(_Generic(WA_FIELD(type, field), void*: 1, const char*: 1, default: 0),          \
                   #type "." #field " must be a pointer")
#define WA_CHECK_FLOAT(type, field)                                                              \
    _Static_assert(_Generic(WA_FIELD(type, field), float: 1, default: 0), #type "." #field)
#define WA_CHECK_DOUBLE(type, field)                                                             \
    _Static_assert(_Generic(WA_FIELD(type, field), double: 1, default: 0), #type "." #field)

#include "generated/abi_layout_oracle.inc"

_Static_assert(sizeof(struct JNINativeInterface) == WA_FFI_JNI_TABLE_ENTRIES * sizeof(void*),
               "Every JNI slot through GetObjectRefType, including reserved entries");
_Static_assert(_Alignof(struct JNINativeInterface) == _Alignof(void*), "JNI table alignment");

#define WA_CHECK_JNI_SLOT(index, name)                                                            \
    _Static_assert(offsetof(struct JNINativeInterface, name) == (index) * sizeof(void*),          \
                   "JNI slot " #index " " #name);                                                 \
    _Static_assert(sizeof(WA_FIELD(struct JNINativeInterface, name)) == sizeof(void*),            \
                   "JNI slot " #name " width");
WA_FFI_JNI_SLOTS(WA_CHECK_JNI_SLOT)

