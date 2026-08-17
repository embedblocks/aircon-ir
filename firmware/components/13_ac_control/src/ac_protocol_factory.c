#include "ac_protocol.h"

#include "protocols/gree.h"



const ac_protocol_t *ac_protocol_get(void)
{
    return &gree_protocol;
}