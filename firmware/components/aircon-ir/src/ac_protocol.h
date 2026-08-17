#pragma once

#include "esp_err.h"

#include "ac_operation.h"
#include "ac_ir_frame.h"

typedef struct {
    esp_err_t (*init)(void);

    esp_err_t (*encode)(
        const ac_command_t *command,
        ac_ir_frame_t *frame
    );
} ac_protocol_t;

const ac_protocol_t *ac_protocol_get(void);