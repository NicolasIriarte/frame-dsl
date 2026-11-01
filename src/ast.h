#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "region.h"

typedef enum {
  BIT_ORDER_MSB_FIRST,
  BIT_ORDER_LSB_FIRST,
} BitOrder;

typedef enum {
  BYTE_ORDER_BIG_ENDIAN,
  BYTE_ORDER_LITTLE_ENDIAN,
} ByteOrder;

/** @brief How a slot's pattern reads (#10): the same bits either way, so layout never asks. */
typedef enum {
  FIELD_KIND_UNSIGNED,
  FIELD_KIND_SIGNED,
  FIELD_KIND_FLOAT,
} FieldKind;

/**
 * @brief How a value was written, which only the field's type can judge (#10): `i8 := -1;` means
 * something and `8 := -1;` does not, and a parser reading a value does not know which it is looking at.
 * Composition reads this and turns the value into the pattern it stands for, so nothing past it sees one.
 */
typedef struct {
  bool negative;
  /** @brief With a fraction or an exponent. */
  bool real;
  /** @brief In hex or binary. */
  bool based;
  /** @brief The magnitude, for a float. */
  double number;
} ValueForm;

typedef enum {
  EXPR_LITERAL,
  EXPR_VALOF,
  EXPR_BINARY,
} ExprKind;

typedef struct Expr {
  ExprKind kind;
  int line;
  int column;

  uint64_t literal; /**< EXPR_LITERAL */
  const char *reference; /**< EXPR_VALOF */
  /**
   * @brief EXPR_VALOF: the name as written in the parentheses. `reference` is the same until composition
   * renames it for an instance (`hdr.len` for `len` inside `hdr`); this stays what the reader sees.
   */
  const char *written;
  /** @brief EXPR_VALOF: where the name inside the parentheses is, as opposed to the `valof`. */
  int reference_line;
  int reference_column;

  char op; /**< EXPR_BINARY: one of + - * / */
  struct Expr *lhs;
  struct Expr *rhs;
} Expr;

/* ---- Calibrations ---------------------------------------------------------------------------
 *
 * A calibration's arithmetic is in doubles and has a parameter and calls, none of which a width has.
 * It is a separate node type rather than more kinds on Expr so that nothing in the width path -- the
 * checker's folds, the byte-multiple analysis, the width compiler -- has a case it must ignore. */

typedef enum {
  CAL_EXPR_NUMBER,
  /** @brief A bare name: the parameter, if it is spelled like it. Check 19 says if it is not. */
  CAL_EXPR_NAME,
  /** @brief `name(argument)`: another calibration, applied to `lhs`. */
  CAL_EXPR_CALL,
  CAL_EXPR_NEGATE,
  CAL_EXPR_BINARY,
} CalExprKind;

typedef struct CalExpr {
  CalExprKind kind;
  int line;
  int column;

  double number; /**< CAL_EXPR_NUMBER */
  const char *name; /**< CAL_EXPR_NAME, CAL_EXPR_CALL */

  char op; /**< CAL_EXPR_BINARY: one of + - * / */
  struct CalExpr *lhs; /**< the operand, the argument, or the left-hand side */
  struct CalExpr *rhs;
} CalExpr;

/** @brief `cal name(param) = body [inverse (inverse_param) = inverse];` */
typedef struct {
  const char *name;
  int line;
  int column;
  /** @brief Where the name is, as opposed to the `cal` in front of it. */
  int name_line;
  int name_column;

  const char *param;
  CalExpr *body;

  /** @brief NULL when none was written, which is the usual case: a linear curve inverts itself. */
  const char *inverse_param;
  CalExpr *inverse;
  int inverse_line;
  int inverse_column;

  /** @brief As AstPacket.origin: where an imported calibration was declared, or NULL. */
  const char *origin;
  /** @brief Just past its closing `;`. */
  int end_line;
  int end_column;
} AstCal;

/** @brief One name of an enum, `NAME [= value];`. */
typedef struct {
  const char *name;
  /** @brief As written, or counted: one more than the name before it, 0 for the first. */
  uint64_t value;
  /** @brief Counting went past the largest 64-bit value, so `value` means nothing; check 38 says so. */
  bool overflowed;
  int line;
  int column;
} AstEnumItem;

