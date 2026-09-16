#pragma once

#include <stdint.h>

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    uint8_t sin_zero[8];
};

uint16_t htons(uint16_t value);
