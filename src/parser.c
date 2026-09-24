#include "parser.h"

#include <stdlib.h>
#include <string.h>

#include "lexer.h"

const char *const PARSER_ATTRIBUTE_NAMES[] = { "bit_order", "byte_order", "cal", NULL };
static const char *const BIT_ORDER_VALUES[] = { "msb_first", "lsb_first" };
static const char *const BYTE_ORDER_VALUES[] = { "big_endian", "little_endian" };

typedef struct {
  Lexer lexer;
  Token current;
  /* Just past the token before `current`: where a missing ';' would have gone. */
  int previous_end_line;
  int previous_end_column;
  DiagList *diags;
  AstFile *file;
  bool out_of_memory;
  /* Set while recovering, so a cascade of consequences is not reported as a cascade of errors. */
  bool panicking;
  /* The check-31 warning the last attribute raised, or -1: its fix is only known once the group ends. */
  long redundant;
} Parser;

static void advance(Parser *parser) {
  /* The lexer skips what is between tokens before a token, not after, so here it stands exactly at
   * the end of the one about to become the previous. */
  parser->previous_end_line = parser->lexer.line;
  parser->previous_end_column = parser->lexer.column;
  parser->current = lexer_next(&parser->lexer);
}

/* A "did you mean" for the diagnostic at `index`, about the word `wrong` written at its own position. */
static void suggest(Parser *parser, size_t index, const Token *wrong, const char *const *candidates, size_t count) {
  const char *closest = diag_closest(wrong->text, wrong->length, candidates, count);
  if (closest != NULL) {
    diag_set_fix(
      parser->diags,
      index,
      wrong->line,
      wrong->column,
      wrong->line,
      wrong->column + (int)wrong->length,
      closest,
      "did you mean '%s'?",
      closest
    );
  }
}

static bool check(const Parser *parser, TokenKind kind) {
  return parser->current.kind == kind;
}

static bool match(Parser *parser, TokenKind kind) {
  if (!check(parser, kind)) {
    return false;
  }
  advance(parser);
  return true;
}

static void error_at_current(Parser *parser, const char *expected) {
  if (parser->panicking) {
    return;
  }
  parser->panicking = true;
  diag_add(
    parser->diags,
    parser->current.line,
    parser->current.column,
    0,
    "expected %s, found %s",
    expected,
    token_describe(&parser->current)
  );
}

static bool expect(Parser *parser, TokenKind kind) {
  if (match(parser, kind)) {
    return true;
  }
  size_t index = parser->diags->count;
  error_at_current(parser, token_kind_name(kind));
  /* A missing ';' has one place it could go: straight after what came before it. */
  if (kind == TOK_SEMICOLON && parser->previous_end_line > 0) {
    diag_set_fix(
      parser->diags,
      index,
      parser->previous_end_line,
      parser->previous_end_column,
      parser->previous_end_line,
      parser->previous_end_column,
      ";",
      "insert ';'"
    );
  }
  return false;
}

/* Resume points. A field ends at ';' and a packet at '}', so skipping to one of those puts the
 * parser somewhere it can start reading again -- which is what lets one file report all four of its
 * mistakes instead of the first. */
static void recover_to_field_end(Parser *parser) {
  while (!check(parser, TOK_EOF) && !check(parser, TOK_INVALID)) {
    if (match(parser, TOK_SEMICOLON)) {
      break;
    }
    if (check(parser, TOK_RBRACE)) {
      break;
    }
    advance(parser);
  }
  parser->panicking = false;
}

static void recover_to_packet_end(Parser *parser) {
  while (!check(parser, TOK_EOF) && !check(parser, TOK_INVALID)) {
    if (match(parser, TOK_RBRACE)) {
      match(parser, TOK_SEMICOLON);
      break;
    }
    advance(parser);
  }
  parser->panicking = false;
}

static Expr *expr_new(Parser *parser, ExprKind kind, int line, int column) {
  Expr *expr = region_alloc(parser->file->region, sizeof(Expr));
  if (expr == NULL) {
    parser->out_of_memory = true;
    return NULL;
  }
  expr->kind = kind;
  expr->line = line;
  expr->column = column;
  return expr;
}

static Expr *parse_expression(Parser *parser);

/* factor = integer | valof | "(" expression ")" */
static Expr *parse_factor(Parser *parser) {
  int line = parser->current.line;
  int column = parser->current.column;

  if (check(parser, TOK_INT)) {
    Expr *expr = expr_new(parser, EXPR_LITERAL, line, column);
    if (expr != NULL) {
      expr->literal = parser->current.value;
    }
    advance(parser);
    return expr;
  }

  if (match(parser, TOK_VALOF)) {
    if (!expect(parser, TOK_LPAREN)) {
      return NULL;
    }
    if (!check(parser, TOK_IDENT)) {
      error_at_current(parser, "a field name");
      return NULL;
    }

    Expr *expr = expr_new(parser, EXPR_VALOF, line, column);
    if (expr != NULL) {
      expr->reference_line = parser->current.line;
      expr->reference_column = parser->current.column;
      expr->reference = region_strndup(parser->file->region, parser->current.text, parser->current.length);
      if (expr->reference == NULL) {
        parser->out_of_memory = true;
      }
    }
    advance(parser);
    if (!expect(parser, TOK_RPAREN)) {
      return NULL;
    }
    return expr;
  }

  if (match(parser, TOK_LPAREN)) {
    Expr *expr = parse_expression(parser);
    if (!expect(parser, TOK_RPAREN)) {
      return NULL;
    }
    return expr;
  }

  if (check(parser, TOK_REAL) && !parser->panicking) {
    parser->panicking = true;
    diag_add(
      parser->diags,
      line,
      column,
      0,
      "%s is not a whole number; widths and fixed values are integers, and only a calibration takes "
      "a fraction",
      token_describe(&parser->current)
    );
    return NULL;
  }

  error_at_current(parser, "a number, 'valof' or '('");
  return NULL;
}

