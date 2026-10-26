#ifndef QUIREWISE_BYTE_UTILS_H
#define QUIREWISE_BYTE_UTILS_H

/* Small, dependency-free primitives shared by format adapters.  Keep these
 * header-only: they are simple enough to inline and should not create another
 * registry or link-time dependency for low-level readers and writers. */

#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

static inline int qw_checked_size_add(size_t left, size_t right, size_t *out)
{
    if (!out || right > SIZE_MAX - left) return 0;
    *out = left + right;
    return 1;
}

static inline int qw_checked_size_mul(size_t left, size_t right, size_t *out)
{
    if (!out || (left != 0u && right > SIZE_MAX / left)) return 0;
    *out = left * right;
    return 1;
}

static inline uint16_t qw_read_u16_le(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static inline uint32_t qw_read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static inline uint64_t qw_read_u64_le(const uint8_t *data)
{
    return (uint64_t)qw_read_u32_le(data) |
           ((uint64_t)qw_read_u32_le(data + 4u) << 32);
}

static inline uint16_t qw_read_u16_be(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
}

static inline uint32_t qw_read_u32_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static inline int32_t qw_read_i32_le(const uint8_t *data)
{
    uint32_t raw = qw_read_u32_le(data);
    int32_t value;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static inline uint64_t qw_read_u64_be(const uint8_t *data)
{
    return ((uint64_t)qw_read_u32_be(data) << 32) |
           (uint64_t)qw_read_u32_be(data + 4u);
}

static inline void qw_write_u16_le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static inline void qw_write_u32_le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static inline void qw_write_u16_be(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

static inline void qw_write_u32_be(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

static inline unsigned char qw_ascii_lower(unsigned char value)
{
    return value >= (unsigned char)'A' && value <= (unsigned char)'Z'
        ? (unsigned char)(value + ((unsigned char)'a' - (unsigned char)'A'))
        : value;
}

static inline int qw_ascii_isspace(unsigned char value)
{
    return value == (unsigned char)' ' || value == (unsigned char)'\t' ||
           value == (unsigned char)'\r' || value == (unsigned char)'\n' ||
           value == (unsigned char)'\f' || value == (unsigned char)'\v';
}

#endif
