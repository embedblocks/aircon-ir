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
 *
 * These are logical, protocol-level durations in microseconds and
 * are NOT constrained by the RMT hardware's 15-bit duration field.
 * A single logical duration may be larger than the hardware can
 * represent in one RMT symbol -- the generic TX encoder (ac_ir.c)
 * is responsible for splitting such durations into multiple RMT
 * half-durations of the same level. Protocol sources (gree.c,
 * haier.c, etc.) do not need to know about that limit and should
 * just express the real waveform timing here.
 */
typedef struct {
    uint32_t *durations;
    size_t count;
} ac_ir_frame_t;