/**
 * @brief `enum Name: width { A; B = 5; C; };` at file level (#11): names for a field's values, and the
 * width every field of this type has. A field is typed by it the way it uses a block, `level: TxLevel;`,
 * and composition turns that field into an ordinary one of the enum's width.
 */
typedef struct {
  const char *name;
  /** @brief Where the `enum` is. */
  int line;
  int column;
  int name_line;
  int name_column;

  uint64_t width;
  int width_line;
  int width_column;

  AstEnumItem *items;
  size_t item_count;
  size_t item_capacity;

  /** @brief Where the closing brace is. It counts as declared from here on, as a block does (check 24). */
  int end_line;
  int end_column;
  /** @brief Just past its closing `;`. */
  int stop_line;
  int stop_column;

  /** @brief As AstPacket.origin: where an imported enum was declared, or NULL. */
  const char *origin;
} AstEnum;

/** @brief `import "path" [as alias];` at the top of a file. */
typedef struct AstImport {
  const char *path;
  /** @brief NULL for a flat import. */
  const char *alias;
  int line;
  int column;
  /** @brief Just past its closing `;`. */
  int end_line;
  int end_column;
  /**
   * @brief Every name this import made available, as the file sees it -- `ns.Header` for an `as`
   * import. Filled by imports_resolve(), in the file's region, so the unused-import warning can ask
   * whether any of them is used without working the prefixing out again.
   */
  const char **brought;
  size_t brought_count;
  struct AstImport *next;
} AstImport;

/** @brief How a comparison in a condition compares (#12, #126). */
typedef enum {
  COMPARE_EQ,
  COMPARE_NE,
  COMPARE_LT,
  COMPARE_LE,
  COMPARE_GT,
  COMPARE_GE,
} CompareOp;

typedef enum {
  /** @brief `valof(name) == value`, or another operator, optionally masked: `valof(f) & 0x4 != 0`. */
  CONDITION_COMPARE,
  /** @brief `lhs && rhs`, where it is inside an `||` -- at the top of a condition it is split instead. */
  CONDITION_AND,
  /** @brief `lhs || rhs`. */
  CONDITION_OR,
} ConditionKind;

/**
 * @brief `[ @if: valof(name) == value; ]` (#12): the field, block use or anonymous block it is in front
 * of is present only when this holds, and every enclosing `@if` with it.
 *
 * A field's condition is a chain of *terms* through `parent`, every one of which must hold: each `@if`
 * contributes its top-level `&&` operands (#126), and a nested group's terms are chained to the ones of
 * the groups around it. A term is a comparison, or an `||` (or an `&&` inside one) whose operands are
 * themselves conditions, through `lhs` and `rhs`; only a term has a `parent` or a `text`.
 */
typedef struct AstCondition {
  ConditionKind kind;
  /** @brief CONDITION_AND and CONDITION_OR: the operands, outside any chain. */
  struct AstCondition *lhs;
  struct AstCondition *rhs;

  /** @brief The field compared: an EXPR_VALOF, so it is renamed per instance as a width is. */
  Expr *field;
  CompareOp op;
  /** @brief `valof(f) & mask`: the comparison is of the field's value ANDed with this (#126). */
  bool has_mask;
  uint64_t mask;
  int mask_line;
  int mask_column;
  uint64_t value;
  /** @brief How `value` was written: a signed field's constant may be negative (#10). */
  ValueForm value_form;
  /** @brief The value written as a name of the compared field's enum, or NULL for a number. */
  const char *value_name;
  /** @brief Whether composition has looked `value_name` up: a block's conditions are resolved once, at
   * the block, and every instance copies the result. */
  bool resolved;
  /** @brief Where the term or comparison starts. */
  int line;
  int column;
  /** @brief Where the value is. */
  int value_line;
  int value_column;
  /** @brief The next term of the chain -- of this `@if`, or of the group this one is nested in -- or
   * NULL. */
  struct AstCondition *parent;
  /** @brief The whole chain as text, outermost first, joined by " && ": what a field's description
   * shows. Filled by composition. */
  const char *text;
} AstCondition;

