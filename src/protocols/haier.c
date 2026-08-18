#include "haier.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define HAIER_STATE_BYTES             14
#define HAIER_IR_FRAME_MAX_DURATIONS  229

/* Exact nominal timings derived from PulseView captures */
#define HAIER_HEADER_MARK_1   3010
#define HAIER_HEADER_SPACE_1  3075
#define HAIER_HEADER_MARK_2   3010
#define HAIER_HEADER_SPACE_2  4430

#define HAIER_BIT_MARK        545
#define HAIER_ZERO_SPACE      545
#define HAIER_ONE_SPACE       1680

#define HAIER_TRAILING_MARK   545

#define HAIER_MODEL_A         0xA6

#define HAIER_MIN_TEMP        16
#define HAIER_MAX_TEMP        30


/* -------------------------------------------------------------------------- */
/* Fan field                                                                  */
/* -------------------------------------------------------------------------- */

/*
 * AUTO = 0x05 is confirmed from captures.
 * LOW / MEDIUM / HIGH are still provisional.
 */
#define HAIER_FAN_HIGH        0x01
#define HAIER_FAN_MEDIUM      0x02
#define HAIER_FAN_LOW         0x03
#define HAIER_FAN_AUTO        0x05


/* -------------------------------------------------------------------------- */
/* Mode field                                                                 */
/* -------------------------------------------------------------------------- */

#define HAIER_MODE_AUTO       0x00
#define HAIER_MODE_COOL       0x01
#define HAIER_MODE_DRY        0x02
#define HAIER_MODE_HEAT       0x04
#define HAIER_MODE_FAN        0x06


/* -------------------------------------------------------------------------- */
/* Vertical swing field                                                       */
/* -------------------------------------------------------------------------- */

#define HAIER_SWING_V_OFF     0x0
#define HAIER_SWING_V_TOP     0x1
#define HAIER_SWING_V_MIDDLE  0x2
#define HAIER_SWING_V_BOTTOM  0x3
#define HAIER_SWING_V_DOWN    0xA
#define HAIER_SWING_V_AUTO    0xC


/* -------------------------------------------------------------------------- */
/* Horizontal swing field                                                     */
/* -------------------------------------------------------------------------- */

/*
 * These mappings are currently provisional.
 */
#define HAIER_SWING_H_MIDDLE    0x0
#define HAIER_SWING_H_LEFT_MAX  0x3
#define HAIER_SWING_H_LEFT      0x4
#define HAIER_SWING_H_RIGHT     0x5
#define HAIER_SWING_H_RIGHT_MAX 0x6
#define HAIER_SWING_H_AUTO      0x7


static uint8_t haier_state[HAIER_STATE_BYTES];

static uint32_t haier_durations[
    HAIER_IR_FRAME_MAX_DURATIONS
];


/* -------------------------------------------------------------------------- */
/* Frame helpers                                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t haier_push(
    ac_ir_frame_t *frame,
    uint32_t duration)
{
    if (frame->count >= HAIER_IR_FRAME_MAX_DURATIONS) {
        return ESP_ERR_NO_MEM;
    }

    frame->durations[frame->count++] = duration;

    return ESP_OK;
}


static esp_err_t haier_push_pair(
    ac_ir_frame_t *frame,
    uint32_t mark,
    uint32_t space)
{
    esp_err_t ret;

    ret = haier_push(frame, mark);
    if (ret != ESP_OK) {
        return ret;
    }

    return haier_push(frame, space);
}


static esp_err_t haier_push_bit(
    ac_ir_frame_t *frame,
    uint8_t bit)
{
    return haier_push_pair(
        frame,
        HAIER_BIT_MARK,
        bit ? HAIER_ONE_SPACE : HAIER_ZERO_SPACE
    );
}


/* -------------------------------------------------------------------------- */
/* AC enum -> Haier field                                                      */
/* -------------------------------------------------------------------------- */

static uint8_t haier_encode_fan(ac_fan_t fan)
{
    switch (fan) {

        case AC_FAN_HIGH:
            return HAIER_FAN_HIGH;

        case AC_FAN_MEDIUM:
            return HAIER_FAN_MEDIUM;

        case AC_FAN_LOW:
            return HAIER_FAN_LOW;

        case AC_FAN_AUTO:
        default:
            return HAIER_FAN_AUTO;
    }
}


static uint8_t haier_encode_mode(ac_mode_t mode)
{
    switch (mode) {

        case AC_MODE_COOL:
            return HAIER_MODE_COOL;

        case AC_MODE_DRY:
            return HAIER_MODE_DRY;

        case AC_MODE_HEAT:
            return HAIER_MODE_HEAT;

        case AC_MODE_FAN:
            return HAIER_MODE_FAN;

        case AC_MODE_AUTO:
        default:
            return HAIER_MODE_AUTO;
    }
}


static uint8_t haier_encode_swing_v(ac_swing_v_t swing)
{
    switch (swing) {

        case AC_SWING_V_TOP:
            return HAIER_SWING_V_TOP;

        case AC_SWING_V_MIDDLE:
            return HAIER_SWING_V_MIDDLE;

        case AC_SWING_V_BOTTOM:
            return HAIER_SWING_V_BOTTOM;

        case AC_SWING_V_DOWN:
            return HAIER_SWING_V_DOWN;

        case AC_SWING_V_AUTO:
            return HAIER_SWING_V_AUTO;

        case AC_SWING_V_OFF:
        default:
            return HAIER_SWING_V_OFF;
    }
}