static Expr *binary(Parser *parser, char op, Expr *lhs, Expr *rhs) {
  if (lhs == NULL || rhs == NULL) {
    return NULL;
  }
  Expr *expr = expr_new(parser, EXPR_BINARY, lhs->line, lhs->column);
  if (expr == NULL) {
    return NULL;
  }
  expr->op = op;
  expr->lhs = lhs;
  expr->rhs = rhs;
  return expr;
}

/* term = factor { ("*" | "/") factor } -- left-associative, binding tighter than + and -. */
static Expr *parse_term(Parser *parser) {
  Expr *node = parse_factor(parser);
  while (check(parser, TOK_STAR) || check(parser, TOK_SLASH)) {
    char op = check(parser, TOK_STAR) ? '*' : '/';
    advance(parser);
    node = binary(parser, op, node, parse_factor(parser));
  }
  return node;
}

/* expression = term { ("+" | "-") term } */
static Expr *parse_expression(Parser *parser) {
  Expr *node = parse_term(parser);
  while (check(parser, TOK_PLUS) || check(parser, TOK_MINUS)) {
    char op = check(parser, TOK_PLUS) ? '+' : '-';
    advance(parser);
    node = binary(parser, op, node, parse_term(parser));
  }
  return node;
}

/* ---- Attributes -----------------------------------------------------------------------------
 *
 * `@name: value;`. The names live here rather than in the lexer, so none of them is a reserved word
 * and a field may perfectly well be called `bit_order`. It also means an unknown attribute is
 * reported by name instead of as a parse failure two tokens further on, and that adding one later --
 * an enum, a calibration curve -- costs a row in this file and nothing in the grammar.
 *
 * The `written` flags are not bookkeeping: check 10 turns on the difference between a byte_order
 * written on a field and one the field inherited from a block, packet or file. */
typedef struct {
  BitOrder bit_order;
  ByteOrder byte_order;
  bool bit_written;
  bool byte_written;
  /* Whether the value in effect was written somewhere -- here or in an enclosing scope -- rather than
   * being the language's default. Survives inherited(), which is the point: it is what tells an
   * attribute repeating its enclosing scope (a warning, check 31) from one spelling out the default
   * (documentation, and silent). */
  bool bit_explicit;
  bool byte_explicit;
  int bit_line;
  int bit_column;
  int byte_line;
  int byte_column;

  /* `@cal:` names a calibration for one field. It is not inherited -- a curve is a property of one
   * sensor reading, and handing one to every field in a block would be a guess about the others -- so
   * wherever it is written, other than in front of a field, is an error rather than a default. */
  const char *cal;
  int cal_line;
  int cal_column;
  int cal_value_line;
  int cal_value_column;
} Attributes;

/* Everything a declaration inherits arrives with the flags cleared: what the enclosing scope said is
 * by definition not what was written here. */
static Attributes inherited(Attributes scope) {
  scope.bit_written = false;
  scope.byte_written = false;
  scope.cal = NULL;
  return scope;
}

/* Called wherever a group or a bare attribute applies to something that is not a single field. */
static void refuse_cal_here(Parser *parser, const Attributes *attrs, const char *where) {
  if (attrs->cal == NULL) {
    return;
  }
  diag_add(
    parser->diags,
    attrs->cal_line,
    attrs->cal_column,
    0,
    "'cal' attaches a calibration to one field, and cannot apply to %s; put it in a '[ ... ]' group in "
    "front of the field",
    where
  );
}

static bool word_is(const Token *token, const char *spelling) {
  size_t length = strlen(spelling);
  return token->length == length && memcmp(token->text, spelling, length) == 0;
}

/* The rest of a dotted name whose first part, `first`, has just been read: `vendor.Header`. What an
 * `as` import brings in is named this way. */
static const char *dotted_name(Parser *parser, const char *first, size_t first_length) {
  char name[256];
  size_t used = first_length < sizeof(name) - 1 ? first_length : sizeof(name) - 1;
  memcpy(name, first, used);

  while (check(parser, TOK_DOT)) {
    advance(parser);
    if (!check(parser, TOK_IDENT)) {
      error_at_current(parser, "a name after '.'");
      break;
    }
    if (used + 1 + parser->current.length < sizeof(name)) {
      name[used++] = '.';
      memcpy(name + used, parser->current.text, parser->current.length);
      used += parser->current.length;
    }
    advance(parser);
  }

  const char *copy = region_strndup(parser->file->region, name, used);
  if (copy == NULL) {
    parser->out_of_memory = true;
  }
  return copy;
}

/* Check 31, a warning: an attribute setting what an enclosing scope already wrote. At the value, which
 * is the part that says nothing new. */
