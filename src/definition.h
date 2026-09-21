#pragma once

/**
 * @file
 * @brief The definition behind the opaque handle, shared by the loader and the runtime.
 *
 * It is here rather than in framedsl.c because the frame needs the layout to size its arena, and a
 * getter per field would be the same coupling written out longer.
 */

#include <stdint.h>

#include "ast.h"
#include "bytecode.h"
#include "calibration.h"
#include "check.h"

struct FramedslDefinition {
  AstFile *ast;
  Layout *layout;
  /** @brief What the runtime actually walks. Built once, when the file loads. */
  Program *program;
  /** @brief Every `cal` in the file, compiled. Empty rather than NULL when there are none. */
  CalTable *cals;

  /**
   * @brief Stamped into every FramedslFieldId this definition hands out, and checked by every
   * accessor.
   *
   * Without it an id resolved against one definition and used on another frame would address
   * whatever happened to sit at that index -- a plausible wrong value rather than an error.
   *
   * A counter rather than the pointer: an address is reused as soon as a definition is freed, so two
   * definitions that never coexisted would be indistinguishable. Tags are never reused within a run.
   */
  uint32_t tag;
};

/**
 * @brief Hands out the next definition tag.
 *
 * Not safe against two threads loading at once, which nothing in this project claims to support.
 *
 * @return The next tag.
 */
uint32_t definition_next_tag(void);