/** @brief `name = value;` inside a block use: fixes one field of that instance. */
typedef struct AstOverride {
  /** @brief Relative to the instance, and dotted for a field of a nested one: `inner.id`. */
  const char *name;
  uint64_t value;
  /** @brief How `value` was written. */
  ValueForm form;
  /** @brief The value written as a name of the field's enum, `type = HK;`, or NULL for a number. */
  const char *value_name;
  /** @brief `?=` rather than `:=` (#144): the field becomes what the use wrote, whatever the block said. */
  bool is_default;
  int line;
  int column;
  /** @brief Where the value is. */
  int value_line;
  int value_column;
  struct AstOverride *next;
} AstOverride;

typedef struct {
  const char *name;
  int line;
  int column;

  /**
   * @brief The named block this field is an instance of, or NULL for an ordinary field.
   *
   * Only ever set between parse() and compose_file(): composition replaces every instance with the
   * block's fields, so nothing after it -- the checks, the layout, the bytecode -- sees one.
   */
  const char *block;
  /** @brief Where the block's name is written in the use: after the `:`, or first when anonymous. */
  int block_line;
  int block_column;
  AstOverride *overrides;
  /**
   * @brief A use written without an instance name, `Header { ... };`: the block's fields are pasted
   * in under their own names rather than under `name.`. `name` is then the block's name as written,
   * which is only ever used to say which use a message is about.
   */
  bool anonymous;

  /**
   * @brief For a repeated use, `name: Block[count];`, the count as written: a constant, or an
   * expression with a valof() in it. NULL for an ordinary use. Only ever set between parse() and
   * compose_file(), like `block`.
   */
  Expr *repeat;
  int repeat_line;
  int repeat_column;

  /**
   * @brief What composition leaves in a repetition's place: one field whose width is the count times
   * `element_bits`, laid out as a bit string, whose elements are `element`'s fields. NULL for every
   * other field. `element` is the block, already expanded into ordinary fields.
   */
  const struct AstPacket *element;
  uint64_t element_bits;

  Expr *width;

  /**
   * @brief The arena reservation written as `name[bytes]:`, or NULL when there is none.
   *
   * In BYTES, unlike every other number in the language, because it is storage rather than layout --
   * see manual/language.md. check_file() converts it to bits once and nothing downstream sees bytes.
   */
  Expr *reserve;
  int reserve_line;
  int reserve_column;

  /** @brief Set by composition, from a type written where a width goes (#10). */
  FieldKind kind;

  bool has_fixed;
  /** @brief The pattern, once composition has read `fixed_form`. */
  uint64_t fixed;
  ValueForm fixed_form;
  int fixed_line;
  int fixed_column;
  /**
   * @brief A fixed value written as a name, `type: PktType = HK;`, until composition looks it up in the
   * field's enum and sets `fixed` from it. NULL when the value is a number, or there is none.
   */
  const char *fixed_name;

  /**
   * @brief A default, `?= 1` (#144): what the field holds until something replaces it. Written where
   * `fixed` is and in the same bits, and held to the same checks of its width, but it identifies nothing
   * and is never stamped over what a program set.
   */
  bool has_default;
  uint64_t default_value;
  ValueForm default_form;
  int default_line;
  int default_column;
  /** @brief As `fixed_name`, for a default written as a name. */
  const char *default_name;

  /**
   * @brief The enum this field is typed by, as this file names it (`vendor.Mode`), or NULL. Set by
   * composition, which turns a use of an enum into an ordinary field of the enum's width.
   */
  const char *enumeration;

  /**
   * @brief The innermost `@if` this field is under, or NULL when it is always present (#12). After
   * composition every field carries its whole chain, renamed for the instance it landed in.
   */
  AstCondition *condition;

  /** @brief Resolved: what applies to this field once file, packet and field scope are combined. */
  BitOrder bit_order;
  ByteOrder byte_order;

  /**
   * @brief Whether the setting was *written on this field*, as opposed to inherited.
   *
   * Check 10 turns on the difference: an inherited byte_order that cannot apply here is irrelevant,
   * while one written here means somebody believed they had specified something.
   */
  bool bit_order_written;
  bool byte_order_written;
  int bit_order_line;
  int bit_order_column;
  int byte_order_line;
  int byte_order_column;

  /** @brief The calibration `@cal:` names, or NULL. Resolved by name at check time (check 18). */
  const char *cal;
  int cal_line;
  int cal_column;
  /** @brief Where the calibration's name is, after `@cal:`: what a "did you mean" replaces. */
  int cal_value_line;
  int cal_value_column;
} AstField;

