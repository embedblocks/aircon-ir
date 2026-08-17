#include "gree.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GREE_STATE_BYTES              8
#define GREE_IR_FRAME_MAX_DURATIONS   160

/*
 * Gree IR protocol.
 *
 * Carrier: 38 kHz
 *
 * Header:
 *   9000 us mark
 *   4500 us space
 *
 * Bit:
 *   0 = 620 us mark + 540 us space
 *   1 = 620 us mark + 1600 us space
 *
 * Frame structure (two identical blocks):
 *   Block 1: header + bytes 0..3 (LSB first) + "010" footer + gap
 *   Block 2: header + bytes 4..7 (LSB first) + "010" footer + gap
 */
#define GREE_HEADER_MARK       9000
#define GREE_HEADER_SPACE      4500
#define GREE_BIT_MARK          620
#define GREE_ZERO_SPACE        540
#define GREE_ONE_SPACE         1600
#define GREE_MESSAGE_SPACE     20000

#define GREE_MIN_TEMP          16
#define GREE_MAX_TEMP          30

/* Native mode values */
#define GREE_MODE_AUTO         0
#define GREE_MODE_COOL         1
#define GREE_MODE_DRY          2
#define GREE_MODE_FAN          3
#define GREE_MODE_HEAT         4

/* Native fan values */
#define GREE_FAN_AUTO          0
#define GREE_FAN_MIN           1
#define GREE_FAN_MEDIUM        2
#define GREE_FAN_MAX           3

/* Native vertical swing values */
#define GREE_SWING_V_LAST_POS      0x0
#define GREE_SWING_V_AUTO          0x1
#define GREE_SWING_V_UP            0x2
#define GREE_SWING_V_MIDDLE_UP    0x3
#define GREE_SWING_V_MIDDLE        0x4
#define GREE_SWING_V_MIDDLE_DOWN   0x5
#define GREE_SWING_V_DOWN          0x6
#define GREE_SWING_V_DOWN_AUTO     0x7
#define GREE_SWING_V_MIDDLE_AUTO   0x9
#define GREE_SWING_V_UP_AUTO       0xB

/* Native horizontal swing values */
#define GREE_SWING_H_OFF           0x0
#define GREE_SWING_H_AUTO          0x1
#define GREE_SWING_H_MAX_LEFT      0x2
#define GREE_SWING_H_LEFT          0x3
#define GREE_SWING_H_MIDDLE        0x4
#define GREE_SWING_H_RIGHT         0x5
#define GREE_SWING_H_MAX_RIGHT     0x6

static uint8_t gree_state[GREE_STATE_BYTES];
static uint32_t gree_durations[GREE_IR_FRAME_MAX_DURATIONS];

uint32_t rawData[] = {
    // --- 279 engineered timings ---
 8996, 4483,
  657, 1651, 658, 578, 627, 579, 622, 1687, 623, 582, 624, 583, 622, 585, 622, 583,
  624, 582, 622, 1686, 623, 1685, 622, 1686, 623, 583, 624, 581, 624, 582, 623, 583,
  622, 585, 623, 585, 623, 583, 623, 583, 624, 582, 623, 1685, 624, 1684, 623, 584,
  622, 584, 625, 582, 624, 584, 622, 584, 621, 1686, 622, 585, 622, 1685, 622, 583,
  623, 585, 623, 1686, 622, 584, 622, 20019, // 35 bits + 20ms middle gap
  623, 584, 623, 1685, 622, 1687, 622, 585, 622, 582, 624, 584, 622, 584, 621, 579,
  628, 583, 624, 583, 622, 585, 622, 583, 623, 583, 623, 585, 623, 1683, 624, 584,
  621, 583, 623, 584, 622, 585, 624, 583, 624, 583, 622, 584, 622, 582, 624, 584,
  621, 585, 623, 584, 623, 578, 627, 579, 628, 1682, 627, 584, 622, 1679, 628, 579,
  627, 40030, // 32 bits + 40ms trailing gap
  
  // --- FRAME 2 ---
  8968, 4514,
  625, 1682, 628, 580, 626, 580, 626, 1681, 626, 583, 624, 581, 626, 580, 625, 582,
  625, 580, 627, 1682, 626, 1683, 625, 1682, 626, 581, 625, 583, 625, 581, 626, 581,
  625, 581, 626, 581, 625, 580, 627, 580, 625, 582, 624, 581, 627, 1682, 628, 1681,
  626, 581, 625, 581, 624, 582, 626, 581, 625, 580, 627, 1682, 624, 1683, 625, 1682,
  625, 581, 626, 580, 626, 1683, 625, 20018, // 35 bits + 20ms middle gap
  623, 583, 623, 584, 624, 582, 624, 583, 623, 583, 622, 583, 623, 584, 622, 584,
  624, 583, 623, 583, 624, 584, 622, 582, 624, 583, 623, 584, 622, 584, 624, 583,
  623, 582, 622, 585, 625, 582, 623, 582, 625, 583, 624, 584, 622, 584, 624, 583,
  624, 583, 622, 584, 623, 582, 625, 583, 624, 584, 622, 584, 622, 584, 622, 583,
  625, 1685, 621, 584, 622, 585, 622, 584, 623, 40000 // 38 bits + 40ms trailing gap

};

