#include "ac_operation.h"

#include "ac_ir.h"
#include "ac_protocol.h"

static const ac_protocol_t *protocol = NULL;

static ac_command_t current_command = {
    .power = false,
    .temperature = 25,
    .mode = AC_MODE_COOL,
    .fan = AC_FAN_AUTO,

    .turbo = false,
    .quiet = false,

    .swing_v = AC_SWING_V_OFF,
    .swing_h = AC_SWING_H_MIDDLE,

    .sleep = false,
    .health = false,
};


esp_err_t ac_operation_init(void)
{
    protocol = ac_protocol_get();

    if (protocol == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = ac_ir_init();

    if (ret != ESP_OK) {
        return ret;
    }

    if (protocol->init != NULL) {
        ret = protocol->init();

        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}


esp_err_t ac_set_operation(
    const ac_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (protocol == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ac_ir_frame_t frame = {0};

    esp_err_t ret =
        protocol->encode(command, &frame);

    if (ret != ESP_OK) {
        return ret;
    }

    ret = ac_ir_send(&frame);

    if (ret != ESP_OK) {
        return ret;
    }

    current_command = *command;

    return ESP_OK;
}


esp_err_t ac_switch_off(void)
{
    ac_command_t command = current_command;

    command.power = false;

    return ac_set_operation(&command);
}