static void redundant(Parser *parser, const Token *value, const char *name, int from_line) {
  parser->redundant = (long)parser->diags->count;
  diag_warn(
    parser->diags,
    value->line,
    value->column,
    31,
    "'@%s: %.*s' changes nothing: the '@%s' on line %d already applies here",
    name,
    (int)value->length,
    value->text,
    name,
    from_line
  );
}

/* attribute = "@" identifier ":" identifier ";" */
static bool parse_attribute(Parser *parser, Attributes *attrs) {
  int line = parser->current.line;
  int column = parser->current.column;
  if (!expect(parser, TOK_AT)) {
    return false;
  }

  if (!check(parser, TOK_IDENT)) {
    error_at_current(parser, "an attribute name");
    return false;
  }
  Token name = parser->current;
  advance(parser);

  if (!expect(parser, TOK_COLON)) {
    return false;
  }
  if (!check(parser, TOK_IDENT)) {
    error_at_current(parser, "an attribute value");
    return false;
  }
  Token value = parser->current;
  advance(parser);

  bool ok = true;
  /* What applied before this attribute, and whether it came from a scope enclosing this one. Written
   * earlier in the *same* group is not that: the group is one scope, repeating itself. */
  const bool bit_inherited = attrs->bit_explicit && !attrs->bit_written;
  const bool byte_inherited = attrs->byte_explicit && !attrs->byte_written;
  const BitOrder bit_before = attrs->bit_order;
  const ByteOrder byte_before = attrs->byte_order;
  const int bit_from = attrs->bit_line;
  const int byte_from = attrs->byte_line;

  if (word_is(&name, "bit_order")) {
    if (word_is(&value, "msb_first")) {
      attrs->bit_order = BIT_ORDER_MSB_FIRST;
    } else if (word_is(&value, "lsb_first")) {
      attrs->bit_order = BIT_ORDER_LSB_FIRST;
    } else {
      size_t index = parser->diags->count;
      diag_add(
        parser->diags,
        value.line,
        value.column,
        0,
        "'bit_order' is 'msb_first' or 'lsb_first', not '%.*s'",
        (int)value.length,
        value.text
      );
      suggest(parser, index, &value, BIT_ORDER_VALUES, 2);
      ok = false;
    }
    if (ok && bit_inherited && attrs->bit_order == bit_before) {
      redundant(parser, &value, "bit_order", bit_from);
    }
    attrs->bit_written = ok;
    attrs->bit_explicit = attrs->bit_explicit || ok;
    attrs->bit_line = line;
    attrs->bit_column = column;
  } else if (word_is(&name, "byte_order")) {
    if (word_is(&value, "big_endian")) {
      attrs->byte_order = BYTE_ORDER_BIG_ENDIAN;
    } else if (word_is(&value, "little_endian")) {
      attrs->byte_order = BYTE_ORDER_LITTLE_ENDIAN;
    } else {
      size_t index = parser->diags->count;
      diag_add(
        parser->diags,
        value.line,
        value.column,
        0,
        "'byte_order' is 'big_endian' or 'little_endian', not '%.*s'",
        (int)value.length,
        value.text
      );
      suggest(parser, index, &value, BYTE_ORDER_VALUES, 2);
      ok = false;
    }
    if (ok && byte_inherited && attrs->byte_order == byte_before) {
      redundant(parser, &value, "byte_order", byte_from);
    }
    attrs->byte_written = ok;
    attrs->byte_explicit = attrs->byte_explicit || ok;
    /* Positioned at the '@', which is what check 10 points the author at. */
    attrs->byte_line = line;
    attrs->byte_column = column;
  } else if (word_is(&name, "cal")) {
    attrs->cal = dotted_name(parser, value.text, value.length);
    attrs->cal_line = line;
    attrs->cal_column = column;
    attrs->cal_value_line = value.line;
    attrs->cal_value_column = value.column;
  } else {
    size_t index = parser->diags->count;
    diag_add(
      parser->diags,
      name.line,
      name.column,
      0,
      "unknown attribute '%.*s'; this version knows 'bit_order', 'byte_order' and 'cal'",
      (int)name.length,
      name.text
    );
    suggest(parser, index, &name, PARSER_ATTRIBUTE_NAMES, 3);
    ok = false;
  }

  if (!expect(parser, TOK_SEMICOLON)) {
    return false;
  }
  return ok;
}

/* attribute_group = "[" attribute { attribute } "]" */
static bool parse_attribute_group(Parser *parser, Attributes *attrs) {
  int line = parser->current.line;
  int column = parser->current.column;
  advance(parser); /* '[' */

  if (check(parser, TOK_RBRACKET)) {
    advance(parser);
    diag_add(
      parser->diags,
      line,
      column,
      0,
      "an attribute group must hold at least one attribute; an empty '[ ]' applies nothing"
    );
    return false;
  }

  /* Each attribute's extent, and the check-31 warning it raised if any: the fix for a redundant one
   * is to delete it, or the whole group when it is the only thing in it -- known only once `]` is. */
  enum { GROUP_MAX = 16 };
  struct {
    int line, column, end_line, end_column;
    long redundant;
  } seen[GROUP_MAX];
  size_t count = 0;

  while (check(parser, TOK_AT)) {
    int at_line = parser->current.line;
    int at_column = parser->current.column;
    parser->redundant = -1;
    if (!parse_attribute(parser, attrs)) {
      return false;
    }
    if (count < GROUP_MAX) {
      seen[count].line = at_line;
      seen[count].column = at_column;
      seen[count].end_line = parser->previous_end_line;
      seen[count].end_column = parser->previous_end_column;
      seen[count].redundant = parser->redundant;
    }
    count += 1;
  }
  if (!expect(parser, TOK_RBRACKET)) {
    return false;
  }

  for (size_t i = 0; i < count && i < GROUP_MAX; ++i) {
    if (seen[i].redundant < 0) {
      continue;
    }
    if (count == 1) {
      diag_set_removal(
        parser->diags,
        (size_t)seen[i].redundant,
        line,
        column,
        parser->previous_end_line,
        parser->previous_end_column,
        "remove the attribute group"
      );
    } else {
      diag_set_removal(
        parser->diags,
        (size_t)seen[i].redundant,
        seen[i].line,
        seen[i].column,
        seen[i].end_line,
        seen[i].end_column,
        "remove the redundant attribute"
      );
    }
  }
  return true;
}

