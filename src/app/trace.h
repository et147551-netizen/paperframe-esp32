// esp_timer timestamps and the G4/G5 trace pins.
//
// esp_timer is the primary instrument: microsecond resolution, a few hundred ns per
// call, no external hardware, and every interval in scope is milliseconds or longer
// against a ~19 s total. See docs/phase0-measurement-harness.md §2.
//
// G4 and G5 are the HY2.0-4P Grove connector -- the only free GPIOs brought out to a
// physical connector on this board, so an observer attaches without soldering.
// Everything else is committed: G1/G9/G10 buttons, G2/G3 I2C, G11-G15 EPD and SD,
// G21 RGB, G38-G46 audio, G47 SD CS, G48 IR.
//
// ---------------------------------------------------------------- the encoding
//
//   G5  high for the whole refresh. Rising edge frames the run, falling ends it.
//   G4  TOGGLES at each instrumented instant. Eleven toggles per refresh.
//
// Toggles, not pulses. An earlier version pulsed G4 with two back-to-back GPIO
// writes, which is ~100 ns wide at 240 MHz -- detectable by an edge-triggered input
// but with no margin, and invisible to anything that samples rather than latches.
// A toggle has no width to miss.
//
// The eleven instants, in order, matching exactly the timestamps epd_refresh_solid
// records. That correspondence is the point: an observer measuring these intervals
// is checking the firmware's own numbers, not a parallel approximation of them.
//
//    1  reset_begin       G5 also rises here
//    2  reset_end
//    3  init_end          == xfer_begin
//    4  xfer_end
//    5  pon_cmd
//    6  pon_release       driven from the BUSY ISR
//    7  drf_cmd
//    8  drf_release       driven from the BUSY ISR
//    9  pof_cmd
//   10  pof_release       driven from the BUSY ISR
//   11  run_end           G5 also falls here
//
// So interval 3->4 is t_xfer, 5->6 is t_pon (the 80 ms threshold H1 tests), 7->8 is
// t_drf, 9->10 is t_pof. Intervals 6->7 and 10->11 are the stock fixed delays, which
// makes them externally visible too.
//
// Three of the eleven are emitted from inside the BUSY ISR, so the observer is
// checking the part of the code most likely to be wrong rather than the part that is
// obviously right.

#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "soc/gpio_struct.h"

#define TRACE_PIN_MARK GPIO_NUM_4 // toggles at each instant
#define TRACE_PIN_RUN GPIO_NUM_5  // high for the duration of a refresh

#define TRACE_MARKS_PER_RUN 11

// Shared toggle state. Defined in trace.c -- a `static` inside an inline function in
// a header would give every translation unit its own copy and desynchronise them.
extern volatile uint32_t g_trace_mark_level;

void trace_init(void);

static inline int64_t trace_now_us(void)
{
    return esp_timer_get_time();
}

// Direct register writes rather than gpio_set_level(): these are called from an IRAM
// ISR, and gpio_set_level() is only placed in IRAM when CONFIG_GPIO_CTRL_FUNC_IN_IRAM
// is set. Both pins are below 32, so the low w1ts/w1tc registers cover them.
static inline void trace_mark(void)
{
    g_trace_mark_level ^= 1u;
    if (g_trace_mark_level) {
        GPIO.out_w1ts = (1u << TRACE_PIN_MARK);
    } else {
        GPIO.out_w1tc = (1u << TRACE_PIN_MARK);
    }
}

static inline void trace_run_begin(void)
{
    GPIO.out_w1ts = (1u << TRACE_PIN_RUN);
}

static inline void trace_run_end(void)
{
    GPIO.out_w1tc = (1u << TRACE_PIN_RUN);
}

#endif // TRACE_H
