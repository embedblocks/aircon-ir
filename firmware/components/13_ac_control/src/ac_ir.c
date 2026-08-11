#include "ac_ir.h"

#include "driver/rmt_tx.h"

#define TAG "ac_ir"

#define AC_IR_TX_GPIO          18

#define AC_IR_RESOLUTION_HZ    1000000
#define AC_IR_CARRIER_HZ       38000
#define AC_IR_CARRIER_DUTY     0.33

#define AC_IR_MAX_DURATIONS     229
#define AC_IR_MAX_SYMBOLS       ((AC_IR_MAX_DURATIONS + 1) / 2)

static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t copy_encoder = NULL;

static rmt_symbol_word_t symbols[AC_IR_MAX_SYMBOLS];


esp_err_t ac_ir_init(void)
{
    if (tx_channel != NULL) {
        return ESP_OK;
    }

    rmt_tx_channel_config_t tx_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = AC_IR_RESOLUTION_HZ,
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
        .gpio_num = AC_IR_TX_GPIO,
    };

    esp_err_t ret = rmt_new_tx_channel(
        &tx_config,
        &tx_channel
    );

    if (ret != ESP_OK) {
        return ret;
    }

    rmt_carrier_config_t carrier_config = {
        .frequency_hz = AC_IR_CARRIER_HZ,
        .duty_cycle = AC_IR_CARRIER_DUTY,
    };

    ret = rmt_apply_carrier(
        tx_channel,
        &carrier_config
    );

    if (ret != ESP_OK) {
        rmt_del_channel(tx_channel);
        tx_channel = NULL;
        return ret;
    }

    rmt_copy_encoder_config_t copy_config = {0};

    ret = rmt_new_copy_encoder(
        &copy_config,
        &copy_encoder
    );

    if (ret != ESP_OK) {
        rmt_del_channel(tx_channel);
        tx_channel = NULL;
        return ret;
    }

    return rmt_enable(tx_channel);
}


esp_err_t ac_ir_send(const ac_ir_frame_t *frame)
{
    if (frame == NULL ||
        frame->durations == NULL ||
        frame->count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (tx_channel == NULL ||
        copy_encoder == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (frame->count > AC_IR_MAX_DURATIONS) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t symbol_count = (frame->count + 1) / 2;

    for (size_t i = 0; i < symbol_count; i++) {

        size_t duration_index = i * 2;

        /*
         * Mark:
         * carrier ON
         */
        symbols[i].level0 = 1;
        symbols[i].duration0 =
            frame->durations[duration_index];

        /*
         * Space:
         * carrier OFF
         */
        symbols[i].level1 = 0;

        if (duration_index + 1 < frame->count) {
            symbols[i].duration1 =
                frame->durations[duration_index + 1];
        } else {
            symbols[i].duration1 = 0;
        }
    }

    rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
    };

    esp_err_t ret = rmt_transmit(
        tx_channel,
        copy_encoder,
        symbols,
        symbol_count * sizeof(rmt_symbol_word_t),
        &transmit_config
    );

    if (ret != ESP_OK) {
        return ret;
    }

    /*
     * Make ac_ir_send() synchronous.
     */
    return rmt_tx_wait_all_done(
        tx_channel,
        1000
    );
}