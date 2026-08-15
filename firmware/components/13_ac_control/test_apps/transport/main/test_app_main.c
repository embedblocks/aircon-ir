/*
 * AC IR transport characterization test application.
 *
 * IMPORTANT:
 *   ac_ir.c is NOT modified by this test.
 *
 * The firmware stays running and accepts:
 *
 *   LOAD UNIFORM <count> <start_us> <step_us>
 *   LOAD LEADER_BURST <leader_mark> <leader_space>
 *                       <burst_pairs> <bit_mark> <bit_space>
 *   RUN
 *
 * This allows pytest to characterize the fixed ac_ir implementation
 * against many different waveform shapes after a single flash.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ac_ir.h"
#include "ac_ir_frame.h"

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


/* ---------------------------------------------------------------------- */
/* Configuration                                                          */
/* ---------------------------------------------------------------------- */

#define RX_GPIO CONFIG_AC_IR_TEST_RX_GPIO

#define MAX_TEST_DURATIONS 512
#define MAX_EDGES           16384
#define MAX_COMMAND_LENGTH 160

#define CARRIER_GAP_THRESHOLD_US 100


/* ---------------------------------------------------------------------- */
/* Runtime waveform                                                       */
/* ---------------------------------------------------------------------- */

static uint16_t s_test_durations[MAX_TEST_DURATIONS];
static uint32_t s_test_duration_count = 0;


/* ---------------------------------------------------------------------- */
/* Raw GPIO capture                                                       */
/* ---------------------------------------------------------------------- */

static volatile uint32_t s_edge_ts_us[MAX_EDGES];
static volatile uint8_t  s_edge_level[MAX_EDGES];

static volatile uint32_t s_edge_count = 0;
static volatile bool s_edge_overflow = false;


static void IRAM_ATTR rx_isr_handler(void *arg)
{
    (void)arg;

    uint32_t now_us = (uint32_t)esp_timer_get_time();
    uint8_t level = (uint8_t)gpio_get_level(RX_GPIO);

    uint32_t index = s_edge_count;

    if (index < MAX_EDGES) {
        s_edge_ts_us[index] = now_us;
        s_edge_level[index] = level;
        s_edge_count = index + 1;
    } else {
        s_edge_overflow = true;
    }
}


/* ---------------------------------------------------------------------- */
/* Waveform generators                                                    */
/* ---------------------------------------------------------------------- */

static bool generate_uniform(
    uint32_t count,
    uint32_t start_us,
    uint32_t step_us)
{
    if (count == 0 || count > MAX_TEST_DURATIONS) {
        return false;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t duration =
            start_us + step_us * i;

        if (duration > UINT16_MAX) {
            return false;
        }

        s_test_durations[i] = (uint16_t)duration;
    }

    s_test_duration_count = count;

    return true;
}


static bool generate_leader_burst(
    uint32_t leader_mark_us,
    uint32_t leader_space_us,
    uint32_t burst_pairs,
    uint32_t bit_mark_us,
    uint32_t bit_space_us)
{
    uint32_t count = 2 + burst_pairs * 2;

    if (count == 0 || count > MAX_TEST_DURATIONS) {
        return false;
    }

    if (leader_mark_us > UINT16_MAX ||
        leader_space_us > UINT16_MAX ||
        bit_mark_us > UINT16_MAX ||
        bit_space_us > UINT16_MAX) {
        return false;
    }

    s_test_durations[0] =
        (uint16_t)leader_mark_us;

    s_test_durations[1] =
        (uint16_t)leader_space_us;

    for (uint32_t i = 0; i < burst_pairs; i++) {
        s_test_durations[2 + i * 2] =
            (uint16_t)bit_mark_us;

        s_test_durations[3 + i * 2] =
            (uint16_t)bit_space_us;
    }

    s_test_duration_count = count;

    return true;
}


/* ---------------------------------------------------------------------- */
/* Command parsing                                                        */
/* ---------------------------------------------------------------------- */

