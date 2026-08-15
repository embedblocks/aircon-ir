/*
 * AC IR RMT transport layer -- hardware verification test firmware.
 *
 * Purpose:
 *   Verify that ac_ir_send() physically transmits a known, deterministic
 *   duration sequence in order, without loss/duplication/reordering,
 *   across the custom RMT encoder's multi-invocation path.
 *
 * Physical setup:
 *   Jumper from ac_ir TX GPIO (GPIO10) to RX_GPIO.
 *
 * The RX GPIO captures the raw carrier edges independently.
 * The raw edges stay entirely on the ESP. The test firmware reconstructs
 * the mark/space envelope and sends only the resulting durations to pytest.
 */

#include "esp_attr.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "ac_ir.h"
#include "ac_ir_frame.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "sdkconfig.h"

/* ---------------------------------------------------------------------- */
/* Test configuration                                                     */
/* ---------------------------------------------------------------------- */

#define TEST_DURATION_COUNT   112
#define TEST_START_US         1000
#define TEST_STEP_US          10

#define RX_GPIO               CONFIG_AC_IR_TEST_RX_GPIO

/*
 * Raw carrier capture buffer.
 *
 * 38 kHz carrier produces thousands of edges. Keep the raw capture
 * because it is the independent physical measurement of the TX GPIO.
 */
#define MAX_EDGES             16384

/*
 * Same threshold used by the original Python waveform_parser.py.
 *
 * A gap > 100 us means the carrier has stopped, i.e. a mark/space
 * boundary.
 */
#define CARRIER_GAP_THRESHOLD_US  100

/*
 * Maximum reconstructed durations.
 *
 * Primary test has 112, but give some headroom so a malformed waveform
 * cannot overwrite memory.
 */
#define MAX_DURATIONS         256


/* ---------------------------------------------------------------------- */
/* Deterministic test frame                                               */
/* ---------------------------------------------------------------------- */

static uint16_t s_test_durations[TEST_DURATION_COUNT];

static void build_test_durations(void)
{
    for (int i = 0; i < TEST_DURATION_COUNT; i++) {
        s_test_durations[i] =
            (uint16_t)(TEST_START_US + TEST_STEP_US * i);
    }
}


/* ---------------------------------------------------------------------- */
/* Independent GPIO edge capture                                          */
/* ---------------------------------------------------------------------- */

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


/* ---------------------------------------------------------------------- */
/* Raw-edge -> mark/space envelope                                        */
/* ---------------------------------------------------------------------- */

/*
 * This implements the same envelope reconstruction previously performed
 * by waveform_parser.py.
 *
 * Carrier:
 *
 *       ~38 kHz carrier
 *       ┌─┐ ┌─┐ ┌─┐
 *       │ │ │ │ │ │
 *  ─────┘ └─┘ └─┘ └────
 *
 * Space:
 *
 *  ─────────────────────
 *
 * A gap > CARRIER_GAP_THRESHOLD_US indicates a mark/space boundary.
 *
 * The first rising edge starts the first mark.
 *
 * At a boundary:
 *
 *       falling edge
 *             │
 *             ▼
 *   MARK ─────┐
 *             └──────── SPACE ─────────┐
 *                                      │
 *                                      ▼
 *                                next rising edge
 *
 * The final space is bounded by tx_done_ts_us, exactly as the original
 * Python implementation did.
 */
