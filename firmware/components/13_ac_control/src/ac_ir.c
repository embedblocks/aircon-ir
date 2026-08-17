#include "ac_ir.h"

#include <stdlib.h>

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include <string.h>
#include "driver/gpio.h"
#define TAG "ac_ir"

#define AC_IR_TX_GPIO           10

#define AC_IR_RESOLUTION_HZ    1000000
#define AC_IR_CARRIER_FREQ_HZ  38000
#define AC_IR_CARRIER_DUTY     0.33f


/* -------------------------------------------------------------------------- */
/* Module state                                                               */
/* -------------------------------------------------------------------------- */

static rmt_channel_handle_t s_tx_channel = NULL;
static rmt_encoder_handle_t s_encoder = NULL;

//debug

typedef struct {
    uint32_t symbols_written;
    uint32_t symbols_free;
    uint32_t to_write;
    uint32_t total_symbols;
    uint8_t  done;
} encode_debug_t;

static volatile encode_debug_t dbg[16];
static volatile uint32_t dbg_count = 0;
static volatile uint32_t dbg_encode_calls = 0;


/* -------------------------------------------------------------------------- */
/* Simple RMT encoder callback                                                */
/* -------------------------------------------------------------------------- */

/*
 * The RMT driver owns the `symbols[]` buffer and tells us exactly how
 * much room is currently free (`symbols_free`) and how far into the
 * logical stream we already are (`symbols_written`). We write directly
 * into that buffer.
 *
 * There is no local scratch buffer and no stateful child (copy) encoder
 * here, so there is nothing that can be overwritten out from under a
 * child encoder's internal position -- the bug that caused the
 * "correct -> garbage -> correct" pattern in the previous implementation
 * cannot occur with this architecture.
 *
 * symbol 0 -> durations[0], durations[1]
 * symbol 1 -> durations[2], durations[3]
 * symbol 2 -> durations[4], durations[5]
 * ...
 */
static size_t IRAM_ATTR ac_ir_simple_encode_cb(
    const void *data,
    size_t data_size,
    size_t symbols_written,
    size_t symbols_free,
    rmt_symbol_word_t *symbols,
    bool *done,
    void *arg)
{
    dbg_encode_calls++;

    const ac_ir_frame_t *frame = (const ac_ir_frame_t *)data;

    if (frame == NULL || data_size != sizeof(ac_ir_frame_t) ||
        frame->durations == NULL || frame->count == 0) {
        *done = true;
        return 0;
    }

    size_t total_symbols = (frame->count + 1) / 2;

    if (symbols_written >= total_symbols) {
        *done = true;
        return 0;
    }

    size_t remaining = total_symbols - symbols_written;
    size_t to_write = (remaining < symbols_free) ? remaining : symbols_free;

    if (to_write == 0) {
        /* Not enough room yet -- the driver will call us again later
         * once more space has freed up. */
        if (dbg_count < 16) {
            dbg[dbg_count].symbols_written = symbols_written;
            dbg[dbg_count].symbols_free = symbols_free;
            dbg[dbg_count].to_write = 0;
            dbg[dbg_count].total_symbols = total_symbols;
            dbg[dbg_count].done = 0;
            dbg_count++;
        }
        return 0;
    }

    for (size_t i = 0; i < to_write; i++) {
        size_t symbol_index = symbols_written + i;
        size_t duration_index = symbol_index * 2;

        symbols[i].level0 = 1;
        symbols[i].duration0 = frame->durations[duration_index];

        symbols[i].level1 = 0;
        if ((duration_index + 1) < frame->count) {
            symbols[i].duration1 = frame->durations[duration_index + 1];
        } else {
            symbols[i].duration1 = 0;
        }
    }

    bool finished = (symbols_written + to_write >= total_symbols);
    *done = finished;

    //toggle the debug gpio
    gpio_set_level(4, !gpio_get_level(4));

    if (dbg_count < 16) {
        dbg[dbg_count].symbols_written = symbols_written;
        dbg[dbg_count].symbols_free = symbols_free;
        dbg[dbg_count].to_write = to_write;
        dbg[dbg_count].total_symbols = total_symbols;
        dbg[dbg_count].done = finished ? 1 : 0;
        dbg_count++;
    }

    return to_write;
}


/* -------------------------------------------------------------------------- */
/* Encoder creation                                                            */
/* -------------------------------------------------------------------------- */

