#include "ac_ir.h"

#include <stdlib.h>

#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include <string.h>
#include "driver/gpio.h"
#define TAG "ac_ir"

#define AC_IR_TX_GPIO          CONFIG_IR_GPIO

#define AC_IR_RESOLUTION_HZ    1000000
#define AC_IR_CARRIER_FREQ_HZ  38000
#define AC_IR_CARRIER_DUTY     0.33f

/*
 * An individual RMT duration field is only 15 bits wide, even though
 * rmt_symbol_word_t itself is 32 bits (duration0:15, level0:1,
 * duration1:15, level1:1). Any logical protocol duration longer than
 * this must be split into multiple consecutive RMT duration fields
 * that share the same level.
 *
 * NOTE: ac_ir_frame_t.durations must be `const uint32_t *` (not
 * uint16_t) in ac_ir.h for logical durations longer than this limit
 * to survive intact from the protocol layer (gree.c / haier.c / etc.)
 * down to this file. This file assumes that change has been made.
 */
#define AC_IR_MAX_RMT_DURATION   32767


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
/* Logical duration -> RMT half-duration expansion                            */
/* -------------------------------------------------------------------------- */

/*
 * Logical durations (frame->durations[]) alternate level starting with
 * mark (HIGH) at index 0:
 *
 *   durations[0] -> mark  (level 1)
 *   durations[1] -> space (level 0)
 *   durations[2] -> mark  (level 1)
 *   ...
 *
 * Each logical duration expands to ceil(duration / AC_IR_MAX_RMT_DURATION)
 * RMT "half-durations" (all at the same level as the logical duration
 * they came from). A duration of exactly 0 still counts as one
 * half-duration, matching the previous behavior. Two consecutive
 * half-durations are packed into one rmt_symbol_word_t.
 *
 * Returns the total number of half-durations for the whole frame.
 */
static size_t ac_ir_count_half_durations(const ac_ir_frame_t *frame)
{
    size_t total = 0;

    for (size_t j = 0; j < frame->count; j++) {
        uint32_t d = frame->durations[j];

        if (d == 0) {
            total += 1;
        } else {
            total += (d + AC_IR_MAX_RMT_DURATION - 1) / AC_IR_MAX_RMT_DURATION;
        }
    }

    return total;
}

/*
 * Walk the logical durations, splitting any duration longer than
 * AC_IR_MAX_RMT_DURATION into multiple same-level half-durations, and
 * write only the half-durations falling within [half_start, half_end)
 * into `symbols[]` (relative to half_start).
 *
 * `symbols[]` must already point at a buffer large enough for
 * ((half_end - half_start) + 1) / 2 entries, and should be zeroed
 * beforehand -- if (half_end - half_start) is odd, the last symbol's
 * duration1/level1 are intentionally left as whatever the caller
 * pre-set (normally 0), matching the original odd-total padding
 * behavior.
 *
 * This recomputes/re-walks from the start of the frame on every call
 * (Option A from the diagnosis: no persistent expansion state). IR
 * frames are small, so this is cheap and avoids introducing another
 * state machine that could desync the way the old copy-encoder did.
 */
static void ac_ir_emit_half_durations(
    const ac_ir_frame_t *frame,
    size_t half_start,
    size_t half_end,
    rmt_symbol_word_t *symbols)
{
    size_t half_index = 0;

    for (size_t j = 0; j < frame->count && half_index < half_end; j++) {
        uint8_t level = (j % 2 == 0) ? 1 : 0;
        uint32_t d = frame->durations[j];

        if (d == 0) {
            if (half_index >= half_start) {
                size_t rel = half_index - half_start;
                if ((rel & 1) == 0) {
                    symbols[rel / 2].level0 = level;
                    symbols[rel / 2].duration0 = 0;
                } else {
                    symbols[rel / 2].level1 = level;
                    symbols[rel / 2].duration1 = 0;
                }
            }
            half_index++;
            continue;
        }

        while (d > 0 && half_index < half_end) {
            uint32_t chunk =
                (d > AC_IR_MAX_RMT_DURATION) ? AC_IR_MAX_RMT_DURATION : d;

            if (half_index >= half_start) {
                size_t rel = half_index - half_start;
                if ((rel & 1) == 0) {
                    symbols[rel / 2].level0 = level;
                    symbols[rel / 2].duration0 = (uint16_t)chunk;
                } else {
                    symbols[rel / 2].level1 = level;
                    symbols[rel / 2].duration1 = (uint16_t)chunk;
                }
            }

            d -= chunk;
            half_index++;
        }
    }
}


/* -------------------------------------------------------------------------- */
/* Simple RMT encoder callback                                                */
/* -------------------------------------------------------------------------- */

/*
 * The RMT driver owns the `symbols[]` buffer and tells us exactly how
 * much room is currently free (`symbols_free`) and how far into the
 * logical stream we already are (`symbols_written`). We write directly
 * into that buffer.
 *
 * Two independent kinds of "splitting" happen here, and they must not
 * be conflated:
 *
 *   1. A single logical duration longer than AC_IR_MAX_RMT_DURATION is
 *      split into multiple RMT half-durations (this file's job).
 *   2. The whole frame may need more RMT symbols than fit in
 *      mem_block_symbols at once, so the driver calls this callback
 *      multiple times as memory frees up (symbols_free/symbols_written
 *      handles this; unrelated to point 1).
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

    size_t total_half = ac_ir_count_half_durations(frame);
    size_t total_symbols = (total_half + 1) / 2;

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

    memset(symbols, 0, to_write * sizeof(rmt_symbol_word_t));

    size_t half_start = symbols_written * 2;
    size_t half_end = half_start + to_write * 2;
    if (half_end > total_half) {
        half_end = total_half;
    }

    ac_ir_emit_half_durations(frame, half_start, half_end, symbols);

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
         * This is independent of the long-duration splitting done in
         * ac_ir_emit_half_durations().
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




    /*

    for (size_t i = 0; i < frame->count; i++) {
        ESP_LOGI(TAG, "duration[%u] = %u",
                (unsigned)i,
                (unsigned)frame->durations[i]);
    }
                */
    /*
     * One-shot transmission.
     */
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };


    
    esp_err_t ret =
        rmt_transmit(
            s_tx_channel,
            s_encoder,
            frame,
            sizeof(*frame),
            &tx_config
        );

    
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

    /*
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
    */
    return rett;

}