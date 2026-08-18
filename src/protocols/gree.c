#include "gree.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define GREE_STATE_BYTES              8
#define GREE_IR_FRAME_MAX_DURATIONS   320

/* Using standard transmitter timings; will verify against ESP32 capture */
#define GREE_HEADER_MARK       9000
#define GREE_HEADER_SPACE      4500
#define GREE_BIT_MARK          620
#define GREE_ZERO_SPACE        540
#define GREE_ONE_SPACE         1600
#define GREE_MIDDLE_GAP        20000
#define GREE_FINAL_GAP         40000

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

/* -------------------------------------------------------------------------- */
/* Frame helpers                                                              */
/* -------------------------------------------------------------------------- */

static esp_err_t gree_push(ac_ir_frame_t *frame, uint32_t duration) {
    if (frame->count >= GREE_IR_FRAME_MAX_DURATIONS) return ESP_ERR_NO_MEM;
    frame->durations[frame->count++] = duration;
    return ESP_OK;
}

static esp_err_t gree_push_pair(ac_ir_frame_t *frame, uint32_t mark, uint32_t space) {
    esp_err_t ret = gree_push(frame, mark);
    if (ret != ESP_OK) return ret;
    return gree_push(frame, space);
}

static esp_err_t gree_push_bit(ac_ir_frame_t *frame, uint8_t bit) {
    return gree_push_pair(frame, GREE_BIT_MARK, bit ? GREE_ONE_SPACE : GREE_ZERO_SPACE);
}

