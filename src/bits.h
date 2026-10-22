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

/** @brief The low `width` bits of `value` with their bytes in the opposite order: `little_endian`. Only
 * for a whole number of bytes, which is the only place a byte order applies. */
static inline uint64_t bits_swap(uint64_t value, uint64_t width) {
  return width == 0 ? value : __builtin_bswap64(value) >> (64 - width);
}

/* The run loops below are the general case: as many bits as are left in the current byte, at most
 * eight at a time, whatever the alignment. A field that spans at most eight bytes -- every scalar but
 * one of 58 bits or more off a boundary -- is quicker as a number: its bytes gathered big-endian into
 * one word, shifted and masked once. */

static inline uint64_t bits_mask(uint64_t width) {
  return width >= 64 ? UINT64_MAX : (UINT64_C(1) << width) - 1;
}

/**
 * @brief Lays the low `width` bits of `value` into the stream at `offset`, most significant first. OR'd
 * in: for bytes known to be zero there, as the encoder's output is.
 */
static inline void bits_or(uint8_t *out, uint64_t offset, uint64_t width, uint64_t value) {
  uint8_t *at = out + offset / 8;
  const uint64_t end = offset % 8 + width;
  if (end <= 64) {
    const unsigned bytes = (unsigned)((end + 7) / 8);
    uint64_t word = (value & bits_mask(width)) << (bytes * 8 - end);
    for (unsigned i = bytes; i-- > 0;) {
      at[i] |= (uint8_t)word;
      word >>= 8;
    }
    return;
  }
  uint64_t position = offset;
  uint64_t remaining = width;
  while (remaining > 0) {
    uint64_t used = position % 8;
    uint64_t run = 8 - used < remaining ? 8 - used : remaining;
    uint64_t bits = (value >> (remaining - run)) & ((1u << run) - 1);
    out[position / 8] |= (uint8_t)(bits << (8 - used - run));
    position += run;
    remaining -= run;
  }
}

/** @brief As bits_or(), replacing what the field's bits held: for bytes that already hold a frame. */
static inline void bits_put(uint8_t *out, uint64_t offset, uint64_t width, uint64_t value) {
  uint8_t *at = out + offset / 8;
  const uint64_t end = offset % 8 + width;
  if (end <= 64) {
    const unsigned bytes = (unsigned)((end + 7) / 8);
    const unsigned below = (unsigned)(bytes * 8 - end);
    uint64_t mask = bits_mask(width) << below;
    uint64_t word = (value << below) & mask;
    for (unsigned i = bytes; i-- > 0;) {
      at[i] = (uint8_t)((at[i] & ~(uint8_t)mask) | (uint8_t)word);
      mask >>= 8;
      word >>= 8;
    }
    return;
  }
  uint64_t position = offset;
  uint64_t remaining = width;
  while (remaining > 0) {
    uint64_t used = position % 8;
    uint64_t run = 8 - used < remaining ? 8 - used : remaining;
    unsigned shift = (unsigned)(8 - used - run);
    uint8_t mask = (uint8_t)(((1u << run) - 1) << shift);
    uint8_t bits = (uint8_t)(((value >> (remaining - run)) << shift) & mask);
    out[position / 8] = (uint8_t)((out[position / 8] & ~mask) | bits);
    position += run;
    remaining -= run;
  }
}

/** @brief The `width` bits at `offset`, most significant first: the inverse of bits_or(). */
static inline uint64_t bits_get(const uint8_t *in, uint64_t offset, uint64_t width) {
  const uint8_t *at = in + offset / 8;
  /* The usual shapes on a byte boundary are a load each. */
  if (offset % 8 == 0) {
    switch (width) {
    case 8:
      return at[0];
    case 16:
      return (uint64_t)at[0] << 8 | at[1];
    case 32:
      return (uint64_t)at[0] << 24 | (uint64_t)at[1] << 16 | (uint64_t)at[2] << 8 | at[3];
    case 64:
      return bits_load_be64(at);
    default:
      break;
    }
  }
  const uint64_t end = offset % 8 + width;
  if (end <= 64) {
    const unsigned bytes = (unsigned)((end + 7) / 8);
    uint64_t word = 0;
    for (unsigned i = 0; i < bytes; ++i) {
      word = word << 8 | at[i];
    }
    return (word >> (bytes * 8 - end)) & bits_mask(width);
  }
  uint64_t value = 0;
  uint64_t position = offset;
  uint64_t remaining = width;
  while (remaining > 0) {
    uint64_t used = position % 8;
    uint64_t run = 8 - used < remaining ? 8 - used : remaining;
    uint64_t bits = ((uint64_t)in[position / 8] >> (8 - used - run)) & ((1u << run) - 1);
    value = (value << run) | bits;
    position += run;
    remaining -= run;
  }
  return value;
}
