#pragma once

/**
 * @file
 * @brief Moving bits in stream order, shared by the encoder, the decoder and the frame's accessors.
 *
 * Stream bit i is bit 7 - (i % 8) of byte i / 8 (see encode.c). Read as a big-endian number, eight
 * consecutive stream bytes are 64 stream bits in order, so shifting that number moves bits across byte
 * boundaries exactly as the stream does -- which is what lets a copy off a byte boundary go a word at a
 * time instead of a byte at a time (#105).
 */

#include <stdint.h>
#include <string.h>

/** @brief Eight bytes as the big-endian number they spell, wherever they are in memory. */
static inline uint64_t bits_load_be64(const uint8_t *bytes) {
  uint64_t value;
  memcpy(&value, bytes, sizeof(value));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return value;
#else
  return __builtin_bswap64(value);
#endif
}

/** @brief The inverse of bits_load_be64(). */
static inline void bits_store_be64(uint8_t *bytes, uint64_t value) {
#if !(defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  value = __builtin_bswap64(value);
#endif
  memcpy(bytes, &value, sizeof(value));
}
