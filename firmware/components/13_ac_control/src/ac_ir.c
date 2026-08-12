#include "ac_ir.h"

#include <stdlib.h>

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG "ac_ir"

#define AC_IR_TX_GPIO          10

#define AC_IR_RESOLUTION_HZ   1000000
#define AC_IR_CARRIER_FREQ_HZ 38000
#define AC_IR_CARRIER_DUTY    0.33f

/*
 * ESP32-C3 has limited RMT memory.
 *
 * We only generate a small number of symbols at a time.
 */
#define AC_IR_ENCODER_SYMBOLS 32


typedef struct {
    rmt_encoder_t base;

    rmt_encoder_handle_t copy_encoder;

    /*
     * Index into ac_ir_frame_t::durations.
     *
     * 0 = first mark
     * 1 = first space
     * 2 = second mark
     * ...
     */
    size_t duration_index;

    rmt_symbol_word_t symbols[AC_IR_ENCODER_SYMBOLS];
} ac_ir_encoder_t;


static rmt_channel_handle_t s_tx_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;


/* -------------------------------------------------------------------------- */
/* Custom encoder                                                             */
/* -------------------------------------------------------------------------- */

static size_t ac_ir_encoder_encode(
    rmt_encoder_t *encoder,
    rmt_channel_handle_t channel,
    const void *primary_data,
    size_t data_size,
    rmt_encode_state_t *ret_state)
{
    ac_ir_encoder_t *enc =
        __containerof(encoder, ac_ir_encoder_t, base);

    const ac_ir_frame_t *frame =
        (const ac_ir_frame_t *)primary_data;

    rmt_encode_state_t session_state =
        RMT_ENCODING_RESET;

    rmt_encode_state_t state =
        RMT_ENCODING_RESET;

    size_t encoded_symbols = 0;

    /*
     * Build a small batch of RMT symbols.
     */
    size_t remaining_durations =
        frame->count - enc->duration_index;

    size_t symbol_count =
        (remaining_durations + 1) / 2;

    if (symbol_count > AC_IR_ENCODER_SYMBOLS) {
        symbol_count = AC_IR_ENCODER_SYMBOLS;
    }

    for (size_t i = 0; i < symbol_count; i++) {

        size_t index =
            enc->duration_index + (i * 2);

        /*
         * MARK:
         * carrier ON
         */
        enc->symbols[i].level0 = 1;
        enc->symbols[i].duration0 =
            frame->durations[index];

        /*
         * SPACE:
         * carrier OFF
         */
        enc->symbols[i].level1 = 0;

        if ((index + 1) < frame->count) {
            enc->symbols[i].duration1 =
                frame->durations[index + 1];
        } else {
            /*
             * Last duration is a mark with no following space.
             */
            enc->symbols[i].duration1 = 0;
        }
    }

    /*
     * Use the ESP-IDF copy encoder to actually put the generated
     * RMT symbols into the driver's available memory.
     *
     * This is the same pattern used by the old working encoder.
     */
    encoded_symbols += enc->copy_encoder->encode(
        enc->copy_encoder,
        channel,
        enc->symbols,
        symbol_count * sizeof(rmt_symbol_word_t),
        &session_state
    );

    /*
     * The copy encoder tells us how much of the generated symbol
     * array it consumed.
     */
    enc->duration_index += encoded_symbols * 2;

    if (session_state & RMT_ENCODING_MEM_FULL) {
        state |= RMT_ENCODING_MEM_FULL;
    }

    /*
     * All durations have now been consumed.
     */
    if (enc->duration_index >= frame->count) {
        state |= RMT_ENCODING_COMPLETE;
    }

    *ret_state = state;

    return encoded_symbols;
}


static esp_err_t ac_ir_encoder_reset(
    rmt_encoder_t *encoder)
{
    ac_ir_encoder_t *enc =
        __containerof(encoder, ac_ir_encoder_t, base);

    enc->duration_index = 0;

    return rmt_encoder_reset(
        enc->copy_encoder
    );
}


static esp_err_t ac_ir_encoder_delete(
    rmt_encoder_t *encoder)
{
    ac_ir_encoder_t *enc =
        __containerof(encoder, ac_ir_encoder_t, base);

    if (enc->copy_encoder != NULL) {
        rmt_del_encoder(enc->copy_encoder);
    }

    free(enc);

    return ESP_OK;
}


static esp_err_t ac_ir_encoder_create(
    rmt_encoder_handle_t *ret_encoder)
{
    if (ret_encoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ac_ir_encoder_t *encoder =
        calloc(1, sizeof(ac_ir_encoder_t));

    if (encoder == NULL) {
        return ESP_ERR_NO_MEM;
    }

    encoder->base.encode =
        ac_ir_encoder_encode;

    encoder->base.reset =
        ac_ir_encoder_reset;

    encoder->base.del =
        ac_ir_encoder_delete;

    /*
     * rmt_copy_encoder_config_t is an empty structure
     * in this ESP-IDF version.
     */
    rmt_copy_encoder_config_t copy_config = {};

    esp_err_t ret = rmt_new_copy_encoder(
        &copy_config,
        &encoder->copy_encoder
    );

    if (ret != ESP_OK) {
        free(encoder);
        return ret;
    }

    *ret_encoder = &encoder->base;

    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* IR transmitter                                                             */
/* -------------------------------------------------------------------------- */

esp_err_t ac_ir_init(void)
{
    if (s_tx_channel != NULL) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = AC_IR_RESOLUTION_HZ,

        /*
         * ESP32-C3 RMT memory.
         *
         * The custom encoder feeds it incrementally.
         */
        .mem_block_symbols = 48,

        .trans_queue_depth = 1,

        .gpio_num = AC_IR_TX_GPIO,
    };

    esp_err_t ret = rmt_new_tx_channel(
        &tx_config,
        &s_tx_channel
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to create RMT TX channel: %s",
            esp_err_to_name(ret)
        );

        return ret;
    }

    /*
     * 38 kHz IR carrier.
     */
    rmt_carrier_config_t carrier_config = {
        .frequency_hz = AC_IR_CARRIER_FREQ_HZ,
        .duty_cycle = AC_IR_CARRIER_DUTY,
    };

    ret = rmt_apply_carrier(
        s_tx_channel,
        &carrier_config
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to configure IR carrier: %s",
            esp_err_to_name(ret)
        );

        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;

        return ret;
    }

    ret = ac_ir_encoder_create(
        &s_encoder
    );

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to create IR encoder: %s",
            esp_err_to_name(ret)
        );

        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;

        return ret;
    }

    ret = rmt_enable(s_tx_channel);

    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to enable RMT TX: %s",
            esp_err_to_name(ret)
        );

        rmt_del_encoder(s_encoder);
        s_encoder = NULL;

        rmt_del_channel(s_tx_channel);
        s_tx_channel = NULL;

        return ret;
    }

    return ESP_OK;
}


esp_err_t ac_ir_send(
    const ac_ir_frame_t *frame)
{
    if (frame == NULL ||
        frame->durations == NULL ||
        frame->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_tx_channel == NULL ||
        s_encoder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * One-shot transmission.
     */
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    esp_err_t ret = rmt_transmit(
        s_tx_channel,
        s_encoder,
        frame,
        sizeof(*frame),
        &tx_config
    );

    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * Make ac_ir_send() synchronous.
     */
    return rmt_tx_wait_all_done(
        s_tx_channel,
        1000
    );
}