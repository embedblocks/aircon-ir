#include "ac_protocol.h"

#include "protocols/midea.h"



const ac_protocol_t *ac_protocol_get(void)
{
    return &midea_protocol;
}