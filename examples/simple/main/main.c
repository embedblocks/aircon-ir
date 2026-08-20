#include "ac_operation.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"



static const char *TAG = "app";

static esp_err_t apply(const char *label, const ac_command_t *command)
{
    ESP_LOGI(TAG, "-> %s", label);

    esp_err_t ret = ac_set_operation(command);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply '%s': %s",
                 label, esp_err_to_name(ret));
    }

    return ret;
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== BOOT === reset_reason=%d", esp_reset_reason());

    esp_err_t ret = ac_operation_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AC initialization failed: %s",
                 esp_err_to_name(ret));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));


    /*
     * 1) Turn the AC on: cool, 24 C, high fan.
     */
    ac_command_t command = {
        .power = true,
        .temperature = 24,
        .mode = AC_MODE_COOL,
        .fan = AC_FAN_HIGH,

        .turbo = false,
        .quiet = false,

        .swing_v = AC_SWING_V_OFF,
        .swing_h = AC_SWING_H_MIDDLE,

        .sleep = false,
        .health = false,
    };

    if (apply("power on, cool 24C, fan high", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));


    /*
     * 2) Change the target temperature and fan speed.
     *
     * ac_command_t always describes the complete desired state, so
     * only the fields that actually change need to be touched —
     * the rest of `command` is simply re-sent as-is.
     */
    command.temperature = 22;
    command.fan = AC_FAN_MEDIUM;

    if (apply("temperature 22C, fan medium", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));


    /*
     * 3) Cycle through modes. Power, mode, temperature and fan speed
     *    are the fields that were actually captured from a real
     *    remote and verified against the generated waveform for
     *    every protocol in this component (see README: "Data
     *    Provenance") -- these are the fields you can rely on.
     */
    command.mode = AC_MODE_DRY;
    command.fan = AC_FAN_AUTO;

    if (apply("mode dry, fan auto", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    command.mode = AC_MODE_HEAT;
    command.temperature = 26;
    command.fan = AC_FAN_LOW;

    if (apply("mode heat, 26C, fan low", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    command.mode = AC_MODE_AUTO;
    command.fan = AC_FAN_AUTO;

    if (apply("mode auto, fan auto", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));


    /*
     * 4) Turbo mode: fast cooling.
     */
    command.mode = AC_MODE_COOL;
    command.temperature = 22;
    command.turbo = true;

    if (apply("mode cool, 22C, turbo on [unverified] ", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));


    /*
     * 5) Turbo back off, quiet mode instead.
     */
    command.turbo = false;
    command.quiet = true;

    if (apply("turbo off, quiet on [unverified]", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));


    /*
     * 6) Health (air purification) and sleep mode.
     */
    command.quiet = false;
    command.health = true;
    command.sleep = true;

    if (apply("health on, sleep on [unverified]", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));


    /*
     * 7) UNVERIFIED: swing_v / swing_h.
     *
     * Swing was not part of the original remote-capture set for
     * any protocol in this component. Midea ignores these fields
     * entirely; Haier/Gree encode them, but with best-effort bit
     * values that have not been confirmed against a real capture.
     * Shown here for API completeness only -- do not assume this
     * moves the louvers correctly without verifying it yourself
     * against your specific AC (see README: "Data Provenance").
     */
    command.health = false;
    command.sleep = false;
    command.swing_v = AC_SWING_V_AUTO;
    command.swing_h = AC_SWING_H_AUTO;

    if (apply("swing v/h auto [unverified]", &command) != ESP_OK) {
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));


    /*
     * 8) Switch the AC off using the dedicated power-off helper,
     *    rather than building an ac_command_t with .power = false.
     */
    ESP_LOGI(TAG, "-> power off");

    ret = ac_switch_off();

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch AC off: %s",
                 esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "=== DONE ===");
}