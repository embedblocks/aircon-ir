#include "midea.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MIDEA_FRAME_MAX_DURATIONS 500

#define MIDEA_TEMP_LOW             17
#define MIDEA_TEMP_HIGH            30

/*
 * T = 21 cycles of a 38 kHz carrier = 552.63 us.
 * Captures confirm:
 *   header  ≈ 4.40 ms = 8 × 550 us
 *   1-space ≈ 1.65 ms = 3 × 550 us
 *   stop    ≈ 5.23 ms (measured, not a multiple of T)
 */
#define MIDEA_T                    550

/* Header: 8T mark + 8T space */
#define MIDEA_HEADER_MARK          (8 * MIDEA_T)
#define MIDEA_HEADER_SPACE         (8 * MIDEA_T)

/* Bit: 1T mark + 1T or 3T space */
#define MIDEA_BIT_MARK             MIDEA_T
#define MIDEA_ZERO_SPACE           MIDEA_T
#define MIDEA_ONE_SPACE            (3 * MIDEA_T)

/*
 * Stop: 1T mark + ~5230 us space.
 * Captures consistently show 5.228–5.230 ms.
 */
#define MIDEA_STOP_MARK            MIDEA_T
#define MIDEA_STOP_SPACE           5230

#define MIDEA_DATA_BYTES           3
#define MIDEA_TRANSMITTED_BYTES    6
#define MIDEA_BITS                (MIDEA_TRANSMITTED_BYTES * 8)

/* -------------------------------------------------------------------------- */
/* Midea protocol values                                                      */
/* -------------------------------------------------------------------------- */

/* Temperature encoding — verified from captures */
static const uint8_t temperature_table[] = {
    0b0000,   /* 17 C */
    0b0001,   /* 18 C */
    0b0011,   /* 19 C */
    0b0010,   /* 20 C */
    0b0110,   /* 21 C */
    0b0111,   /* 22 C */
    0b0101,   /* 23 C */
    0b0100,   /* 24 C */
    0b1100,   /* 25 C */
    0b1101,   /* 26 C */
    0b1001,   /* 27 C */
    0b1000,   /* 28 C */
    0b1010,   /* 29 C */
    0b1011,   /* 30 C */
};

/* Fan encoding — verified from 24°C Cool fan-speed captures */
static const uint8_t fan_table[] = {
    0b1011,   /* auto */
    0b1001,   /* low */
    0b0101,   /* medium */
    0b0011,   /* high */
};

/*
 * In Auto and Dry modes, the fan speed is not user-configurable.
 * Captures (e.g., Auto 21C -> 1F E0) show the fan nibble is 1 in these modes.
 */
#define MIDEA_FAN_AUTO_DRY         0b0001

/* Mode values — verified from captures */
#define MIDEA_MODE_COOL            0b0000
#define MIDEA_MODE_HEAT            0b1100
#define MIDEA_MODE_AUTO            0b1000
#define MIDEA_MODE_FAN             0b0100
#define MIDEA_MODE_DRY             0b0100

#define MIDEA_STATE_ON             0b1111
#define MIDEA_STATE_OFF            0b1011

/* In FAN mode the temperature field is 1110. */
#define MIDEA_TEMP_FAN             0b1110

/* -------------------------------------------------------------------------- */
/* Protocol state                                                             */
/* -------------------------------------------------------------------------- */

static uint32_t midea_durations[MIDEA_FRAME_MAX_DURATIONS];

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static esp_err_t midea_push(
    ac_ir_frame_t *frame,
    uint32_t duration)
{
    if (frame->count >= MIDEA_FRAME_MAX_DURATIONS) {
        return ESP_ERR_NO_MEM;
    }
    frame->durations[frame->count++] = duration;
    return ESP_OK;
}

static esp_err_t midea_push_pair(
    ac_ir_frame_t *frame,
    uint32_t mark,
    uint32_t space)
{
    esp_err_t ret = midea_push(frame, mark);
    if (ret != ESP_OK) {
        return ret;
    }
    return midea_push(frame, space);
}

static esp_err_t midea_push_bit(
    ac_ir_frame_t *frame,
    bool bit)
{
    return midea_push_pair(
        frame,
        MIDEA_BIT_MARK,
        bit ? MIDEA_ONE_SPACE : MIDEA_ZERO_SPACE
    );
}

/* -------------------------------------------------------------------------- */
/* Field encoding                                                             */
/* -------------------------------------------------------------------------- */

static uint8_t midea_encode_temperature(
    uint8_t temperature)
{
    if (temperature < MIDEA_TEMP_LOW ||
        temperature > MIDEA_TEMP_HIGH) {
        return MIDEA_TEMP_FAN;
    }
    return temperature_table[temperature - MIDEA_TEMP_LOW];
}

static uint8_t midea_encode_fan(
    ac_fan_t fan)
{
    switch (fan) {
        case AC_FAN_LOW:
            return fan_table[1];
        case AC_FAN_MEDIUM:
            return fan_table[2];
        case AC_FAN_HIGH:
            return fan_table[3];
        case AC_FAN_AUTO:
        default:
            return fan_table[0];
    }
}

