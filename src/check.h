#pragma once

/* The load-time semantic pass: the twelve checks in docs/design.md, and the layout they need.
 *
 * These are what make a definition file trustworthy. Every one of them turns a mistake into a
 * message at startup rather than a wrong frame on a live link, which is the whole argument for
 * loading a description at run time instead of compiling one in. */

#include <stdbool.h>
#include <stdint.h>

#include "ast.h"
#include "diag.h"

/* A field of this width or less is a scalar; anything wider is a bit string read and written as
 * bytes. The rule is the width alone -- nothing else has to be declared. */
#define FRAMEDSL_SCALAR_MAX_BITS 64

typedef struct {
  /* Width depends on another field's value, so it is only known per frame. */
  bool variable;
  /* The width, when it is not variable. */
  uint64_t width_bits;
  /* The declared bound, when it is. This is what the arena reserves. */
  uint64_t max_bits;

  /* Everything after a variable-width field sits somewhere nobody knows until decode time. */
  bool offset_known;
  uint64_t offset_bits;

  /* How much the arena reserves: the width, or the bound for a variable field. */
  uint64_t reserved_bits;
  bool is_scalar;
} FieldLayout;

typedef struct {
  FieldLayout *fields;
  size_t field_count;
  uint64_t reserved_bits;

  /* Which stream bits this packet's fixed fields pin, and to what, over the stretch where offsets
   * are still constant. Check 12 compares these; a decoder will match against them.
   *
   * Held as bits rather than as a list of fields because the question a decoder asks is about the
   * stream, not about the declaration: two packets can pin the same stream bits through fields of
   * different names and widths. */
  uint8_t *mask;
  uint8_t *value;
  size_t discriminator_bits;
} PacketLayout;

typedef struct {
  PacketLayout *packets;
  size_t packet_count;
} Layout;

/* Runs every check over `file`, reporting into `diags`.
 *
 * Returns the layout when the file passes, and NULL when it does not -- in which case `diags` says
 * why, with as many of the problems as could be found in one pass rather than only the first. */
Layout *check_file(const AstFile *file, DiagList *diags);

void layout_free(Layout *layout);