/* field = identifier [ "[" expression "]" ] ":" width [ "=" integer ] ";" */
/* ---- Using a named block ---------------------------------------------------------------------
 *
 * use      = identifier ":" block_ref [ "{" { override } "}" ] ";"
 *          | block_ref [ "{" { override } "}" ] ";"
 * block_ref = identifier { "." identifier }
 * override = identifier { "." identifier } "=" integer ";"
 *
 * The second form is anonymous: the block's fields are pasted in under their own names.
 *
 * Only recorded here. compose_file() does the work once every block has been read, since a use and the
 * declaration it names are checked against each other there (checks 23 to 25). */

static bool parse_override(Parser *parser, AstField *field) {
  int line = parser->current.line;
  int column = parser->current.column;

  if (!check(parser, TOK_IDENT)) {
    error_at_current(parser, "the name of a field in the block");
    return false;
  }

  /* The path, rejoined with dots: `inner.id` is the name the expanded field will carry. */
  char path[256];
  size_t used = 0;
  for (;;) {
    if (!check(parser, TOK_IDENT)) {
      error_at_current(parser, "a field name after '.'");
      return false;
    }
    if (used + parser->current.length + 2 > sizeof(path)) {
      diag_add(parser->diags, line, column, 0, "an override path longer than %zu characters", sizeof(path) - 1);
      return false;
    }
    memcpy(path + used, parser->current.text, parser->current.length);
    used += parser->current.length;
    advance(parser);
    if (!match(parser, TOK_DOT)) {
      break;
    }
    path[used++] = '.';
  }
  path[used] = '\0';

  if (!expect(parser, TOK_EQUALS)) {
    return false;
  }
  if (!check(parser, TOK_INT)) {
    error_at_current(parser, "a number; an override fixes a field's value");
    return false;
  }

  AstOverride *override = region_alloc(parser->file->region, sizeof(AstOverride));
  if (override == NULL) {
    parser->out_of_memory = true;
    return false;
  }
  memset(override, 0, sizeof(*override));
  override->name = region_strndup(parser->file->region, path, used);
  override->value = parser->current.value;
  override->line = line;
  override->column = column;
  advance(parser);

  /* Kept in the order written, so diagnostics come out in it too. */
  AstOverride **tail = &field->overrides;
  while (*tail != NULL) {
    tail = &(*tail)->next;
  }
  *tail = override;

  return expect(parser, TOK_SEMICOLON);
}

/* What follows the block's name in a use, named or not: the overrides, if any, and the `;`. */
static void parse_use_rest(Parser *parser, AstField *field) {
  if (match(parser, TOK_LBRACE)) {
    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF) && !check(parser, TOK_INVALID)) {
      if (!parse_override(parser, field)) {
        recover_to_field_end(parser);
        return;
      }
    }
    if (!expect(parser, TOK_RBRACE)) {
      recover_to_field_end(parser);
      return;
    }
  }

  if (check(parser, TOK_EQUALS)) {
    diag_add(
      parser->diags,
      parser->current.line,
      parser->current.column,
      0,
      "'%s' uses a block, and a block has no single value to fix; fix its fields in '{ ... }' instead",
      field->name
    );
    recover_to_field_end(parser);
    return;
  }

  if (!expect(parser, TOK_SEMICOLON)) {
    recover_to_field_end(parser);
  }
}

/* A group in front of a use would have to reach inside the block and change its layout, which is
 * settled where the block is declared so that the same block reads the same everywhere. */
static void refuse_attributes_on_use(Parser *parser, const AstField *field, Attributes attrs) {
  if (attrs.bit_written || attrs.byte_written || attrs.cal != NULL) {
    int line = attrs.bit_written ? attrs.bit_line : attrs.byte_written ? attrs.byte_line : attrs.cal_line;
    int column = attrs.bit_written ? attrs.bit_column : attrs.byte_written ? attrs.byte_column : attrs.cal_column;
    diag_add(
      parser->diags,
      line,
      column,
      0,
      "'%s' uses a block, whose layout is fixed where the block is declared; put the attribute there",
      field->name
    );
  }
}

static void parse_block_use(Parser *parser, AstField *field, Attributes attrs) {
  refuse_attributes_on_use(parser, field, attrs);
  if (field->reserve != NULL) {
    diag_add(
      parser->diags,
      field->reserve_line,
      field->reserve_column,
      0,
      "'%s' uses a block, which has no reservation of its own; its fields reserve what they need",
      field->name
    );
  }

  Token first = parser->current;
  field->block_line = first.line;
  field->block_column = first.column;
  advance(parser);
  field->block = dotted_name(parser, first.text, first.length);
  if (field->block == NULL) {
    return;
  }
  parse_use_rest(parser, field);
}

