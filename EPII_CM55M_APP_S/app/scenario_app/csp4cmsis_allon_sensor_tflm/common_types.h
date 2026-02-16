// common_types.h
#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <stdint.h>

struct trigger_t {};

struct frame_t {
    uint32_t index;
    uint32_t jpeg_addr;
    uint32_t jpeg_sz;
};

struct result_t {
    uint32_t frame_index;
    int8_t prediction;
};

#endif // COMMON_TYPES_H
