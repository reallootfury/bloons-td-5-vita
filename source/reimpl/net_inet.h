/*
 * Pure inet_* string conversions. See net_inet.c.
 */

#ifndef BTD5_NET_INET_H
#define BTD5_NET_INET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int inet_aton_offline(const char *src, void *dst);
char *inet_ntoa_offline(uint32_t addr_network_order);
int inet_pton_offline(int af, const char *src, void *dst);
const char *inet_ntop_offline(int af, const void *src, char *dst,
                              unsigned int size);

#ifdef __cplusplus
};
#endif

#endif // BTD5_NET_INET_H