/* `Header { ... };` or `Header;`: the name just read, `first`, was a block's and not a field's. */
static void parse_anonymous_use(Parser *parser, AstField *field, Token first, Attributes attrs) {
  field->anonymous = true;
  field->block_line = first.line;
  field->block_column = first.column;
  field->block = dotted_name(parser, first.text, first.length);
  if (field->block == NULL) {
    return;
  }
  field->name = field->block;
  refuse_attributes_on_use(parser, field, attrs);
  parse_use_rest(parser, field);
}

static void parse_field(Parser *parser, AstPacket *packet, Attributes attrs) {
  AstField *field = ast_add_field(packet);
  if (field == NULL) {
    parser->out_of_memory = true;
    return;
  }

  field->line = parser->current.line;
  field->column = parser->current.column;
  field->bit_order = attrs.bit_order;
  field->byte_order = attrs.byte_order;
  field->bit_order_written = attrs.bit_written;
  field->byte_order_written = attrs.byte_written;
  field->bit_order_line = attrs.bit_line;
  field->bit_order_column = attrs.bit_column;
  field->byte_order_line = attrs.byte_line;
  field->byte_order_column = attrs.byte_column;
  field->cal = attrs.cal;
  field->cal_line = attrs.cal_line;
  field->cal_column = attrs.cal_column;
  field->cal_value_line = attrs.cal_value_line;
  field->cal_value_column = attrs.cal_value_column;

  if (!check(parser, TOK_IDENT)) {
    error_at_current(parser, "a field name");
    recover_to_field_end(parser);
    packet->field_count -= 1;
    return;
  }
  Token first = parser->current;
  field->name = region_strndup(parser->file->region, parser->current.text, parser->current.length);
  if (field->name == NULL) {
    parser->out_of_memory = true;
    return;
  }
  advance(parser);

  /* A field's name is followed by `:` or `[`; a block's, used without naming the instance, by `{`, `;`
   * or the `.` of an imported namespace. One token decides. */
  if (check(parser, TOK_LBRACE) || check(parser, TOK_SEMICOLON) || check(parser, TOK_DOT)) {
    parse_anonymous_use(parser, field, first, attrs);
    return;
  }

  /* The reservation binds to the name, not to the width: it says how big this field's arena slot is,
   * which is a property of the field rather than of the expression that sizes it per frame. */
  if (check(parser, TOK_LBRACKET)) {
    field->reserve_line = parser->current.line;
    field->reserve_column = parser->current.column;
    advance(parser);
    field->reserve = parse_expression(parser);
    if (!expect(parser, TOK_RBRACKET)) {
      recover_to_field_end(parser);
      packet->field_count -= 1;
      return;
    }
  }

  if (!expect(parser, TOK_COLON)) {
    recover_to_field_end(parser);
    packet->field_count -= 1;
    return;
  }

  /* A name where a width would be is an instance of a named block. A width never starts with a name
   * -- it is a number, a valof() or a parenthesis -- so one token decides. */
  if (check(parser, TOK_IDENT)) {
    parse_block_use(parser, field, attrs);
    return;
  }

  field->width = parse_expression(parser);

  if (match(parser, TOK_EQUALS)) {
    if (!check(parser, TOK_INT)) {
      error_at_current(parser, "a number");
      recover_to_field_end(parser);
      return;
    }
    field->has_fixed = true;
    field->fixed = parser->current.value;
    field->fixed_line = parser->current.line;
    field->fixed_column = parser->current.column;
    advance(parser);
  }

  if (!expect(parser, TOK_SEMICOLON)) {
    recover_to_field_end(parser);
  }
}

/* declaration = [ attribute_group ] ( field | block )
 * block       = "block" "{" declaration { declaration } "}" ";"
 *
 * A block is an attribute scope and nothing else: its fields are appended to the packet they sit in,
 * with consecutive offsets and the same flat names they would have had without it. So the AST never
 * nests, and neither the checker, the layout pass nor the bytecode ever learns that blocks exist.
 * The named form, which does nest a dotted name, is composition -- see #13. */
static void parse_declaration(Parser *parser, AstPacket *packet, Attributes scope);

static void parse_block(Parser *parser, AstPacket *packet, Attributes attrs) {
  int line = parser->current.line;
  int column = parser->current.column;
  advance(parser); /* 'block' */

  if (check(parser, TOK_IDENT)) {
    diag_add(
      parser->diags,
      line,
      column,
      0,
      "a named block is declared at file level and used as a field, 'name: %.*s;', or pasted in, '%.*s;'; "
      "inside a packet, 'block { ... };' is an attribute scope and has no name",
      (int)parser->current.length,
      parser->current.text,
      (int)parser->current.length,
      parser->current.text
    );
    recover_to_packet_end(parser);
    return;
  }

  if (!expect(parser, TOK_LBRACE)) {
    recover_to_field_end(parser);
    return;
  }

  /* What the group on the block said is inherited by the fields inside, not written on them. Check 10
   * turns on exactly that: a byte_order a block hands down to a sub-byte field is irrelevant there and
   * meaningful two fields later, while one written on the field means somebody believed otherwise. */
  Attributes inner = inherited(attrs);
  size_t before = packet->field_count;

  while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF) && !check(parser, TOK_INVALID)
         && !parser->out_of_memory) {
    parse_declaration(parser, packet, inner);
  }

  if (packet->field_count == before && !parser->panicking) {
    diag_add(parser->diags, line, column, 0, "a block must declare at least one field");
  }

  if (!expect(parser, TOK_RBRACE)) {
    recover_to_field_end(parser);
    return;
  }
  if (!expect(parser, TOK_SEMICOLON)) {
    recover_to_field_end(parser);
  }
}