static void process_load_command(char *line)
{
    char shape[32];

    unsigned int a;
    unsigned int b;
    unsigned int c;
    unsigned int d;
    unsigned int e;

    /*
     * LOAD UNIFORM <count> <start> <step>
     */
    if (sscanf(
            line,
            "LOAD %31s %u %u %u",
            shape,
            &a,
            &b,
            &c) == 4) {

        if (strcmp(shape, "UNIFORM") == 0) {

            if (generate_uniform(a, b, c)) {
                printf(
                    "LOAD_OK UNIFORM COUNT=%u\n",
                    (unsigned)s_test_duration_count
                );
            } else {
                printf("LOAD_ERR invalid_uniform\n");
            }

            return;
        }
    }

    /*
     * LOAD LEADER_BURST
     *      <leader_mark>
     *      <leader_space>
     *      <burst_pairs>
     *      <bit_mark>
     *      <bit_space>
     */
    if (sscanf(
            line,
            "LOAD %31s %u %u %u %u %u",
            shape,
            &a,
            &b,
            &c,
            &d,
            &e) == 6) {

        if (strcmp(shape, "LEADER_BURST") == 0) {

            if (generate_leader_burst(
                    a, b, c, d, e)) {

                printf(
                    "LOAD_OK LEADER_BURST COUNT=%u\n",
                    (unsigned)s_test_duration_count
                );
            } else {
                printf("LOAD_ERR invalid_leader_burst\n");
            }

            return;
        }
    }

    printf("LOAD_ERR unknown_command\n");
}


/* ---------------------------------------------------------------------- */
/* Envelope extraction                                                    */
/* ---------------------------------------------------------------------- */

static uint32_t extract_envelope(
    uint32_t tx_done_ts_us,
    uint16_t *durations,
    uint32_t max_durations)
{
    uint32_t n = s_edge_count;

    if (n == 0) {
        return 0;
    }

    if (s_edge_level[0] != 1) {
        printf("ENVELOPE_ERROR=FIRST_EDGE_NOT_RISING\n");
        printf(
            "FIRST_EDGE_LEVEL=%u\n",
            (unsigned)s_edge_level[0]
        );
        return 0;
    }

    uint32_t duration_count = 0;

    uint32_t mark_start_ts =
        s_edge_ts_us[0];

    for (uint32_t i = 0; i < n; i++) {

        uint8_t level =
            s_edge_level[i];

        uint32_t ts =
            s_edge_ts_us[i];

        if (level != 0) {
            continue;
        }

        /*
         * Find the next edge after this falling edge.
         */
        bool last_edge = (i == n - 1);

        uint32_t next_gap = 0;

        if (!last_edge) {
            next_gap =
                s_edge_ts_us[i + 1] - ts;
        }

        /*
         * A large gap after a falling edge means:
         *
         *     carrier/mark ended
         *     space follows
         *
         * Therefore this falling edge terminates one
         * mark/space pair.
         */

        if (!last_edge &&
            next_gap > CARRIER_GAP_THRESHOLD_US) {

            printf(
                "ENVELOPE_GAP symbol=%lu i=%lu gap=%lu\n",
                (unsigned long)(duration_count / 2),
                (unsigned long)i,
                (unsigned long)next_gap
            );
        }

       if (!last_edge &&
            next_gap <= CARRIER_GAP_THRESHOLD_US) {
            continue;
        }

        /*
         * We have reached the end of a mark.
         */
        if (duration_count + 2 > max_durations) {
            printf("ENVELOPE_OVERFLOW=1\n");
            return duration_count;
        }

        uint32_t mark_end_ts = ts;

        durations[duration_count++] =
            (uint16_t)(mark_end_ts - mark_start_ts);

        /*
         * If there is another edge after the gap,
         * that edge is the beginning of the next mark.
         */
        if (!last_edge) {

            uint32_t next_mark_start_ts =
                s_edge_ts_us[i + 1];

            durations[duration_count++] =
                (uint16_t)(
                    next_mark_start_ts - mark_end_ts
                );

            mark_start_ts =
                next_mark_start_ts;
        }
        else {
            /*
             * Final space extends from the final falling
             * edge until TX completion.
             */
            durations[duration_count++] =
                (uint16_t)(
                    tx_done_ts_us - mark_end_ts
                );
        }
    }

    return duration_count;
}

/* ---------------------------------------------------------------------- */
/* Compact waveform output                                                */
/* ---------------------------------------------------------------------- */

static void print_actual_durations(
    const uint16_t *durations,
    uint32_t count)
{
    printf(
        "ACTUAL_DURATION_COUNT=%u\n",
        (unsigned)count
    );

    printf("ACTUAL_DURATIONS=");

    for (uint32_t i = 0; i < count; i++) {

        if (i != 0) {
            printf(",");
        }

        printf(
            "%u",
            (unsigned)durations[i]
        );
    }

    printf("\n");
}



///////////////////////////////