static uint8_t midea_encode_mode(
    ac_mode_t mode)
{
    switch (mode) {
        case AC_MODE_COOL:
            return MIDEA_MODE_COOL;
        case AC_MODE_HEAT:
            return MIDEA_MODE_HEAT;
        case AC_MODE_FAN:
            return MIDEA_MODE_FAN;
        case AC_MODE_DRY:
            return MIDEA_MODE_DRY;
        case AC_MODE_AUTO:
        default:
            return MIDEA_MODE_AUTO;
    }
}

/* -------------------------------------------------------------------------- */
/* Build the three-byte Midea packet                                          */
/* -------------------------------------------------------------------------- */

/*
 * The three logical bytes are transmitted as six bytes:
 *
 *   data[0] = packet[0]      data[1] = ~packet[0]
 *   data[2] = packet[1]      data[3] = ~packet[1]
 *   data[4] = packet[2]      data[5] = ~packet[2]
 *
 * Since ~0xB2 = 0x4D, the transmitted stream starts with B2 4D.
 *
 * Captured frame structure (verified from all logs):
 *
 *   packet[0] = 0xB2                          (constant)
 *   packet[1] = (fan_code << 4) | state       (fan + power state)
 *   packet[2] = (temp_code << 4) | mode_code  (temperature + mode)
 */
static esp_err_t midea_build_packet(
    const ac_command_t *command,
    uint8_t packet[MIDEA_DATA_BYTES])
{
    if (command == NULL || packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Byte 0: constant 0xB2 (10110010) */
    packet[0] = 0xB2;

    /*
     * Byte 1: FFFF SSSS
     *
     * Upper nibble = fan code
     * Lower nibble  = power state (ON=0xF, OFF=0xB)
     *
     * In Auto and Dry modes the fan nibble is fixed to 0x1.
     * In Cool/Heat/Fan modes it follows the fan table.
     */
    uint8_t fan;

    if (command->mode == AC_MODE_AUTO ||
        command->mode == AC_MODE_DRY) {
        fan = MIDEA_FAN_AUTO_DRY;
    } else {
        fan = midea_encode_fan(command->fan);
    }

    uint8_t state =
        command->power ? MIDEA_STATE_ON : MIDEA_STATE_OFF;

    packet[1] = ((fan & 0x0F) << 4) | (state & 0x0F);

    /*
     * Byte 2: TTTT MMMM
     *
     * Upper nibble = temperature code
     * Lower nibble  = mode code
     *
     * FAN mode uses special temperature 0xE.
     * DRY mode uses mode nibble 4 (same as FAN) with normal temperature.
     */
    uint8_t temperature;

    if (command->mode == AC_MODE_FAN) {
        temperature = MIDEA_TEMP_FAN;
    } else {
        temperature = midea_encode_temperature(command->temperature);
    }

    uint8_t mode = midea_encode_mode(command->mode);

    packet[2] = ((temperature & 0x0F) << 4) | (mode & 0x0F);

    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/* Add complementary bytes                                                    */
/* -------------------------------------------------------------------------- */

static void midea_build_transmitted_bytes(
    const uint8_t packet[MIDEA_DATA_BYTES],
    uint8_t data[MIDEA_TRANSMITTED_BYTES])
{
    for (size_t i = 0; i < MIDEA_DATA_BYTES; i++) {
        data[i * 2] = packet[i];
        data[i * 2 + 1] = (uint8_t)~packet[i];
    }
}

/* -------------------------------------------------------------------------- */
/* Build waveform                                                             */
/* -------------------------------------------------------------------------- */

static esp_err_t midea_build_frame(
    const uint8_t data[MIDEA_TRANSMITTED_BYTES],
    ac_ir_frame_t *frame)
{
    frame->durations = midea_durations;
    frame->count = 0;

    /* Start: 8T mark + 8T space */
    esp_err_t ret = midea_push_pair(
        frame, MIDEA_HEADER_MARK, MIDEA_HEADER_SPACE);
    if (ret != ESP_OK) {
        return ret;
    }

    /* 48 bits, MSB first */
    for (size_t byte = 0; byte < MIDEA_TRANSMITTED_BYTES; byte++) {
        uint8_t value = data[byte];
        for (int bit = 7; bit >= 0; bit--) {
            ret = midea_push_bit(frame, (value >> bit) & 1);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    /* Stop: 1T mark + ~5230 us space */
    return midea_push_pair(frame, MIDEA_STOP_MARK, MIDEA_STOP_SPACE);
}

/* -------------------------------------------------------------------------- */
/* Protocol interface                                                         */
/* -------------------------------------------------------------------------- */

static esp_err_t midea_init(void)
{
    return ESP_OK;
}

static esp_err_t midea_encode(
    const ac_command_t *command,
    ac_ir_frame_t *frame)
{
    if (command == NULL || frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet[MIDEA_DATA_BYTES];
    uint8_t data[MIDEA_TRANSMITTED_BYTES];

    esp_err_t ret = midea_build_packet(command, packet);
    if (ret != ESP_OK) {
        return ret;
    }

    midea_build_transmitted_bytes(packet, data);

    /*
     * The Midea remote sends every command twice.
     * Both frames are identical, separated only by the stop space.
     */
    ret = midea_build_frame(data, frame);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Append the second identical frame */
    size_t first_frame_count = frame->count;

    for (size_t i = 0; i < first_frame_count; i++) {
        ret = midea_push(frame, frame->durations[i]);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

const ac_protocol_t midea_protocol = {
    .init = midea_init,
    .encode = midea_encode,
};