static void parse_declaration(Parser *parser, AstPacket *packet, Attributes scope) {
  /* Each declaration starts with a clean slate. Suppression exists to stop one mistake being reported
   * as four consequences of itself, and a declaration boundary is where those consequences end --
   * without this, a field that errors and then happens to resynchronise on its own ';' silently
   * swallows the next field's error, because nothing ever cleared the flag. */
  parser->panicking = false;

  Attributes attrs = inherited(scope);

  if (check(parser, TOK_LBRACKET)) {
    if (!parse_attribute_group(parser, &attrs)) {
      recover_to_field_end(parser);
      return;
    }
  }

  /* The likeliest mistake, so it gets a message about the rule rather than about the token. */
  if (check(parser, TOK_AT)) {
    diag_add(
      parser->diags,
      parser->current.line,
      parser->current.column,
      0,
      "an attribute here belongs in a '[ ... ]' group before the declaration it applies to; a "
      "bare '@' is a file default and may only appear before the first packet"
    );
    recover_to_field_end(parser);
    return;
  }

  if (check(parser, TOK_BLOCK)) {
    refuse_cal_here(parser, &attrs, "a block");
    parse_block(parser, packet, attrs);
    return;
  }
  parse_field(parser, packet, attrs);
}

/* ---- Calibrations -------------------------------------------------------------------------
 *
 * calibration = "cal" identifier "(" identifier ")" "=" cal_expr
 *               [ "inverse" "(" identifier ")" "=" cal_expr ] ";"
 * cal_expr    = cal_term { ("+" | "-") cal_term }
 * cal_term    = cal_unary { ("*" | "/") cal_unary }
 * cal_unary   = "-" cal_unary | cal_factor
 * cal_factor  = number | identifier [ "(" cal_expr ")" ] | "(" cal_expr ")"
 *
 * Arithmetic on doubles, with a parameter and calls. Whether a bare name is the parameter, and whether
 * a call names a calibration, is the checker's business (checks 18 and 19): here a name is a name. */

static CalExpr *cal_new(Parser *parser, CalExprKind kind, int line, int column) {
  CalExpr *expr = region_alloc(parser->file->region, sizeof(CalExpr));
  if (expr == NULL) {
    parser->out_of_memory = true;
    return NULL;
  }
  memset(expr, 0, sizeof(*expr));
  expr->kind = kind;
  expr->line = line;
  expr->column = column;
  return expr;
}

static const char *take_name(Parser *parser) {
  const char *name = region_strndup(parser->file->region, parser->current.text, parser->current.length);
  if (name == NULL) {
    parser->out_of_memory = true;
  }
  advance(parser);
  return name;
}

static CalExpr *parse_cal_expr(Parser *parser);

static CalExpr *parse_cal_factor(Parser *parser) {
  int line = parser->current.line;
  int column = parser->current.column;

  if (check(parser, TOK_INT) || check(parser, TOK_REAL)) {
    CalExpr *expr = cal_new(parser, CAL_EXPR_NUMBER, line, column);
    if (expr != NULL) {
      expr->number = parser->current.real;
    }
    advance(parser);
    return expr;
  }

  if (check(parser, TOK_IDENT)) {
    Token first = parser->current;
    advance(parser);
    const char *name = dotted_name(parser, first.text, first.length);

    if (!match(parser, TOK_LPAREN)) {
      CalExpr *expr = cal_new(parser, CAL_EXPR_NAME, line, column);
      if (expr != NULL) {
        expr->name = name;
      }
      return expr;
    }

    CalExpr *argument = parse_cal_expr(parser);
    if (!expect(parser, TOK_RPAREN) || argument == NULL) {
      return NULL;
    }
    CalExpr *expr = cal_new(parser, CAL_EXPR_CALL, line, column);
    if (expr != NULL) {
      expr->name = name;
      expr->lhs = argument;
    }
    return expr;
  }

  if (match(parser, TOK_LPAREN)) {
    CalExpr *expr = parse_cal_expr(parser);
    if (!expect(parser, TOK_RPAREN)) {
      return NULL;
    }
    return expr;
  }

  error_at_current(parser, "a number, a name or '('");
  return NULL;
}

static CalExpr *parse_cal_unary(Parser *parser) {
  int line = parser->current.line;
  int column = parser->current.column;

  if (match(parser, TOK_MINUS)) {
    CalExpr *operand = parse_cal_unary(parser);
    if (operand == NULL) {
      return NULL;
    }
    CalExpr *expr = cal_new(parser, CAL_EXPR_NEGATE, line, column);
    if (expr != NULL) {
      expr->lhs = operand;
    }
    return expr;
  }
  return parse_cal_factor(parser);
}

