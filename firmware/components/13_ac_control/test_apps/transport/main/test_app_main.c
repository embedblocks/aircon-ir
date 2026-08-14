/*
 * AC IR RMT transport layer -- hardware verification test firmware.
 *
 * Purpose (see requirements.md section 1): verify that ac_ir_send()
 * physically transmits a known, deterministic duration sequence in
 * order, without loss/duplication/reordering, across the custom RMT
 * encoder's multi-invocation path (112 durations -> 56 symbols, encoder
 * batch size = 32 symbols, so this necessarily spans multiple encoder
 * invocations).
 *
 * This firmware does NOT decide pass/fail. It only:
 *   1. Transmits the test frame through the real, unmodified ac_ir.c.
 *   2. Captures the physical GPIO waveform independently, via a plain
 *      interrupt-driven edge timestamp log on a second GPIO -- NOT via
 *      RMT RX, and NOT via ac_ir.c's internal dbg[] trace (see
 *      requirements.md section 16: that trace is unpopulated and must
 *      not be trusted as evidence).
 *   3. Dumps the raw edge log over serial.
 *
 * All envelope reconstruction (carrier demodulation into mark/space
 * durations) and all comparison against the expected sequence happens
 * on the Python side (../waveform_parser.py), per requirements.md
 * section 14.
 *
 * Physical setup required:
 *   Jumper wire from the ac_ir TX GPIO (GPIO10, defined in ac_ir.c) to
 *   the RX capture GPIO defined by CONFIG_AC_IR_TEST_RX_GPIO (see
 *   Kconfig.projbuild / sdkconfig.defaults, default GPIO3).
 */

#include <stdbool.h>
#include <stdio.h>

#include "ac_ir.h"
#include "ac_ir_frame.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define TAG "ac_ir_transport_test"

/* ---------------------------------------------------------------------- */
/* Deterministic test frame (requirements.md section 6)                   */
/*                                                                        */
/* MUST stay in sync with ../expected_waveform.py                        */
/* (START_US / STEP_US / DURATION_COUNT). This is duplicated rather than */
/* shared because the two sides run in different languages/toolchains -- */
/* if you change one, change the other.                                  */
/* ---------------------------------------------------------------------- */
#define TEST_DURATION_COUNT   112
#define TEST_START_US         1000
#define TEST_STEP_US          10

static uint16_t s_test_durations[TEST_DURATION_COUNT];

static void build_test_durations(void)
{
    for (int i = 0; i < TEST_DURATION_COUNT; i++) {
        s_test_durations[i] = (uint16_t)(TEST_START_US + TEST_STEP_US * i);
    }
}

/* ---------------------------------------------------------------------- */
/* Edge capture                                                           */
/* ---------------------------------------------------------------------- */

/*
 * RX capture pin. Plain interrupt-capable GPIO input -- deliberately NOT
 * an RMT channel. Using RMT RX to verify RMT TX would share most of its
 * driver/encoder machinery with the thing under test, which defeats the
 * point of an independent measurement.
 */
#define RX_GPIO   CONFIG_AC_IR_TEST_RX_GPIO

/*
 * Edge capture buffer, sized with margin above the expected worst case
 * for the primary 112-duration test frame.
 *
 * Rough estimate: marks total ~86.8ms of carrier-on time at 38 kHz
 * (2 edges per carrier cycle) => ~6600 edges, plus one edge per
 * mark/space boundary. 16384 gives ~2.4x headroom.
 *
 * If EDGE_OVERFLOW=1 is ever reported, this buffer is too small for the
 * data being used -- raise MAX_EDGES. Do not trust a truncated capture.
 */
#define MAX_EDGES   16384

static volatile uint32_t s_edge_ts_us[MAX_EDGES];
static volatile uint8_t  s_edge_level[MAX_EDGES];
static volatile uint32_t s_edge_count = 0;
static volatile bool     s_edge_overflow = false;

static void IRAM_ATTR rx_isr_handler(void *arg)
{
    uint32_t now_us = (uint32_t)esp_timer_get_time();
    int level = gpio_get_level(RX_GPIO);

    uint32_t idx = s_edge_count;
    if (idx < MAX_EDGES) {
        s_edge_ts_us[idx] = now_us;
        s_edge_level[idx] = (uint8_t)level;
        s_edge_count = idx + 1;
    } else {
        s_edge_overflow = true;
    }
}

static void dump_edge_capture(uint32_t tx_done_ts_us)
{
    printf("AC_IR_EDGE_DUMP_START\n");
    printf("EDGE_COUNT=%u\n", (unsigned)s_edge_count);
    printf("EDGE_OVERFLOW=%u\n", s_edge_overflow ? 1u : 0u);
    printf("TX_DONE_TS_US=%u\n", (unsigned)tx_done_ts_us);

    
    for (uint32_t i = 0; i < s_edge_count; i++) {
        printf("%u,%u\n", (unsigned)s_edge_level[i], (unsigned)s_edge_ts_us[i]);
    }
    
    printf("AC_IR_EDGE_DUMP_END\n");
}

/* ---------------------------------------------------------------------- */
/* Test entry point                                                       */
/* ---------------------------------------------------------------------- */

void app_main(void)
{
    build_test_durations();

    printf("AC_IR_TRANSPORT_TEST_START\n");
    printf("DURATION_COUNT=%u\n", (unsigned)TEST_DURATION_COUNT);
    printf("EXPECTED_SYMBOL_COUNT=%u\n", (unsigned)((TEST_DURATION_COUNT + 1) / 2));
    printf("RX_GPIO=%u\n", (unsigned)RX_GPIO);

    ESP_ERROR_CHECK(ac_ir_init());

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(RX_GPIO, rx_isr_handler, NULL));

    s_edge_count = 0;
    s_edge_overflow = false;

    ac_ir_frame_t frame = {
        .durations = s_test_durations,
        .count = TEST_DURATION_COUNT,
    };

    /*
     * This is the real, unmodified production entry point. It is
     * synchronous (rmt_tx_wait_all_done() internally), so it only
     * returns once the physical transmission has finished.
     */
    esp_err_t ret = ac_ir_send(&frame);

    /*
     * Captured immediately on return. Used by the Python side to bound
     * the final space duration, which has no following edge to bound it
     * (the waveform simply ends). See waveform_parser.extract_envelope().
     */
    uint32_t tx_done_ts_us = (uint32_t)esp_timer_get_time();

    gpio_isr_handler_remove(RX_GPIO);

    if (ret != ESP_OK) {
        printf("AC_IR_SEND_FAILED=%d\n", (int)ret);
    }

    printf("AC_IR_TRANSPORT_TEST_TX_DONE\n");

    dump_edge_capture(tx_done_ts_us);

    printf("AC_IR_TRANSPORT_TEST_END\n");
}
