#include "ac_protocol.h"

#include "protocols/haier.h"

const ac_protocol_t *ac_protocol_get(void)
{
    return &haier_protocol;
}