static CalExpr *cal_binary(Parser *parser, char op, CalExpr *lhs, CalExpr *rhs) {
  if (lhs == NULL || rhs == NULL) {
    return NULL;
  }
  CalExpr *expr = cal_new(parser, CAL_EXPR_BINARY, lhs->line, lhs->column);
  if (expr == NULL) {
    return NULL;
  }
  expr->op = op;
  expr->lhs = lhs;
  expr->rhs = rhs;
  return expr;
}

static CalExpr *parse_cal_term(Parser *parser) {
  CalExpr *node = parse_cal_unary(parser);
  while (check(parser, TOK_STAR) || check(parser, TOK_SLASH)) {
    char op = check(parser, TOK_STAR) ? '*' : '/';
    advance(parser);
    node = cal_binary(parser, op, node, parse_cal_unary(parser));
  }
  return node;
}

static CalExpr *parse_cal_expr(Parser *parser) {
  CalExpr *node = parse_cal_term(parser);
  while (check(parser, TOK_PLUS) || check(parser, TOK_MINUS)) {
    char op = check(parser, TOK_PLUS) ? '+' : '-';
    advance(parser);
    node = cal_binary(parser, op, node, parse_cal_term(parser));
  }
  return node;
}

/* `(name) = expression`, the part a calibration and its inverse share. */
static bool parse_cal_function(Parser *parser, const char **param, CalExpr **body) {
  if (!expect(parser, TOK_LPAREN)) {
    return false;
  }
  if (!check(parser, TOK_IDENT)) {
    error_at_current(parser, "a parameter name");
    return false;
  }
  *param = take_name(parser);
  if (!expect(parser, TOK_RPAREN) || !expect(parser, TOK_EQUALS)) {
    return false;
  }
  *body = parse_cal_expr(parser);
  return *body != NULL;
}

static void parse_cal(Parser *parser) {
  int line = parser->current.line;
  int column = parser->current.column;
  advance(parser); /* 'cal' */

  AstCal *cal = ast_add_cal(parser->file);
  if (cal == NULL) {
    parser->out_of_memory = true;
    return;
  }
  cal->line = line;
  cal->column = column;

  if (!check(parser, TOK_IDENT)) {
    error_at_current(parser, "a calibration name");
    parser->file->cal_count -= 1;
    recover_to_field_end(parser);
    return;
  }
  cal->name_line = parser->current.line;
  cal->name_column = parser->current.column;
  cal->name = take_name(parser);

  if (!parse_cal_function(parser, &cal->param, &cal->body)) {
    parser->file->cal_count -= 1;
    recover_to_field_end(parser);
    return;
  }

  /* `inverse` is a word only here, after a calibration's body, for the same reason `cal` is only one
   * where a declaration starts. */
  if (check(parser, TOK_IDENT) && word_is(&parser->current, "inverse")) {
    cal->inverse_line = parser->current.line;
    cal->inverse_column = parser->current.column;
    advance(parser);
    if (!parse_cal_function(parser, &cal->inverse_param, &cal->inverse)) {
      parser->file->cal_count -= 1;
      recover_to_field_end(parser);
      return;
    }
  }

  if (!expect(parser, TOK_SEMICOLON)) {
    recover_to_field_end(parser);
    return;
  }
  cal->end_line = parser->previous_end_line;
  cal->end_column = parser->previous_end_column;
}

/* import = "import" string [ "as" identifier ] ";" */
static AstImport *parse_import(Parser *parser) {
  int line = parser->current.line;
  int column = parser->current.column;
  advance(parser); /* 'import' */

  if (!check(parser, TOK_STRING)) {
    error_at_current(parser, "a quoted path to import");
    recover_to_field_end(parser);
    return NULL;
  }
  AstImport *import = region_alloc(parser->file->region, sizeof(AstImport));
  if (import == NULL) {
    parser->out_of_memory = true;
    return NULL;
  }
  memset(import, 0, sizeof(*import));
  import->path = region_strndup(parser->file->region, parser->current.text, parser->current.length);
  import->line = line;
  import->column = column;
  advance(parser);

  if (import->path == NULL || import->path[0] == '\0') {
    diag_add(parser->diags, line, column, 0, "an import needs a path");
  }

  if (check(parser, TOK_IDENT) && word_is(&parser->current, "as")) {
    advance(parser);
    if (!check(parser, TOK_IDENT)) {
      error_at_current(parser, "a name to import under");
      recover_to_field_end(parser);
      return NULL;
    }
    import->alias = take_name(parser);
  }

  if (!expect(parser, TOK_SEMICOLON)) {
    recover_to_field_end(parser);
    return import;
  }
  import->end_line = parser->previous_end_line;
  import->end_column = parser->previous_end_column;
  return import;
}

