#include "haier.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define HAIER_STATE_BYTES             14
#define HAIER_IR_FRAME_MAX_DURATIONS  229

/*
 * Haier-specific header from the captured protocol.
 *
 * 3000 mark
 * 3000 space
 * 3000 mark
 * 4370 space
 */
#define HAIER_HEADER_MARK_1   3000
#define HAIER_HEADER_SPACE_1  3000
#define HAIER_HEADER_MARK_2   3000
#define HAIER_HEADER_SPACE_2  4370

/*
 * Nominal pulse-distance encoding.
 *
 * 0 = 562.5 us mark + 562.5 us space
 * 1 = 562.5 us mark + 1687.5 us space
 *
 * Rounded to integer microseconds for the 1 MHz RMT clock.
 */
#define HAIER_BIT_MARK        563
#define HAIER_ZERO_SPACE      563
#define HAIER_ONE_SPACE       1688

#define HAIER_TRAILING_MARK   563

#define HAIER_MODEL_A         0xA6

#define HAIER_MIN_TEMP        16
#define HAIER_MAX_TEMP        30


/* Fan field */
#define HAIER_FAN_HIGH        0x01
#define HAIER_FAN_MEDIUM      0x02
#define HAIER_FAN_LOW         0x03
#define HAIER_FAN_AUTO        0x05


/* Mode field */
#define HAIER_MODE_AUTO       0x00
#define HAIER_MODE_COOL       0x01
#define HAIER_MODE_DRY        0x02
#define HAIER_MODE_HEAT       0x04
#define HAIER_MODE_FAN        0x06


/* Vertical swing field */
#define HAIER_SWING_V_OFF     0x0
#define HAIER_SWING_V_TOP     0x1
#define HAIER_SWING_V_MIDDLE  0x2
#define HAIER_SWING_V_BOTTOM  0x3
#define HAIER_SWING_V_DOWN    0xA
#define HAIER_SWING_V_AUTO    0xC


/* Horizontal swing field */
#define HAIER_SWING_H_MIDDLE    0x0
#define HAIER_SWING_H_LEFT_MAX  0x3
#define HAIER_SWING_H_LEFT      0x4
#define HAIER_SWING_H_RIGHT     0x5
#define HAIER_SWING_H_RIGHT_MAX 0x6
#define HAIER_SWING_H_AUTO      0x7


/* Button field */
#define HAIER_BUTTON_TEMP_UP    0x00
#define HAIER_BUTTON_TEMP_DOWN  0x01
#define HAIER_BUTTON_SWING_V    0x02
#define HAIER_BUTTON_SWING_H    0x03
#define HAIER_BUTTON_FAN        0x04
#define HAIER_BUTTON_POWER      0x05
#define HAIER_BUTTON_MODE       0x06
#define HAIER_BUTTON_HEALTH     0x07
#define HAIER_BUTTON_TURBO      0x08
#define HAIER_BUTTON_SLEEP      0x0B


static uint8_t haier_state[HAIER_STATE_BYTES];

static uint16_t haier_durations[
    HAIER_IR_FRAME_MAX_DURATIONS
];


/* -------------------------------------------------------------------------- */
/* Frame helpers                                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t haier_push(
    ac_ir_frame_t *frame,
    uint16_t duration)
{
    if (frame->count >= HAIER_IR_FRAME_MAX_DURATIONS) {
        return ESP_ERR_NO_MEM;
    }

    frame->durations[frame->count++] = duration;

    return ESP_OK;
}


static esp_err_t haier_push_pair(
    ac_ir_frame_t *frame,
    uint16_t mark,
    uint16_t space)
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
/* AC enum → Haier field                                                      */
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
     * Model = A
     */
    haier_state[0] = HAIER_MODEL_A;


    /*
     * Byte 1:
     *
     * bits 0..3 = vertical swing
     * bits 4..7 = temperature - 16
     */
    uint8_t temperature =
        command->temperature - HAIER_MIN_TEMP;

    uint8_t swing_v =
        haier_encode_swing_v(command->swing_v);

    haier_state[1] =
        (swing_v & 0x0F) |
        ((temperature & 0x0F) << 4);


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
     * Health = bit 1.
     */
    if (command->health) {
        haier_state[3] |= (1 << 1);
    }


    /*
     * Byte 4:
     *
     * Power = bit 6.
     */
    if (command->power) {
        haier_state[4] |= (1 << 6);
    }


    /*
     * Byte 5:
     *
     * Fan = bits 5..7.
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
        haier_state[6] |= (1 << 6);
    }

    if (command->quiet) {
        haier_state[6] |= (1 << 7);
    }


    /*
     * Byte 7:
     *
     * Mode = bits 5..7.
     */
    haier_state[7] =
        (haier_encode_mode(command->mode) & 0x07) << 5;


    /*
     * Byte 8:
     *
     * Sleep = bit 7.
     */
    if (command->sleep) {
        haier_state[8] |= (1 << 7);
    }


    /*
     * Bytes 9..11:
     *
     * Timers and other fields.
     * Not exposed in V1, therefore zero.
     */


    /*
     * Byte 12:
     *
     * Button = low five bits.
     *
     * For a complete operation command, Power is the safest
     * representative command.
     */
    haier_state[12] = HAIER_BUTTON_POWER;


    /*
     * Byte 13:
     *
     * Checksum.
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
     * Haier header:
     *
     * 3000 mark
     * 3000 space
     * 3000 mark
     * 4370 space
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
     * NEC-style transmission is LSB first.
     */
    for (size_t byte = 0; byte < HAIER_STATE_BYTES; byte++) {

        for (uint8_t bit = 0; bit < 8; bit++) {

            ret = haier_push_bit(
                frame,
                (haier_state[byte] >> (7 - bit)) & 1  // <--- FIXED: MSB first
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
/* Protocol interface                                                        */
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