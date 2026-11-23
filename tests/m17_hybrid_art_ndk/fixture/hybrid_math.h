#pragma once

#include <stdint.h>

uint64_t hybrid_mix_checksum(int seed);
double hybrid_dot_f64(double x, double y, double z);
void hybrid_fill_histogram(int seed, int bins[4]);
int hybrid_constructor_count(void);
