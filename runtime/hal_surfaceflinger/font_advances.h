#ifndef WA_SURFACEFLINGER_FONT_ADVANCES_H
#define WA_SURFACEFLINGER_FONT_ADVANCES_H

#include "wasmandroid/surfaceflinger.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WA_FONT_ASCII_FIRST 0x20u
#define WA_FONT_ASCII_COUNT 95u

/* Optional extension to the 20-byte wa_font_metric_t ABI. Each row is written
 * by the browser under its own sequence counter, in the same face/weight order.
 * Widths are Q16 ratios of measured glyph extent to CSS font size. */
typedef struct wa_font_advances {
    uint32_t sequence;
    uint32_t ascii_q16[WA_FONT_ASCII_COUNT];
    uint32_t upper_q16, lower_q16, digit_q16, punct_q16, wide_q16;
} wa_font_advances_t;

wa_font_advances_t *wa_sf_font_advances(wa_sf_t *sf);
uint32_t wa_sf_font_calibration_generation(const wa_sf_t *sf);

#ifdef __cplusplus
}
#endif
#endif
