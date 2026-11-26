#include <arm_neon.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t wa_fixture_route(uint32_t first, uint32_t second);
// Bypass the NDK's inline ctype implementation to exercise the host symbol.
extern int guest_isspace(int value) __asm__("isspace");

volatile uint32_t wa_ctor_value;
volatile uint32_t wa_data = 19;
volatile uint32_t wa_bss[2049];
uint32_t wa_table[3] = {11, 42, 99};
uint32_t* volatile wa_relocated_pointer = &wa_table[1];

__attribute__((aligned(4096), used)) const uint8_t wa_cold[256 * 1024] = {
    0x73,
    [256 * 1024 - 1] = 0x89,
};

__attribute__((constructor)) static void Initialize(void) {
    wa_ctor_value = 0x13579bdf;
    wa_bss[0] = 37;
}

uint32_t wa_get_data(void) {
    return wa_data;
}

uint32_t wa_add_data(uint32_t increment) {
    wa_data += increment;
    return wa_data;
}

uint32_t wa_read_relocated(void) {
    return *wa_relocated_pointer;
}

uint32_t wa_sum6(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

#if defined(__arm__)
__attribute__((target("arm"))) uint32_t wa_arm_entry(uint32_t value) {
    return value ^ 0x12u;
}
#endif

int32_t wa_neon_dot(const int32_t* a, const int32_t* b) {
    const int32x4_t products = vmulq_s32(vld1q_s32(a), vld1q_s32(b));
#if defined(__aarch64__)
    return vaddvq_s32(products);
#else
    const int32x2_t pairs = vadd_s32(vget_low_s32(products), vget_high_s32(products));
    return vget_lane_s32(vpadd_s32(pairs, pairs), 0);
#endif
}

uint32_t wa_math_bits(uint32_t bits) {
    union {
        uint32_t bits;
        float value;
    } scalar = {.bits = bits};
    scalar.value = sinf(scalar.value) + cosf(scalar.value) + sqrtf(4.0f);
    return scalar.bits;
}

uint32_t wa_libc_roundtrip(const char* text, uint32_t length) {
    char* buffer = malloc(length + 2);
    if (!buffer)
        return 0;
    memset(buffer, 'x', length + 2);
    memcpy(buffer, text, length + 1);
    memmove(buffer + 1, buffer, length + 1);
    const uint32_t result = strcmp(text, buffer + 1) == 0 ? (uint32_t)strlen(buffer) : 0;
    free(buffer);
    return result;
}

uint32_t wa_hot_libc(char* buffer, const char* text, uint32_t length, uint32_t rounds) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    uint32_t total = 0;
    while (rounds--) {
        if (pthread_mutex_lock(&lock)) return 0;
        // Touch both spans so a warmed unit can hand resident bytes to libc.
        *(volatile char*)buffer = *(const volatile char*)text;
        memcpy(buffer, text, length);
        memmove(buffer + 1, buffer, length);
        if (memcmp(buffer + 1, text, length)) return 0;
        total += strlen(buffer + 1);
        if (pthread_mutex_unlock(&lock)) return 0;
    }
    return total;
}

uint32_t wa_hot_space(uint32_t rounds) {
    uint32_t total = 0;
    while (rounds--)
        total += guest_isspace((rounds & 1) ? ' ' : 'x') != 0;
    return total;
}

void wa_ctype_results(int (*ctype)(int), int* results, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i)
        results[i] = ctype((int)i - 2);
}

uint32_t wa_hot_poll(int (*poll_once)(int, int*, int*, void**), uint32_t rounds) {
    uint32_t timeouts = 0;
    while (rounds--) {
        int fd = -1, events = -1;
        void* data = (void*)1;
        const int result = poll_once(0, &fd, &events, &data);
        if (fd || events || data) return UINT32_MAX;
        timeouts += result == -3; // ALOOPER_POLL_TIMEOUT
    }
    return timeouts;
}

uint32_t wa_hot_heap(uint32_t rounds) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < rounds; ++i) {
        char* bytes = malloc(31);
        if (!bytes) return UINT32_MAX;
        *(volatile char*)bytes = 7;
        char* grown = realloc(bytes, 32);
        if (!grown || malloc_usable_size(grown) < 32) return UINT32_MAX;
        total += (uint8_t)*grown;
        free(grown);
        bytes = calloc(8, 4);
        if (!bytes) return UINT32_MAX;
        total += (uint8_t)*bytes;
        free(bytes);
    }
    return total;
}

uint32_t wa_hot_memory(char* dst, const char* text, uint32_t size, uint32_t rounds) {
    uint32_t total = 0;
    while (rounds--) {
        memset(dst, 0xff, size + 4);
        memcpy(dst, text, size);
        memmove(dst + 3, dst, size);
        if (strcmp(dst + 3, text) || strncmp(dst + 3, text, size) ||
            memcmp(dst + 3, text, size) || strnlen(dst + 3, size - 1) != size - 1 ||
            strchr(dst + 3, '\0') != dst + 3 + size - 1 ||
            memchr(dst + 3, text[0], size) != dst + 3)
            return UINT32_MAX;
        total += strlen(dst + 3);
        strcpy(dst, text);
        strncpy(dst + 1, text, size + 2);
        memmove(dst, dst + 1, size + 2);
        if (dst[size + 1]) return UINT32_MAX;
    }
    return total;
}

typedef uint32_t (*SameObject)(uintptr_t env, uintptr_t left, uintptr_t right);

uint32_t wa_hot_same_object(SameObject same, uint32_t rounds) {
    uint32_t total = 0;
    while (rounds--)
        total += same(5, 1, (rounds & 1) ? 2 : 0);
    return total;
}

void* wa_move_bytes(void* dst, const void* src, size_t size) {
    return memmove(dst, src, size);
}

static int Compare(const void* left, const void* right) {
    const int32_t a = *(const int32_t*)left;
    const int32_t b = *(const int32_t*)right;
    return (a > b) - (a < b);
}

uint32_t wa_sort(int32_t* values, uint32_t count) {
    volatile uint32_t marker = 0x2468;
    qsort(values, count, sizeof(*values), Compare);
    return marker;
}

uint32_t wa_call_virtual(uint32_t a, uint32_t b) {
    return wa_fixture_route(a, b) + 3;
}

uint32_t wa_wasm_unit(uint32_t* word, uint32_t addend) {
    const uint32_t result = *word + addend;
    *word = result;
    return result;
}

static void* RunWorker(void* argument) {
    for (uint32_t remaining = (uintptr_t)argument; remaining; --remaining)
        __asm__ volatile("" ::: "memory");
    return argument;
}

int wa_run_sliced_worker(uint32_t iterations) {
    pthread_t worker;
    void* const argument = (void*)(uintptr_t)iterations;
    void* result = NULL;
    if (pthread_create(&worker, NULL, RunWorker, argument))
        return 0;
    return pthread_join(worker, &result) == 0 && result == argument;
}
