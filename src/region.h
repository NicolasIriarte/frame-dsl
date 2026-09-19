#pragma once

/**
 * @file
 * @brief A bump allocator freed in one go.
 *
 * The AST and the strings in it live exactly as long as a load: built by the parser, read by the
 * checks, thrown away once a definition is compiled. Nothing in it is freed individually, so
 * tracking individual lifetimes would be bookkeeping in exchange for nothing.
 *
 * This is not the packet arena. That one is sized at load time and lives for the definition; this
 * one is scaffolding that the definition is built on and then discards.
 */

#include <stddef.h>

typedef struct Region Region;

Region *region_new(void);

/**
 * @brief Allocate `size` bytes from `region`.
 * @param region The region to allocate from.
 * @param size How many bytes.
 * @return NULL only when out of memory. Memory is zeroed and aligned for any type.
 */
void *region_alloc(Region *region, size_t size);

/**
 * @brief Copies `len` bytes and terminates.
 *
 * `text` need not be terminated itself, which is what the lexer hands over: a pointer into the
 * source and a length.
 *
 * @param region The region to allocate from.
 * @param text The bytes to copy.
 * @param len How many bytes.
 * @return The terminated copy.
 */
char *region_strndup(Region *region, const char *text, size_t len);

void region_free(Region *region);