static esp_err_t gree_push_byte_lsb(ac_ir_frame_t *frame, uint8_t value) {
    for (uint8_t bit = 0; bit < 8; bit++) {
        esp_err_t ret = gree_push_bit(frame, (value >> bit) & 0x01);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* AC enum -> Gree native field                                              */
/* -------------------------------------------------------------------------- */

static uint8_t gree_encode_mode(ac_mode_t mode) {
    switch (mode) {
        case AC_MODE_COOL: return GREE_MODE_COOL;
        case AC_MODE_DRY: return GREE_MODE_DRY;
        case AC_MODE_HEAT: return GREE_MODE_HEAT;
        case AC_MODE_FAN: return GREE_MODE_FAN;
        case AC_MODE_AUTO:
        default: return GREE_MODE_AUTO;
    }
}

static uint8_t gree_encode_fan(ac_fan_t fan) {
    switch (fan) {
        case AC_FAN_LOW: return GREE_FAN_MIN;
        case AC_FAN_MEDIUM: return GREE_FAN_MEDIUM;
        case AC_FAN_HIGH: return GREE_FAN_MAX;
        case AC_FAN_AUTO:
        default: return GREE_FAN_AUTO;
    }
}

static uint8_t gree_encode_swing_v(ac_swing_v_t swing) {
    switch (swing) {
        case AC_SWING_V_TOP: return GREE_SWING_V_UP;
        case AC_SWING_V_MIDDLE: return GREE_SWING_V_MIDDLE;
        case AC_SWING_V_BOTTOM: return GREE_SWING_V_DOWN;
        case AC_SWING_V_DOWN: return GREE_SWING_V_DOWN;
        case AC_SWING_V_AUTO: return GREE_SWING_V_AUTO;
        case AC_SWING_V_OFF:
        default: return GREE_SWING_V_LAST_POS;
    }
}

static uint8_t gree_encode_swing_h(ac_swing_h_t swing) {
    switch (swing) {
        case AC_SWING_H_LEFT_MAX: return GREE_SWING_H_MAX_LEFT;
        case AC_SWING_H_LEFT: return GREE_SWING_H_LEFT;
        case AC_SWING_H_RIGHT: return GREE_SWING_H_RIGHT;
        case AC_SWING_H_RIGHT_MAX: return GREE_SWING_H_MAX_RIGHT;
        case AC_SWING_H_AUTO: return GREE_SWING_H_AUTO;
        case AC_SWING_H_MIDDLE:
        default: return GREE_SWING_H_MIDDLE;
    }
}

/* -------------------------------------------------------------------------- */
/* Gree checksum (Exact logic analyzer capture algorithm)                     */
/* -------------------------------------------------------------------------- */

static uint8_t gree_calc_checksum(const uint8_t state[GREE_STATE_BYTES])
{
    uint8_t sum = 10; // 0xA initial

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

static esp_err_t gree_build_state(const ac_command_t *command) {
    if (command == NULL) return ESP_ERR_INVALID_ARG;
    if (command->temperature < GREE_MIN_TEMP || command->temperature > GREE_MAX_TEMP) return ESP_ERR_INVALID_ARG;

    memset(gree_state, 0, sizeof(gree_state));

    uint8_t mode  = gree_encode_mode(command->mode);
    uint8_t fan   = gree_encode_fan(command->fan);

    gree_state[0] = (mode & 0x07) |
                    (command->power ? (1U << 3) : 0U) |
                    ((fan & 0x03) << 4) |
                    (command->swing_v == AC_SWING_V_AUTO ? (1U << 6) : 0U) |
                    (command->sleep ? (1U << 7) : 0U);

    gree_state[1] = (uint8_t)(command->temperature - GREE_MIN_TEMP);
    
    gree_state[2] = 0x20; // Matches captured remote state

    if (command->turbo) gree_state[2] |= (1U << 4);

    gree_state[3] = 0x51; // Matches captured remote state

    uint8_t swing_v = gree_encode_swing_v(command->swing_v);
    uint8_t swing_h = gree_encode_swing_h(command->swing_h);

    gree_state[4] = (swing_v & 0x0F) | ((swing_h & 0x07) << 4);
    gree_state[5] = 0x40; // Matches captured remote state
    gree_state[6] = 0x00;
    gree_state[7] = (gree_calc_checksum(gree_state) << 4);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Waveform                                                                   */
/* -------------------------------------------------------------------------- */

static esp_err_t gree_push_data_bytes(ac_ir_frame_t *frame, const uint8_t *bytes) {
    for (size_t i = 0; i < 4; i++) {
        esp_err_t ret = gree_push_byte_lsb(frame, bytes[i]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

static esp_err_t gree_build_frame(ac_ir_frame_t *frame) {
    frame->durations = gree_durations;
    frame->count = 0;

    esp_err_t ret = gree_push_pair(frame, GREE_HEADER_MARK, GREE_HEADER_SPACE);
    if (ret != ESP_OK) return ret;

    ret = gree_push_data_bytes(frame, &gree_state[0]);
    if (ret != ESP_OK) return ret;

    // Footer "010" + separate mark + middle gap
    ret = gree_push_pair(frame, GREE_BIT_MARK, GREE_ZERO_SPACE);
    if (ret != ESP_OK) return ret;
    
    ret = gree_push_pair(frame, GREE_BIT_MARK, GREE_ONE_SPACE);
    if (ret != ESP_OK) return ret;
    
    ret = gree_push_pair(frame, GREE_BIT_MARK, GREE_ZERO_SPACE);
    if (ret != ESP_OK) return ret;

    ret = gree_push_pair(frame, GREE_BIT_MARK, GREE_MIDDLE_GAP);
    if (ret != ESP_OK) return ret;

    ret = gree_push_data_bytes(frame, &gree_state[4]);
    if (ret != ESP_OK) return ret;

    return gree_push_pair(frame, GREE_BIT_MARK, GREE_FINAL_GAP);
}

/* -------------------------------------------------------------------------- */
/* Protocol interface                                                         */
/* -------------------------------------------------------------------------- */

static esp_err_t gree_init(void) {
    memset(gree_state, 0, sizeof(gree_state));
    return ESP_OK;
}

static esp_err_t gree_encode(const ac_command_t *command, ac_ir_frame_t *frame) {
    if (command == NULL || frame == NULL) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = gree_build_state(command);
    if (ret != ESP_OK) return ret;

    ret = gree_build_frame(frame);
    if (ret != ESP_OK) return ret;

    // Append the second identical frame
    size_t first_frame_count = frame->count;
    for (size_t i = 0; i < first_frame_count; i++) {
        ret = gree_push(frame, frame->durations[i]);
        if (ret != ESP_OK) return ret;
    }

    return ESP_OK;
}

const ac_protocol_t gree_protocol = {
    .init = gree_init,
    .encode = gree_encode,
};