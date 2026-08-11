#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Generic IR waveform.
 *
 * durations[0] = mark  (carrier ON)
 * durations[1] = space (carrier OFF)
 * durations[2] = mark
 * durations[3] = space
 * ...
 */
typedef struct {
    uint16_t *durations;
    size_t count;
} ac_ir_frame_t;