static void dump_edge_summary(void)
{
    uint32_t count = s_edge_count;

    printf("RAW_EDGE_SUMMARY_START\n");

    printf("RAW_EDGE_COUNT=%u\n", (unsigned)count);

    uint32_t first_count = count < 20 ? count : 20;

    printf("RAW_EDGE_HEAD\n");
    for (uint32_t i = 0; i < first_count; i++) {
        printf("EDGE[%u]=%u,%u\n",
               (unsigned)i,
               (unsigned)s_edge_level[i],
               (unsigned)s_edge_ts_us[i]);
    }

    printf("RAW_EDGE_TAIL\n");

    uint32_t start = count > 20 ? count - 20 : 0;

    for (uint32_t i = start; i < count; i++) {
        printf("EDGE[%u]=%u,%u\n",
               (unsigned)i,
               (unsigned)s_edge_level[i],
               (unsigned)s_edge_ts_us[i]);
    }

    printf("RAW_EDGE_SUMMARY_END\n");
}

/* ---------------------------------------------------------------------- */
/* Execute one RUN                                                        */
/* ---------------------------------------------------------------------- */

static void run_test(void)
{
    if (s_test_duration_count == 0) {
        printf("RUN_ERR no_waveform_loaded\n");
        return;
    }

    /*
     * Reset capture.
     */
    s_edge_count = 0;
    s_edge_overflow = false;

    printf("AC_IR_TRANSPORT_TEST_START\n");

    printf(
        "DURATION_COUNT=%u\n",
        (unsigned)s_test_duration_count
    );

    printf(
        "EXPECTED_SYMBOL_COUNT=%u\n",
        (unsigned)((s_test_duration_count + 1) / 2)
    );

    printf(
        "RX_GPIO=%u\n",
        (unsigned)RX_GPIO
    );

    ac_ir_frame_t frame = {
        .durations = s_test_durations,
        .count = s_test_duration_count,
    };

    esp_err_t ret =
        ac_ir_send(&frame);
    //ac_ir_debug_dump_encoder();

    uint32_t tx_done_ts_us =
        (uint32_t)esp_timer_get_time();

    /*
     * Stop capturing immediately after ac_ir_send() returns.
     */

    dump_edge_summary();
    gpio_intr_disable(RX_GPIO);

    printf("AC_IR_TRANSPORT_TEST_TX_DONE\n");

    printf(
        "AC_IR_SEND_RESULT=%d\n",
        (int)ret
    );

    printf(
        "EDGE_COUNT=%u\n",
        (unsigned)s_edge_count
    );

    printf(
        "EDGE_OVERFLOW=%u\n",
        s_edge_overflow ? 1u : 0u
    );

    printf(
        "TX_DONE_TS_US=%u\n",
        (unsigned)tx_done_ts_us
    );

    /*
     * If ac_ir_send() itself failed, don't pretend the waveform
     * is a valid transport result.
     */
    if (ret != ESP_OK) {

        printf(
            "AC_IR_SEND_FAILED=%d\n",
            (int)ret
        );

        printf(
            "ACTUAL_DURATION_COUNT=0\n"
        );

        printf(
            "ACTUAL_DURATIONS=\n"
        );

        printf(
            "AC_IR_TRANSPORT_TEST_END\n"
        );

        gpio_intr_enable(RX_GPIO);

        return;
    }

    uint16_t actual_durations[MAX_TEST_DURATIONS];
    

    uint32_t actual_count =
        extract_envelope(
            tx_done_ts_us,
            actual_durations,
            MAX_TEST_DURATIONS
        );

    print_actual_durations(
        actual_durations,
        actual_count
    );

    printf(
        "AC_IR_TRANSPORT_TEST_END\n"
    );

    gpio_intr_enable(RX_GPIO);
}



/* ---------------------------------------------------------------------- */
/* Main                                                                   */
/* ---------------------------------------------------------------------- */

void app_main(void)
{
    /*
     * Configure RX GPIO.
     */
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << RX_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };

    ESP_ERROR_CHECK(
        gpio_config(&io_conf)
    );

    ESP_ERROR_CHECK(
        gpio_install_isr_service(0)
    );

    ESP_ERROR_CHECK(
        gpio_isr_handler_add(
            RX_GPIO,
            rx_isr_handler,
            NULL
        )
    );

    /*
     * Initialize the real production implementation.
     */
    ESP_ERROR_CHECK(
        ac_ir_init()
    );

    printf("AC_IR_TEST_READY\n");

    /*
     * Persistent command loop.
     *
     * The ESP is flashed once. pytest can then send LOAD/RUN commands
     * repeatedly without rebooting or reflashing.
     */
    char line[MAX_COMMAND_LENGTH];

    while (true) {

        if (fgets(line, sizeof(line), stdin) == NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        /*
         * Remove CR/LF.
         */
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "LOAD ", 5) == 0) {

            process_load_command(line);

        } else if (strcmp(line, "RUN") == 0) {

            run_test();

        } else if (line[0] != '\0') {

            printf(
                "CMD_ERR unknown_command\n"
            );
        }
    }
}