/* -------------------------------------------------------------------------- */
/* Frame helpers                                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t gree_push(
    ac_ir_frame_t *frame,
    uint32_t duration)
{
    if (frame->count >= GREE_IR_FRAME_MAX_DURATIONS) {
        return ESP_ERR_NO_MEM;
    }
    frame->durations[frame->count++] = duration;
    return ESP_OK;
}

static esp_err_t gree_push_pair(
    ac_ir_frame_t *frame,
    uint32_t mark,
    uint32_t space)
{
    esp_err_t ret = gree_push(frame, mark);
    if (ret != ESP_OK) {
        return ret;
    }
    return gree_push(frame, space);
}

static esp_err_t gree_push_bit(
    ac_ir_frame_t *frame,
    uint8_t bit)
{
    return gree_push_pair(
        frame,
        GREE_BIT_MARK,
        bit ? GREE_ONE_SPACE : GREE_ZERO_SPACE);
}

static esp_err_t gree_push_byte_lsb(
    ac_ir_frame_t *frame,
    uint8_t value)
{
    for (uint8_t bit = 0; bit < 8; bit++) {
        esp_err_t ret = gree_push_bit(
            frame,
            (value >> bit) & 0x01);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* AC enum -> Gree native field                                              */
/* -------------------------------------------------------------------------- */

static uint8_t gree_encode_mode(ac_mode_t mode)
{
    switch (mode) {
        case AC_MODE_COOL:
            return GREE_MODE_COOL;
        case AC_MODE_DRY:
            return GREE_MODE_DRY;
        case AC_MODE_HEAT:
            return GREE_MODE_HEAT;
        case AC_MODE_FAN:
            return GREE_MODE_FAN;
        case AC_MODE_AUTO:
        default:
            return GREE_MODE_AUTO;
    }
}

static uint8_t gree_encode_fan(ac_fan_t fan)
{
    switch (fan) {
        case AC_FAN_LOW:
            return GREE_FAN_MIN;
        case AC_FAN_MEDIUM:
            return GREE_FAN_MEDIUM;
        case AC_FAN_HIGH:
            return GREE_FAN_MAX;
        case AC_FAN_AUTO:
        default:
            return GREE_FAN_AUTO;
    }
}

static uint8_t gree_encode_swing_v(ac_swing_v_t swing)
{
    switch (swing) {
        case AC_SWING_V_TOP:
            return GREE_SWING_V_UP;
        case AC_SWING_V_MIDDLE:
            return GREE_SWING_V_MIDDLE;
        case AC_SWING_V_BOTTOM:
            return GREE_SWING_V_DOWN;
        case AC_SWING_V_DOWN:
            return GREE_SWING_V_DOWN;
        case AC_SWING_V_AUTO:
            return GREE_SWING_V_AUTO;
        case AC_SWING_V_OFF:
        default:
            return GREE_SWING_V_LAST_POS;
    }
}

static uint8_t gree_encode_swing_h(ac_swing_h_t swing)
{
    switch (swing) {
        case AC_SWING_H_LEFT_MAX:
            return GREE_SWING_H_MAX_LEFT;
        case AC_SWING_H_LEFT:
            return GREE_SWING_H_LEFT;
        case AC_SWING_H_RIGHT:
            return GREE_SWING_H_RIGHT;
        case AC_SWING_H_RIGHT_MAX:
            return GREE_SWING_H_MAX_RIGHT;
        case AC_SWING_H_AUTO:
            return GREE_SWING_H_AUTO;
        case AC_SWING_H_MIDDLE:
        default:
            return GREE_SWING_H_MIDDLE;
    }
}


/* -------------------------------------------------------------------------- */
/* Gree checksum                                                              */
/* -------------------------------------------------------------------------- */

/*
 * Gree 4-bit block checksum:
 *
 *   sum = 0
 *   + low nibble of bytes 0..3
 *   + high nibble of bytes 4..6
 *   modulo 16
 *
 * The result occupies the high nibble of byte 7.
 */
static uint8_t gree_calc_checksum(const uint8_t state[GREE_STATE_BYTES])
{
    uint8_t sum = 0;

    for (uint8_t i = 0; i < 4; i++) {
        sum += state[i] & 0x0F;
    }

    for (uint8_t i = 4; i < 7; i++) {
        sum += (state[i] >> 4) & 0x0F;
    }

    return sum & 0x0F;
}