static uint8_t haier_encode_swing_h(ac_swing_h_t swing)
{
    switch (swing) {

        case AC_SWING_H_LEFT_MAX:
            return HAIER_SWING_H_LEFT_MAX;

        case AC_SWING_H_LEFT:
            return HAIER_SWING_H_LEFT;

        case AC_SWING_H_RIGHT:
            return HAIER_SWING_H_RIGHT;

        case AC_SWING_H_RIGHT_MAX:
            return HAIER_SWING_H_RIGHT_MAX;

        case AC_SWING_H_AUTO:
            return HAIER_SWING_H_AUTO;

        case AC_SWING_H_MIDDLE:
        default:
            return HAIER_SWING_H_MIDDLE;
    }
}


/* -------------------------------------------------------------------------- */
/* Haier state                                                                */
/* -------------------------------------------------------------------------- */

static esp_err_t haier_build_state(
    const ac_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (command->temperature < HAIER_MIN_TEMP ||
        command->temperature > HAIER_MAX_TEMP) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(
        haier_state,
        0,
        sizeof(haier_state)
    );


    /*
     * Byte 0:
     *
     * Model = A6
     */
    haier_state[0] = HAIER_MODEL_A;


    /*
     * Byte 1:
     *
     * bits 4..7 = temperature - 16
     * bits 0..3 = vertical swing
     */
    uint8_t temperature =
        command->temperature - HAIER_MIN_TEMP;

    uint8_t swing_v =
        haier_encode_swing_v(command->swing_v);

    haier_state[1] =
        ((temperature & 0x0F) << 4) |
        (swing_v & 0x0F);


    /*
     * Byte 2:
     *
     * bits 5..7 = horizontal swing
     */
    uint8_t swing_h =
        haier_encode_swing_h(command->swing_h);

    haier_state[2] =
        (swing_h & 0x07) << 5;


    /*
     * Byte 3:
     *
     * Health = bit 1
     */
    if (command->health) {
        haier_state[3] |= (1U << 1);
    }


    /*
     * Byte 4:
     *
     * Power = bit 6
     */
    if (command->power) {
        haier_state[4] |= (1U << 6);
    }


    /*
     * Byte 5:
     *
     * Fan = bits 5..7
     */
    haier_state[5] =
        (haier_encode_fan(command->fan) & 0x07) << 5;


    /*
     * Byte 6:
     *
     * Turbo = bit 6
     * Quiet = bit 7
     */
    if (command->turbo) {
        haier_state[6] |= (1U << 6);
    }

    if (command->quiet) {
        haier_state[6] |= (1U << 7);
    }


    /*
     * Byte 7:
     *
     * Mode = bits 5..7
     */
    haier_state[7] =
        (haier_encode_mode(command->mode) & 0x07) << 5;


    /*
     * Byte 8:
     *
     * Sleep = bit 7
     */
    if (command->sleep) {
        haier_state[8] |= (1U << 7);
    }


    /*
     * Bytes 9..11:
     *
     * Timers / other fields.
     * Not currently exposed.
     */


    /*
     * Byte 12:
     *
     *
     *
     * The current common ac_command_t interface does not expose
     * that information, so V1 uses TEMP_UP (0x00), which is the
     * value observed in the temperature-change captures.
     *
     * IMPORTANT:
     * This is a protocol limitation, not a generic "state update"
     * value. FAN/MODE/POWER captures use different values.
     */
    haier_state[12] = 0x00;


    /*
     * Byte 13:
     *
     * Checksum = sum of bytes 0..12, modulo 256.
     */
    uint8_t checksum = 0;

    for (size_t i = 0; i < 13; i++) {
        checksum += haier_state[i];
    }

    haier_state[13] = checksum;

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* Waveform                                                                  */
/* -------------------------------------------------------------------------- */

static esp_err_t haier_build_frame(
    ac_ir_frame_t *frame)
{
    frame->durations = haier_durations;
    frame->count = 0;


    /*
     * Header:
     *
     * 3010 mark
     * 3075 space
     * 3010 mark
     * 4430 space
     */
    esp_err_t ret = haier_push_pair(
        frame,
        HAIER_HEADER_MARK_1,
        HAIER_HEADER_SPACE_1
    );

    if (ret != ESP_OK) {
        return ret;
    }

    ret = haier_push_pair(
        frame,
        HAIER_HEADER_MARK_2,
        HAIER_HEADER_SPACE_2
    );

    if (ret != ESP_OK) {
        return ret;
    }


    /*
     * 14 bytes × 8 bits = 112 bits.
     *
     * Captured protocol is MSB first.
     */
    for (size_t byte = 0; byte < HAIER_STATE_BYTES; byte++) {

        for (uint8_t bit = 0; bit < 8; bit++) {

            ret = haier_push_bit(
                frame,
                (haier_state[byte] >> (7 - bit)) & 0x01
            );

            if (ret != ESP_OK) {
                return ret;
            }
        }
    }


    /*
     * Final mark.
     */
    return haier_push(
        frame,
        HAIER_TRAILING_MARK
    );
}


/* -------------------------------------------------------------------------- */
/* Protocol interface                                                         */
/* -------------------------------------------------------------------------- */

static esp_err_t haier_init(void)
{
    memset(
        haier_state,
        0,
        sizeof(haier_state)
    );

    return ESP_OK;
}


static esp_err_t haier_encode(
    const ac_command_t *command,
    ac_ir_frame_t *frame)
{
    if (command == NULL ||
        frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret =
        haier_build_state(command);

    if (ret != ESP_OK) {
        return ret;
    }

    return haier_build_frame(frame);
}


const ac_protocol_t haier_protocol = {
    .init = haier_init,
    .encode = haier_encode,
};