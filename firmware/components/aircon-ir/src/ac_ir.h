#pragma once

#include "esp_err.h"

#include "ac_ir_frame.h"

esp_err_t ac_ir_init(void);

esp_err_t ac_ir_send(const ac_ir_frame_t *frame);