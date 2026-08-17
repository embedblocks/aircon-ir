#include "ac_protocol.h"
#include "protocols/midea.h"
#include "protocols/gree.h"
#include "protocols/haier.h"

const ac_protocol_t *ac_protocol_get(void)
{
#if CONFIG_AC_PROTOCOL_MIDEA
    return &midea_protocol;
#elif CONFIG_AC_PROTOCOL_GREE
    return &gree_protocol;
#elif CONFIG_AC_PROTOCOL_HAIER
    return &haier_protocol;
#else
#error "No AC protocol selected"
#endif
}