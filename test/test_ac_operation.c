#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "unity.h"
#include "ac_operation.h" // Your public header

static const char *TAG = "AC_REMOTE";

// Static state to act as the remote's memory between test cases
static ac_command_t current_state;
static bool is_initialized = false;

// Helper to map fan enum to string for logging
static const char* fan_to_string(ac_fan_t fan) {
    switch (fan) {
        case AC_FAN_LOW: return "LOW";
        case AC_FAN_MEDIUM: return "MEDIUM";
        case AC_FAN_HIGH: return "HIGH";
        case AC_FAN_AUTO: return "AUTO";
        default: return "UNKNOWN";
    }
}

// Helper to map mode enum to string for logging
static const char* mode_to_string(ac_mode_t mode) {
    switch (mode) {
        case AC_MODE_COOL: return "COOL";
        case AC_MODE_HEAT: return "HEAT";
        case AC_MODE_AUTO: return "AUTO";
        case AC_MODE_FAN: return "FAN";
        case AC_MODE_DRY: return "DRY";
        default: return "UNKNOWN";
    }
}

// Prints the current state to the console like an LCD screen
static void print_remote_display() {
    ESP_LOGI(TAG, "┌────────────────────────────┐");
    if (current_state.power) {
        ESP_LOGI(TAG, "│ POWER: ON                  │");
        ESP_LOGI(TAG, "│ MODE : %-5s                │", mode_to_string(current_state.mode));
        ESP_LOGI(TAG, "│ TEMP : %-2d C                │", current_state.temperature);
        ESP_LOGI(TAG, "│ FAN  : %-6s               │", fan_to_string(current_state.fan));
    } else {
        ESP_LOGI(TAG, "│ POWER: OFF                 │");
    }
    ESP_LOGI(TAG, "└────────────────────────────┘");
}

// Unity runs this before every selected test case
void setUp(void) {
    if (!is_initialized) {
        ac_operation_init();
        // Default state as requested: Cool, 26C, Auto Fan, Power Off
        memset(&current_state, 0, sizeof(ac_command_t));
        current_state.mode = AC_MODE_COOL;
        current_state.temperature = 26;
        current_state.fan = AC_FAN_AUTO;
        is_initialized = true;
    }
}

void tearDown(void) {
    // Small delay to allow IR transmission to complete before next command
    vTaskDelay(pdMS_TO_TICKS(200));
}

// ─────────────────────────────────────────────────────────────────────────────
// REMOTE CONTROL BUTTONS
// ─────────────────────────────────────────────────────────────────────────────

/**
 * [1] Power ON
 * Turns the AC on using the default settings (Cool, 26C, Auto Fan).
 */
TEST_CASE("Power ON", "[remote]") {
    current_state.power = true;
    current_state.mode = AC_MODE_COOL;
    current_state.temperature = 26;
    current_state.fan = AC_FAN_AUTO;
    
    print_remote_display();
    esp_err_t ret = ac_set_operation(&current_state);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * [2] Power OFF
 * Turns the AC off using the normal power-off operation.
 */
TEST_CASE("Power OFF", "[remote]") {
    current_state.power = false;
    
    print_remote_display();
    esp_err_t ret = ac_switch_off();
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * [3] Set Mode: COOL
 */
TEST_CASE("Set Mode: COOL", "[remote]") {
    current_state.power = true; // Changing a setting usually implies turning it on
    current_state.mode = AC_MODE_COOL;
    
    print_remote_display();
    esp_err_t ret = ac_set_operation(&current_state);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * [4] Set Mode: HEAT
 */
TEST_CASE("Set Mode: HEAT", "[remote]") {
    current_state.power = true;
    current_state.mode = AC_MODE_HEAT;
    
    print_remote_display();
    esp_err_t ret = ac_set_operation(&current_state);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * [5] Increase Fan Speed
 * Cycles: AUTO -> LOW -> MEDIUM -> HIGH -> AUTO
 */
TEST_CASE("Increase Fan Speed", "[remote]") {
    current_state.power = true;
    
    // Cycle fan speed
    switch (current_state.fan) {
        case AC_FAN_AUTO: current_state.fan = AC_FAN_LOW; break;
        case AC_FAN_LOW: current_state.fan = AC_FAN_MEDIUM; break;
        case AC_FAN_MEDIUM: current_state.fan = AC_FAN_HIGH; break;
        case AC_FAN_HIGH: current_state.fan = AC_FAN_AUTO; break;
        default: current_state.fan = AC_FAN_AUTO; break;
    }
    
    print_remote_display();
    esp_err_t ret = ac_set_operation(&current_state);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * [6] Decrease Fan Speed
 * Cycles: AUTO -> HIGH -> MEDIUM -> LOW -> AUTO
 */
TEST_CASE("Decrease Fan Speed", "[remote]") {
    current_state.power = true;
    
    // Cycle fan speed backwards
    switch (current_state.fan) {
        case AC_FAN_AUTO: current_state.fan = AC_FAN_HIGH; break;
        case AC_FAN_LOW: current_state.fan = AC_FAN_AUTO; break;
        case AC_FAN_MEDIUM: current_state.fan = AC_FAN_LOW; break;
        case AC_FAN_HIGH: current_state.fan = AC_FAN_MEDIUM; break;
        default: current_state.fan = AC_FAN_AUTO; break;
    }
    
    print_remote_display();
    esp_err_t ret = ac_set_operation(&current_state);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * [7] Increase Temperature
 * Max limit: 30C
 */
TEST_CASE("Increase Temperature", "[remote]") {
    current_state.power = true;
    
    if (current_state.temperature < 30) {
        current_state.temperature++;
    } else {
        ESP_LOGW(TAG, "Already at max temperature (30C)");
    }
    
    print_remote_display();
    esp_err_t ret = ac_set_operation(&current_state);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}

/**
 * [8] Decrease Temperature
 * Min limit: 17C
 */
TEST_CASE("Decrease Temperature", "[remote]") {
    current_state.power = true;
    
    if (current_state.temperature > 17) {
        current_state.temperature--;
    } else {
        ESP_LOGW(TAG, "Already at min temperature (17C)");
    }
    
    print_remote_display();
    esp_err_t ret = ac_set_operation(&current_state);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
}