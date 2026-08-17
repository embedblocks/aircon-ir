#include "midea.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MIDEA_FRAME_MAX_DURATIONS 500

#define MIDEA_TEMP_LOW             17
#define MIDEA_TEMP_HIGH            30

/*
 * The source defines T as 21 cycles of a 38 kHz carrier.
 *
 * 21 / 38000 = 552.63 us
 *
 * We use the integer microsecond representation.
 */
#define MIDEA_T                    553

/*
 * Header:
 *
 * 8T mark
 * 8T space
 */
#define MIDEA_HEADER_MARK          (8 * MIDEA_T)
#define MIDEA_HEADER_SPACE         (8 * MIDEA_T)

/*
 * Bit encoding:
 *
 * 0 = 1T mark + 1T space
 * 1 = 1T mark + 3T space
 */
#define MIDEA_BIT_MARK             MIDEA_T
#define MIDEA_ZERO_SPACE           MIDEA_T
#define MIDEA_ONE_SPACE            (3 * MIDEA_T)

/*
 * Stop:
 *
 * The original implementation calls add_bit(true), which gives:
 *
 *     1T mark + 3T space
 *
 * and then adds another 8T of low time.
 *
 * Therefore:
 *
 *     1T mark + 11T space
 */
#define MIDEA_STOP_MARK            MIDEA_T
#define MIDEA_STOP_SPACE           (11 * MIDEA_T)

#define MIDEA_DATA_BYTES           3
#define MIDEA_TRANSMITTED_BYTES    6
#define MIDEA_BITS                  (MIDEA_TRANSMITTED_BYTES * 8)


/* -------------------------------------------------------------------------- */
/* Midea protocol values                                                      */
/* -------------------------------------------------------------------------- */

/*
 * Temperature encoding taken directly from the supplied source.
 */
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


/*
 * Fan encoding taken directly from the supplied source.
 *
 * 0 = automatic
 * 1 = low
 * 2 = medium
 * 3 = high
 */
static const uint8_t fan_table[] = {
    0b1011,   /* auto */
    0b1001,   /* low */
    0b0101,   /* medium */
    0b0011,   /* high */
};


/* Midea mode values from the supplied source. */
#define MIDEA_MODE_COOL            0b0000
#define MIDEA_MODE_HEAT            0b1100
#define MIDEA_MODE_AUTO            0b1000
#define MIDEA_MODE_FAN             0b0100


#define MIDEA_STATE_ON             0b1111
#define MIDEA_STATE_OFF            0b1011


/*
 * In FAN mode the temperature field is 1110.
 */
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
    esp_err_t ret;

    ret = midea_push(frame, mark);

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

    return temperature_table[
        temperature - MIDEA_TEMP_LOW
    ];
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

        case AC_MODE_AUTO:
        default:
            return MIDEA_MODE_AUTO;
    }
}


/* -------------------------------------------------------------------------- */
/* Build the three-byte Midea packet                                          */
/* -------------------------------------------------------------------------- */

static esp_err_t midea_build_packet(
    const ac_command_t *command,
    uint8_t packet[MIDEA_DATA_BYTES])
{
    if (command == NULL ||
        packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Byte 0:
     *
     * 1010 0010
     *
     * This is the constant defined by the supplied source.
     */
    packet[0] = 0xB2;


    /*
     * Byte 1:
     *
     * ffff ssss
     *
     * fan  = upper nibble
     * state = lower nibble
     */
    uint8_t fan =
        midea_encode_fan(command->fan);

    uint8_t state =
        command->power
            ? MIDEA_STATE_ON
            : MIDEA_STATE_OFF;

    packet[1] =
        ((fan & 0x0F) << 4) |
        (state & 0x0F);


    /*
     * Byte 2:
     *
     * tttt cccc
     *
     * temperature = upper nibble
     * command/mode = lower nibble
     */
    uint8_t temperature;

    if (command->mode == AC_MODE_FAN) {
        temperature = MIDEA_TEMP_FAN;
    } else {
        temperature =
            midea_encode_temperature(
                command->temperature
            );
    }

    uint8_t mode =
        midea_encode_mode(command->mode);

    packet[2] =
        ((temperature & 0x0F) << 4) |
        (mode & 0x0F);

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* Add complementary bytes                                                   */
/* -------------------------------------------------------------------------- */

static void midea_build_transmitted_bytes(
    const uint8_t packet[MIDEA_DATA_BYTES],
    uint8_t data[MIDEA_TRANSMITTED_BYTES])
{
    for (size_t i = 0; i < MIDEA_DATA_BYTES; i++) {

        data[i * 2] = packet[i];

        data[i * 2 + 1] =
            (uint8_t)~packet[i];
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


    /*
     * Start condition:
     *
     * 8T mark
     * 8T space
     */
    esp_err_t ret = midea_push_pair(
        frame,
        MIDEA_HEADER_MARK,
        MIDEA_HEADER_SPACE
    );

    if (ret != ESP_OK) {
        return ret;
    }


    /*
     * Six bytes, MSB first.
     *
     * This follows the supplied implementation:
     *
     *     print_bit(v & (1<<7));
     *     add_bit(...);
     *     v <<= 1;
     */
    for (size_t byte = 0;
         byte < MIDEA_TRANSMITTED_BYTES;
         byte++) {

        uint8_t value = data[byte];

        for (int bit = 7; bit >= 0; bit--) {

            ret = midea_push_bit(
                frame,
                (value >> bit) & 1
            );

            if (ret != ESP_OK) {
                return ret;
            }
        }
    }


    /*
     * Stop condition.
     */
    return midea_push_pair(
        frame,
        MIDEA_STOP_MARK,
        MIDEA_STOP_SPACE
    );
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
    if (command == NULL ||
        frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t packet[MIDEA_DATA_BYTES];
    uint8_t data[MIDEA_TRANSMITTED_BYTES];


    esp_err_t ret =
        midea_build_packet(
            command,
            packet
        );

    if (ret != ESP_OK) {
        return ret;
    }


    midea_build_transmitted_bytes(
        packet,
        data
    );


    /*
     * The original implementation sends the normal command
     * twice. In our architecture, repetition belongs to the
     * protocol waveform, not the RMT driver.
     *
     * Therefore this needs to be represented in the frame.
     */
    ret = midea_build_frame(
        data,
        frame
    );

    if (ret != ESP_OK) {
        return ret;
    }


    /*
     * Append the second identical frame.
     *
     * The supplied source calls:
     *
     *     send_ir_data(data, 2);
     *
     * meaning the complete packet is transmitted twice.
     */
    size_t first_frame_count =
        frame->count;

    for (size_t i = 0;
         i < first_frame_count;
         i++) {

        ret = midea_push(
            frame,
            frame->durations[i]
        );

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