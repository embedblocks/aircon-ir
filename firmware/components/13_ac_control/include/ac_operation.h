#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    AC_MODE_AUTO,
    AC_MODE_COOL,
    AC_MODE_DRY,
    AC_MODE_HEAT,
    AC_MODE_FAN,
} ac_mode_t;

typedef enum {
    AC_FAN_AUTO,
    AC_FAN_LOW,
    AC_FAN_MEDIUM,
    AC_FAN_HIGH,
} ac_fan_t;

typedef enum {
    AC_SWING_V_OFF,
    AC_SWING_V_AUTO,
    AC_SWING_V_TOP,
    AC_SWING_V_MIDDLE,
    AC_SWING_V_BOTTOM,
    AC_SWING_V_DOWN,
} ac_swing_v_t;

typedef enum {
    AC_SWING_H_MIDDLE,
    AC_SWING_H_LEFT_MAX,
    AC_SWING_H_LEFT,
    AC_SWING_H_RIGHT,
    AC_SWING_H_RIGHT_MAX,
    AC_SWING_H_AUTO,
} ac_swing_h_t;

typedef struct {
    bool power;

    uint8_t temperature;
    ac_mode_t mode;
    ac_fan_t fan;

    bool turbo;
    bool quiet;

    ac_swing_v_t swing_v;
    ac_swing_h_t swing_h;

    bool sleep;
    bool health;
} ac_command_t;

/**
 * Initialize the AC control component.
 */
esp_err_t ac_operation_init(void);

/**
 * Set the complete desired AC operating state.
 */
esp_err_t ac_set_operation(const ac_command_t *command);

/**
 * Switch the AC off using the normal AC power-off operation.
 */
esp_err_t ac_switch_off(void);