/* Halcyon OS - small helpers shared by the network card drivers */
#ifndef NET_DRIVERS_NIC_H
#define NET_DRIVERS_NIC_H
#include "core/types.h"

/* The Ethernet CRC-32 computed most significant bit first, as the
 * multicast hash filters of Realtek (and other) chips use it: the top 6 bits
 * select one of 64 filter bits. */
static inline u32 nic_crc32_be(const u8 *p, int n) {
    u32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++)
        for (int b = 0; b < 8; b++) {
            u32 bit = ((u32)p[i] >> b) & 1;
            u32 carry = (crc >> 31) ^ bit;
            crc <<= 1;
            if (carry) crc ^= 0x04C11DB7u;
        }
    return crc;
}

/* Intel multicast table array (128 x 32 bits) index with RCTL.MO = 00: the
 * upper 12 bits of the address (bits 47:36) */
static inline u32 nic_intel_mta_hash(const u8 *mac) {
    return ((u32)(mac[4] >> 4) | ((u32)mac[5] << 4)) & 0xFFF;
}

#endif
