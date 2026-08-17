#include "ac_operation.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"



static const char *TAG = "app";

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
     * Turn AC ON:
     * Cool, 24 C, high fan.
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

    ret = ac_set_operation(&command);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set AC operation: %s",
                 esp_err_to_name(ret));
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    /*
     * Later, change the complete desired operation.
     */
    command.temperature = 22;
    command.fan = AC_FAN_MEDIUM;

    ret = ac_set_operation(&command);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to change AC operation: %s",
                 esp_err_to_name(ret));
        return;
    }

    

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch AC off: %s",
                 esp_err_to_name(ret));
        return;
    }
}