typedef struct AstPacket {
  /** @brief `@decode: manual`: the program says which packet arrived, so nothing here has to (#146). */
  bool manual;
  const char *name;
  int line;
  int column;
  /** @brief Where the name is, as opposed to the `pkt` or `block` in front of it. */
  int name_line;
  int name_column;
  /** @brief Where the declaration starts: its attribute group when it has one, else the keyword. */
  int start_line;
  int start_column;

  AstField *fields;
  size_t field_count;
  size_t field_capacity;

  /** @brief Where the closing brace is. A named block counts as declared from here on (check 24). */
  int end_line;
  int end_column;

  /** @brief For a block merged from an import, the real path of the file that declared it, or NULL.
   * Two blocks of one name from one origin are the same block reached twice, and not a clash.
   */
  const char *origin;
} AstPacket;

typedef struct AstFile {
  AstPacket *packets;
  size_t packet_count;
  size_t packet_capacity;

  /**
   * @brief Named blocks, `block NAME { ... };` at file level. The same shape as a packet -- a name and
   * fields -- and never laid out on their own: they exist to be copied into packets.
   */
  AstPacket *blocks;
  size_t block_count;
  size_t block_capacity;

  AstCal *cals;
  size_t cal_count;
  size_t cal_capacity;

  AstEnum *enums;
  size_t enum_count;
  size_t enum_capacity;

  /** @brief In the order written. Resolved by imports_resolve(), which merges what they bring. */
  AstImport *imports;

  /**
   * @brief The files imports were read from, kept for as long as this one: what they brought is
   * merged by pointer, not copied. Owned, and freed with this file.
   */
  struct AstFile **imported;
  size_t imported_count;
  size_t imported_capacity;

  /** @brief Owns every Expr and every string the nodes point at. */
  Region *region;
} AstFile;

AstFile *ast_new(void);
void ast_free(AstFile *file);

/**
 * @brief Add a packet to `file`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstPacket *ast_add_packet(AstFile *file);

/**
 * @brief Add a field to `packet`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstField *ast_add_field(AstPacket *packet);

/**
 * @brief Add a calibration to `file`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstCal *ast_add_cal(AstFile *file);

/**
 * @brief Add a named block to `file`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstPacket *ast_add_block(AstFile *file);

/**
 * @brief Add an enum to `file`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstEnum *ast_add_enum(AstFile *file);

/**
 * @brief Add a name to `enumeration`.
 * @return NULL on allocation failure. The caller reports it; there is nothing else to do.
 */
AstEnumItem *ast_add_enum_item(AstEnum *enumeration);

/** @brief The enum `file` calls `name`, or NULL. */
const AstEnum *ast_find_enum(const AstFile *file, const char *name);

/**
 * @brief Whether `name` has the shape of a type, `i` or `f` followed by digits (#10): no block or enum may
 * be called that (check 1), so one in a width's place is never anything else.
 *
 * @param name The name, as written.
 * @param kind Set to what it would be.
 * @param width_bits Set to its width, or to 0 for a shape no type has: `i0`, `i65` or `f16` (check 44).
 * @return false if the name is any other shape.
 */
bool ast_type_name(const char *name, FieldKind *kind, uint64_t *width_bits);

/** @brief The name `enumeration` gives `value`, or NULL when it lists none. Its values increase, so
 * this is a binary search. */
const char *ast_enum_name_of(const AstEnum *enumeration, uint64_t value);

/** @brief Whether two terms say the same thing, as written: the same fields, operators, masks and
 * values, combined the same way (#126). Two `@if`s written apart but alike are present together, so the
 * checks compare conditions this way rather than by which group they were written in. */
bool ast_condition_same(const AstCondition *a, const AstCondition *b);

/** @brief Whether every term in `inner`'s chain is also in `outer`'s: a field under `outer` is only
 * ever present when one under `inner` is. NULL is the empty chain. */
bool ast_conditions_cover(const AstCondition *outer, const AstCondition *inner);

/** @brief Whether two chains hold exactly the same terms, in any order. */
bool ast_conditions_equal(const AstCondition *a, const AstCondition *b);

/** @brief The name called `name` in `enumeration`, or NULL. */
const AstEnumItem *ast_enum_item(const AstEnum *enumeration, const char *name);
