#include <arm_neon.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t wa_fixture_route(uint32_t first, uint32_t second);

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