/* -------------------------------------------------------------------------- */
/* State                                                                      */
/* -------------------------------------------------------------------------- */

static esp_err_t gree_build_state(
    const ac_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (command->temperature < GREE_MIN_TEMP ||
        command->temperature > GREE_MAX_TEMP) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(gree_state, 0, sizeof(gree_state));

    /*
     * Byte 0:
     *   bits 0..2 = mode
     *   bit  3    = power
     *   bits 4..5 = fan
     *   bit  6    = swing-v auto flag
     *   bit  7    = sleep
     */
    uint8_t mode  = gree_encode_mode(command->mode);
    uint8_t fan   = gree_encode_fan(command->fan);

    gree_state[0] =
        (mode & 0x07) |
        (command->power ? (1U << 3) : 0U) |
        ((fan & 0x03) << 4) |
        (command->swing_v == AC_SWING_V_AUTO ? (1U << 6) : 0U) |
        (command->sleep ? (1U << 7) : 0U);

    /*
     * Byte 1:
     *   bits 0..3 = temperature - 16
     */
    gree_state[1] =
        (uint8_t)(command->temperature - GREE_MIN_TEMP);

    /*
     * Byte 2:
     *   bit 4 = turbo
     *   bit 5 = light (on by default)
     *   bit 6 = model-A flag (must be set)
     *   bit 7 = X-Fan
     */
    gree_state[2] = (1U << 5) | (1U << 6);   /* Light + ModelA */

    if (command->turbo) {
        gree_state[2] |= (1U << 4);
    }

    /*
     * Byte 3:
     *   bits 4..7 = fixed 0x5
     */
    gree_state[3] = 0x50;

    /*
     * Byte 4:
     *   bits 0..3 = vertical swing position
     *   bits 4..6 = horizontal swing
     */
    uint8_t swing_v = gree_encode_swing_v(command->swing_v);
    uint8_t swing_h = gree_encode_swing_h(command->swing_h);

    gree_state[4] =
        (swing_v & 0x0F) |
        ((swing_h & 0x07) << 4);

    /*
     * Byte 5:
     *   bit 5 = fixed 1
     */
    gree_state[5] = 0x20;

    /*
     * Byte 6: unused
     */
    gree_state[6] = 0x00;

    /*
     * Byte 7:
     *   high nibble = checksum
     *   low nibble  = 0
     */
    gree_state[7] = (gree_calc_checksum(gree_state) << 4);

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* Waveform                                                                   */
/* -------------------------------------------------------------------------- */

/*
 * Push a single block: header + 4 bytes (LSB first) + "010" footer + gap.
 *
 * The "010" footer is three literal bits followed by a trailing mark
 * and the inter-block / final gap.
 */
static esp_err_t gree_push_block(
    ac_ir_frame_t *frame,
    const uint8_t *bytes)
{
    /* Header */
    esp_err_t ret = gree_push_pair(
        frame,
        GREE_HEADER_MARK,
        GREE_HEADER_SPACE);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 4 data bytes, LSB first */
    for (size_t i = 0; i < 4; i++) {
        ret = gree_push_byte_lsb(frame, bytes[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    /*
     * Footer "010" (3 bits) + trailing mark + gap.
     *
     *   bit 0 -> mark + zero_space
     *   bit 1 -> mark + one_space
     *   bit 0 -> mark + zero_space
     *   trailing mark + message_space
     */
    ret = gree_push_bit(frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gree_push_bit(frame, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = gree_push_bit(frame, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    return gree_push_pair(
        frame,
        GREE_BIT_MARK,
        GREE_MESSAGE_SPACE);
}

static esp_err_t gree_build_frame(ac_ir_frame_t *frame)
{
    frame->durations = gree_durations;
    frame->count = 0;

    /* Block 1: header + bytes 0..3 + "010" + gap */
    esp_err_t ret = gree_push_block(frame, &gree_state[0]);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Block 2: header + bytes 4..7 + "010" + gap */
    return gree_push_block(frame, &gree_state[4]);
}


/* -------------------------------------------------------------------------- */
/* Protocol interface                                                         */
/* -------------------------------------------------------------------------- */

static esp_err_t gree_init(void)
{
    memset(gree_state, 0, sizeof(gree_state));
    return ESP_OK;
}

static esp_err_t gree_encode(
    const ac_command_t *command,
    ac_ir_frame_t *frame)
{
    if (command == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = gree_build_state(command);
    if (ret != ESP_OK) {
        return ret;
    }

    

    frame->durations = rawData;
    frame->count = sizeof(rawData) / sizeof(rawData[0]);

    return ESP_OK;
    //return gree_build_frame(frame);
}

const ac_protocol_t gree_protocol = {
    .init = gree_init,
    .encode = gree_encode,
};