static esp_err_t ac_ir_encoder_create(
    rmt_encoder_handle_t *ret_encoder)
{
    if (ret_encoder == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rmt_simple_encoder_config_t config = {
        .callback = ac_ir_simple_encode_cb,
        .arg = NULL,
        /*
         * We can always make progress with as little as 1 free symbol,
         * so we don't need the 64-symbol default minimum.
         */
        .min_chunk_size = 1,
    };

    return rmt_new_simple_encoder(&config, ret_encoder);
}


/* -------------------------------------------------------------------------- */
/* IR transmitter initialization                                              */
/* -------------------------------------------------------------------------- */

esp_err_t ac_ir_init(void)
{
    if (s_tx_channel != NULL) {
        return ESP_OK;
    }

    //For debugging will toggle on each ir transaction
       gpio_config_t io_conf_dbg1 = {
        .pin_bit_mask = 1ULL << 4,
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(
        gpio_config(&io_conf_dbg1)
    );

    gpio_config_t io_conf_dbg2 = {
        .pin_bit_mask = 1ULL << 3,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(
        gpio_config(&io_conf_dbg2)
    );

    //Changed here and also toggled in encode cb
    gpio_set_level(4,0);
    //changed here only Juist to confirm which settings are applied here
    gpio_set_level(3,0);


    rmt_tx_channel_config_t tx_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,

        /*
         * 1 tick = 1 us.
         *
         * This lets the protocol layer express all timings
         * directly in microseconds.
         */
        .resolution_hz =
            AC_IR_RESOLUTION_HZ,

        /*
         * ESP32-C3 RMT memory.
         *
         * The simple encoder callback handles frames larger than this
         * by generating only as many symbols as the driver reports
         * free on each invocation, across multiple callback calls.
         */
        .mem_block_symbols = 80,

        .trans_queue_depth = 1,

        .gpio_num =
            AC_IR_TX_GPIO,
    };


    esp_err_t ret =
        rmt_new_tx_channel(
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
     * Hardware-generated 38 kHz IR carrier.
     */
    rmt_carrier_config_t carrier_config = {
        .frequency_hz =
            AC_IR_CARRIER_FREQ_HZ,

        .duty_cycle =
            AC_IR_CARRIER_DUTY,
    };


    ret =
        rmt_apply_carrier(
            s_tx_channel,
            &carrier_config
        );


    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to configure IR carrier: %s",
            esp_err_to_name(ret)
        );

        rmt_del_channel(
            s_tx_channel
        );

        s_tx_channel = NULL;

        return ret;
    }


    /*
     * Create our protocol-independent waveform encoder.
     */
    ret =
        ac_ir_encoder_create(
            &s_encoder
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to create IR encoder: %s",
            esp_err_to_name(ret)
        );

        rmt_del_channel(
            s_tx_channel
        );

        s_tx_channel = NULL;

        return ret;
    }


    ret =
        rmt_enable(
            s_tx_channel
        );

    if (ret != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to enable RMT TX: %s",
            esp_err_to_name(ret)
        );

        rmt_del_encoder(
            s_encoder
        );

        s_encoder = NULL;

        rmt_del_channel(
            s_tx_channel
        );

        s_tx_channel = NULL;

        return ret;
    }


    return ESP_OK;
}


/* -------------------------------------------------------------------------- */
/* IR transmission                                                            */
/* -------------------------------------------------------------------------- */

esp_err_t ac_ir_send(
    const ac_ir_frame_t *frame)
{
    if (frame == NULL ||
        frame->durations == NULL ||
        frame->count == 0) {

        return ESP_ERR_INVALID_ARG;
    }


    dbg_count = 0;
    dbg_encode_calls = 0;


    if (s_tx_channel == NULL ||
        s_encoder == NULL) {

        return ESP_ERR_INVALID_STATE;
    }


    ESP_LOGI(TAG, "count=%u", frame->count);


    for (size_t i = 0; i < frame->count; i++) {
        ESP_LOGI(TAG, "duration[%u] = %u",
                (unsigned)i,
                frame->durations[i]);
    }
    /*
     * One-shot transmission.
     */
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };


    ESP_LOGI(TAG, "=== fu ===");
    esp_err_t ret =
        rmt_transmit(
            s_tx_channel,
            s_encoder,
            frame,
            sizeof(*frame),
            &tx_config
        );

    ESP_LOGI(TAG, "=== fu again===");
    if (ret != ESP_OK) {

        ESP_LOGI(
            TAG,
            "Failed to start RMT TX: %s",
            esp_err_to_name(ret)
        );
        return ret;
    }


    /*
     * Make the public API synchronous.
     *
     * The caller returns only after the complete IR waveform
     * has physically finished transmitting.
     */

    esp_err_t rett= rmt_tx_wait_all_done(
        s_tx_channel,
        5000
    );

    ESP_LOGI(TAG, "=== ENCODER TRACE === %d",dbg_count);

        for (uint32_t i = 0; i < dbg_count && i < 16; i++) {
            ESP_LOGI(TAG,
                    "call[%u]: written=%u free=%u to_write=%u total=%u done=%d",
                    i,
                    dbg[i].symbols_written,
                    dbg[i].symbols_free,
                    dbg[i].to_write,
                    dbg[i].total_symbols,
                    dbg[i].done);

        }

    ESP_LOGI(TAG, "ENCODER CALL COUNT = %u",
         (unsigned)dbg_encode_calls);

    return rett;

}