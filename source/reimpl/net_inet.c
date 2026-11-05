/*
 * Pure string conversions for the inet_* imports (offline posture).
 *
 * VitaSDK implements inet_pton/inet_ntop on top of SceNet, which is the last
 * reason the loader would import SceNet once net_offline.c serves every
 * socket and resolver call. IPv4 is converted here in plain C; IPv6 is
 * refused with EAFNOSUPPORT, since with no network there is no IPv6 address
 * the engine could have obtained.
 *
 * MIT license, same terms as the surrounding project.
 */

#include "reimpl/net_inet.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BIONIC_AF_INET 2
#define INET4_TEXT_MAX 16

/* Parses one dotted-quad into network byte order. Returns 1 on success. */
static int inet4_parse(const char *src, uint8_t out[4]) {
    if (src == NULL) {
        return 0;
    }
    for (int part = 0; part < 4; ++part) {
        unsigned int value = 0;
        int digits = 0;
        while (digits < 4 && *src >= '0' && *src <= '9') {
            value = value * 10u + (unsigned int)(*src - '0');
            ++src;
            ++digits;
        }
        if (digits == 0 || digits > 3 || value > 255u) {
            return 0;
        }
        out[part] = (uint8_t)value;
        if (part < 3) {
            if (*src != '.') {
                return 0;
            }
            ++src;
        }
    }
    return *src == '\0';
}

static int inet4_format(const uint8_t in[4], char *dst, size_t size) {
    int written = snprintf(dst, size, "%u.%u.%u.%u",
                           (unsigned int)in[0], (unsigned int)in[1],
                           (unsigned int)in[2], (unsigned int)in[3]);
    return written > 0 && (size_t)written < size;
}

int inet_aton_offline(const char *src, void *dst) {
    uint8_t bytes[4];
    if (!inet4_parse(src, bytes)) {
        return 0;
    }
    if (dst != NULL) {
        uint8_t *out = (uint8_t *)dst;
        for (int i = 0; i < 4; ++i) {
            out[i] = bytes[i];
        }
    }
    return 1;
}

char *inet_ntoa_offline(uint32_t addr_network_order) {
    static char text[INET4_TEXT_MAX];
    const uint8_t *bytes = (const uint8_t *)&addr_network_order;
    if (!inet4_format(bytes, text, sizeof(text))) {
        text[0] = '\0';
    }
    return text;
}

int inet_pton_offline(int af, const char *src, void *dst) {
    if (af != BIONIC_AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton_offline(src, dst);
}

const char *inet_ntop_offline(int af, const void *src, char *dst,
                              unsigned int size) {
    if (af != BIONIC_AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }
    if (src == NULL || dst == NULL ||
        !inet4_format((const uint8_t *)src, dst, size)) {
        errno = ENOSPC;
        return NULL;
    }
    return dst;
}