AstFile *parse(const char *source, DiagList *diags) {
  Parser parser = { 0 };
  parser.diags = diags;
  parser.redundant = -1;
  parser.file = ast_new();
  if (parser.file == NULL) {
    diag_add(diags, 1, 1, 0, "out of memory");
    return NULL;
  }

  lexer_init(&parser.lexer, source, diags);
  advance(&parser);

  /* Imports come first, before even the file's own defaults: they bring declarations that everything
   * below may use, and a file reads top to bottom. `import` and `as` are words only here. */
  AstImport **tail = &parser.file->imports;
  while (check(&parser, TOK_IDENT) && word_is(&parser.current, "import")) {
    parser.panicking = false;
    AstImport *import = parse_import(&parser);
    if (import != NULL) {
      *tail = import;
      tail = &import->next;
    }
  }

  /* A bare attribute is the file default and may only appear here, before the first packet. Anywhere
   * else it would have to mean either "from this point on" or "the next declaration only", and having
   * one spelling mean both depending on nesting is how a reader stops being able to tell. */
  Attributes file_attrs = {
    .bit_order = BIT_ORDER_MSB_FIRST,
    .byte_order = BYTE_ORDER_BIG_ENDIAN,
  };
  while (check(&parser, TOK_AT)) {
    if (!parse_attribute(&parser, &file_attrs)) {
      recover_to_field_end(&parser);
    }
    refuse_cal_here(&parser, &file_attrs, "a whole file");
    file_attrs.cal = NULL;
  }

  while (!check(&parser, TOK_EOF) && !check(&parser, TOK_INVALID) && !parser.out_of_memory) {
    parser.panicking = false;

    if (check(&parser, TOK_IDENT) && word_is(&parser.current, "import")) {
      diag_add(
        parser.diags,
        parser.current.line,
        parser.current.column,
        0,
        "an import goes at the top of the file, before anything else, so that everything below can "
        "use what it brings"
      );
      recover_to_field_end(&parser);
      continue;
    }

    /* `cal` is only a word here, where a declaration starts, so it is not reserved and a field may
     * still be called `cal`. */
    if (check(&parser, TOK_IDENT) && word_is(&parser.current, "cal")) {
      parse_cal(&parser);
      continue;
    }

    Attributes packet_attrs = inherited(file_attrs);
    const int start_line = parser.current.line;
    const int start_column = parser.current.column;
    if (check(&parser, TOK_LBRACKET)) {
      if (!parse_attribute_group(&parser, &packet_attrs)) {
        recover_to_packet_end(&parser);
        continue;
      }
      refuse_cal_here(&parser, &packet_attrs, check(&parser, TOK_BLOCK) ? "a block" : "a packet");
    }

    const bool is_block = check(&parser, TOK_BLOCK);
    if (!check(&parser, TOK_PKT) && !is_block) {
      if (check(&parser, TOK_AT)) {
        diag_add(
          parser.diags,
          parser.current.line,
          parser.current.column,
          0,
          "a bare attribute may only appear before the first packet; to set one for a packet, "
          "put it in a '[ ... ]' group in front of 'pkt'"
        );
      } else {
        error_at_current(&parser, "'pkt', 'block', 'cal' or end of file");
      }
      recover_to_packet_end(&parser);
      continue;
    }

    int line = parser.current.line;
    int column = parser.current.column;
    advance(&parser);

    /* A named block is shaped like a packet -- a name and fields, attribute scopes and all -- and is
     * parsed as one. It is only never laid out on its own. */
    AstPacket *packet = is_block ? ast_add_block(parser.file) : ast_add_packet(parser.file);
    if (packet == NULL) {
      parser.out_of_memory = true;
      break;
    }
    packet->line = line;
    packet->column = column;
    packet->start_line = start_line;
    packet->start_column = start_column;

    if (!check(&parser, TOK_IDENT)) {
      error_at_current(
        &parser, is_block ? "a block name; at file level a block is declared to be reused" : "a packet name"
      );
      recover_to_packet_end(&parser);
      continue;
    }
    packet->name_line = parser.current.line;
    packet->name_column = parser.current.column;
    packet->name = region_strndup(parser.file->region, parser.current.text, parser.current.length);
    if (packet->name == NULL) {
      parser.out_of_memory = true;
      break;
    }
    advance(&parser);

    if (!expect(&parser, TOK_LBRACE)) {
      recover_to_packet_end(&parser);
      continue;
    }

    /* The group in front of 'pkt' is the packet's default, so its fields inherit it rather than
     * having written it. */
    Attributes body_attrs = inherited(packet_attrs);
    size_t diags_before_body = diag_error_count(parser.diags);
    while (!check(&parser, TOK_RBRACE) && !check(&parser, TOK_EOF) && !check(&parser, TOK_INVALID)
           && !parser.out_of_memory) {
      parse_declaration(&parser, packet, body_attrs);
    }

    /* The grammar requires at least one declaration, and a packet without fields would encode to no
     * bytes at all -- a frame nothing could ever decode. Found by fuzzing (#59): checks 11 and 13 both
     * look at fields, so an empty packet passed every one of them.
     *
     * Only reported when the body was clean: a malformed field already leaves field_count at 0 and
     * reports its own diagnostic, and this one -- anchored on the packet's own opening brace -- would
     * otherwise sort ahead of it and become the diagnostic callers see first. */
    if (packet->field_count == 0 && diag_error_count(parser.diags) == diags_before_body) {
      diag_add(
        parser.diags,
        packet->line,
        packet->column,
        0,
        "a %s must declare at least one field",
        is_block ? "block" : "packet"
      );
    }

    packet->end_line = parser.current.line;
    packet->end_column = parser.current.column;
    if (!expect(&parser, TOK_RBRACE)) {
      recover_to_packet_end(&parser);
      continue;
    }
    if (!expect(&parser, TOK_SEMICOLON)) {
      recover_to_packet_end(&parser);
    }
  }

  if (parser.out_of_memory) {
    diag_add(diags, 1, 1, 0, "out of memory");
  }

  diag_sort(diags);
  return parser.file;
}
