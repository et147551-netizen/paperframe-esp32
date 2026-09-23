#include "epd_maint_course.h"

#include "epd_cmds.h"

// The order, once. Every rule behind it is in the header; nothing here recomputes it.
//
// EPD_COLOR_ORANGE_UNUSED (0x4) is deliberately absent: index 4 is orange and is valid on NEITHER
// panel this project drives, which epd_cmds.h says at length because the opposite was guessed twice.
static const uint8_t k_course[EPD_MAINT_COURSE_STEPS] = {
    EPD_COLOR_BLACK,  EPD_COLOR_WHITE, EPD_COLOR_RED,   EPD_COLOR_WHITE, EPD_COLOR_YELLOW,
    EPD_COLOR_WHITE,  EPD_COLOR_GREEN, EPD_COLOR_WHITE, EPD_COLOR_BLUE,  EPD_COLOR_WHITE,
};

uint8_t epd_maint_course_colour(int step)
{
    if (step < 0 || step >= EPD_MAINT_COURSE_STEPS) {
        return 0xFF;
    }
    return k_course[step];
}

const char *epd_maint_course_name(int step)
{
    return epd_maint_colour_name(epd_maint_course_colour(step));
}

// 6N + 1: a leading white, then N lots of three (black, white) pairs. The whites at the seams are
// shared, so this is 7 for one cycle and 13 for two rather than 7 and 14.
int epd_maint_clear_steps(int cycles)
{
    if (cycles < 1 || cycles > EPD_MAINT_CLEAR_CYCLES_MAX) {
        return 0;
    }
    return 2 * EPD_MAINT_CLEAR_BLACKS_PER_CYCLE * cycles + 1;
}

// W B W B W B W ... Derived from the step's parity rather than tabulated, so the sequence cannot be
// mistyped and its LENGTH is the only thing that varies -- and because every length from
// epd_maint_clear_steps() is odd, every repeat count starts and ends on white for free.
uint8_t epd_maint_clear_colour(int step, int steps)
{
    if (step < 0 || steps <= 0 || step >= steps) {
        return 0xFF;
    }
    return (step % 2 == 0) ? EPD_COLOR_WHITE : EPD_COLOR_BLACK;
}

const char *epd_maint_clear_name(int step, int steps)
{
    return epd_maint_colour_name(epd_maint_clear_colour(step, steps));
}

const char *epd_maint_colour_name(uint8_t colour)
{
    switch (colour) {
    case EPD_COLOR_BLACK:
        return "black";
    case EPD_COLOR_WHITE:
        return "white";
    case EPD_COLOR_YELLOW:
        return "yellow";
    case EPD_COLOR_RED:
        return "red";
    case EPD_COLOR_BLUE:
        return "blue";
    case EPD_COLOR_GREEN:
        return "green";
    default:
        return "?";
    }
}