static uint32_t extract_envelope(
    uint32_t tx_done_ts_us,
    uint16_t *durations,
    uint32_t max_durations)
{
    uint32_t n = s_edge_count;

    if (n == 0) {
        return 0;
    }

    /*
     * The original parser requires the first edge to be rising,
     * because that represents the beginning of the first mark.
     */
    if (s_edge_level[0] != 1) {
        printf("ENVELOPE_ERROR=FIRST_EDGE_NOT_RISING\n");
        printf("FIRST_EDGE_LEVEL=%u\n",
               (unsigned)s_edge_level[0]);
        return 0;
    }

    uint32_t duration_count = 0;

    uint32_t mark_start_ts = s_edge_ts_us[0];

    for (uint32_t i = 0; i < n; i++) {

        uint8_t level = s_edge_level[i];
        uint32_t ts = s_edge_ts_us[i];

        bool is_last_edge = (i == (n - 1));

        uint32_t next_gap = 0;

        if (!is_last_edge) {
            next_gap = s_edge_ts_us[i + 1] - ts;
        }

        bool is_boundary =
            is_last_edge ||
            (next_gap > CARRIER_GAP_THRESHOLD_US);

        /*
         * The Python implementation only closes a mark when the
         * boundary edge is LOW.
         */
        if (is_boundary && level == 0) {

            uint32_t mark_end_ts = ts;

            /* Mark duration */
            if (duration_count >= max_durations) {
                printf("ENVELOPE_OVERFLOW=1\n");
                return duration_count;
            }

            durations[duration_count++] =
                (uint16_t)(mark_end_ts - mark_start_ts);

            if (is_last_edge) {

                /*
                 * Final space has no following edge.
                 * Bound it using the timestamp captured immediately
                 * after ac_ir_send() returned.
                 */
                if (duration_count >= max_durations) {
                    printf("ENVELOPE_OVERFLOW=1\n");
                    return duration_count;
                }

                durations[duration_count++] =
                    (uint16_t)(tx_done_ts_us - mark_end_ts);

            } else {

                /*
                 * Space ends at the next rising edge.
                 */
                uint32_t next_mark_start_ts =
                    s_edge_ts_us[i + 1];

                if (duration_count >= max_durations) {
                    printf("ENVELOPE_OVERFLOW=1\n");
                    return duration_count;
                }

                durations[duration_count++] =
                    (uint16_t)(next_mark_start_ts - mark_end_ts);

                mark_start_ts = next_mark_start_ts;
            }
        }
    }

    return duration_count;
}


/* ---------------------------------------------------------------------- */
/* Compact result output                                                  */
/* ---------------------------------------------------------------------- */

static void print_actual_durations(
    const uint16_t *durations,
    uint32_t count)
{
    printf("ACTUAL_DURATION_COUNT=%u\n", (unsigned)count);

    printf("ACTUAL_DURATIONS=");

    for (uint32_t i = 0; i < count; i++) {
        if (i != 0) {
            printf(",");
        }

        printf("%u", (unsigned)durations[i]);
    }

    printf("\n");
}


/* ---------------------------------------------------------------------- */
/* Test entry point                                                       */
/* ---------------------------------------------------------------------- */

void app_main(void)
{
    build_test_durations();

    printf("AC_IR_TRANSPORT_TEST_START\n");
    printf("DURATION_COUNT=%u\n",
           (unsigned)TEST_DURATION_COUNT);
    printf("EXPECTED_SYMBOL_COUNT=%u\n",
           (unsigned)((TEST_DURATION_COUNT + 1) / 2));
    printf("RX_GPIO=%u\n",
           (unsigned)RX_GPIO);

    /*
     * Initialize the real production ac_ir implementation.
     */
    ESP_ERROR_CHECK(ac_ir_init());

    /*
     * Configure independent GPIO edge capture.
     */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(
        gpio_isr_handler_add(RX_GPIO, rx_isr_handler, NULL)
    );

    s_edge_count = 0;
    s_edge_overflow = false;

    ac_ir_frame_t frame = {
        .durations = s_test_durations,
        .count = TEST_DURATION_COUNT,
    };

    /*
     * Real production API.
     *
     * ac_ir_send() is synchronous, so when it returns the physical
     * transmission is complete.
     */
    esp_err_t ret = ac_ir_send(&frame);

    /*
     * Capture this immediately after TX completion.
     * It is used to bound the final space.
     */
    uint32_t tx_done_ts_us =
        (uint32_t)esp_timer_get_time();

    gpio_isr_handler_remove(RX_GPIO);

    if (ret != ESP_OK) {
        printf("AC_IR_SEND_FAILED=%d\n", (int)ret);
    }

    printf("AC_IR_TRANSPORT_TEST_TX_DONE\n");

    /*
     * Keep these diagnostics, but DO NOT print the raw 5399 edges.
     */
    printf("EDGE_COUNT=%u\n",
           (unsigned)s_edge_count);

    printf("EDGE_OVERFLOW=%u\n",
           s_edge_overflow ? 1u : 0u);

    printf("TX_DONE_TS_US=%u\n",
           (unsigned)tx_done_ts_us);

    /*
     * Reconstruct the envelope locally.
     */
    uint16_t actual_durations[MAX_DURATIONS];

    uint32_t actual_count =
        extract_envelope(
            tx_done_ts_us,
            actual_durations,
            MAX_DURATIONS
        );

    /*
     * Send only the compact result to pytest.
     */
    print_actual_durations(
        actual_durations,
        actual_count
    );

    printf("AC_IR_TRANSPORT_TEST_END\n");
}