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

/**
 * @brief The low `width` bits of `value` in the opposite order, 1 <= width <= 64 (#108).
 *
 * Laying a value down `lsb_first` puts its bit j at the field's stream bit j; laying it `msb_first`
 * puts its bit width-1-j there. So `lsb_first` is `msb_first` of the reversed value, at every offset,
 * and the one fast path serves both orders. Swapping halves of ever smaller pieces reverses each byte,
 * and a byte swap reverses the bytes.
 */
static inline uint64_t bits_reverse(uint64_t value, uint64_t width) {
  value = (value >> 1 & UINT64_C(0x5555555555555555)) | (value & UINT64_C(0x5555555555555555)) << 1;
  value = (value >> 2 & UINT64_C(0x3333333333333333)) | (value & UINT64_C(0x3333333333333333)) << 2;
  value = (value >> 4 & UINT64_C(0x0F0F0F0F0F0F0F0F)) | (value & UINT64_C(0x0F0F0F0F0F0F0F0F)) << 4;
  return __builtin_bswap64(value) >> (64 - width);
}
