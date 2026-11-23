#include "hybrid_math.h"

#include <pthread.h>

static int constructor_count;
static uint64_t cookie;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void initialize_cookie(void) {
    cookie = UINT64_C(0x100000000);
}

__attribute__((constructor)) static void initialize_math(void) {
    ++constructor_count;
    pthread_once(&once, initialize_cookie);
}

uint64_t hybrid_mix_checksum(int seed) {
    return cookie + (uint64_t)seed * 37 + constructor_count;
}

double hybrid_dot_f64(double x, double y, double z) {
    return x * 2.0 + y * 3.0 + z * 5.0;
}

void hybrid_fill_histogram(int seed, int bins[4]) {
    for (int i = 0; i < 4; ++i) bins[i] = seed + (i + 1) * (i + 1);
}

int hybrid_constructor_count(void) { return constructor_count; }
