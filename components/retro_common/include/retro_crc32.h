/*
 * CRC-32 (IEEE 802.3, the zlib/PNG one). Used for ROM identification and
 * file integrity checks.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start with crc = 0; feed chunks by passing the previous result. */
uint32_t retro_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
