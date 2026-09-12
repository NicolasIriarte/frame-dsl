#include "parser.h"

#include <stdlib.h>
#include <string.h>

#include "lexer.h"

typedef struct {
  Lexer lexer;
  Token current;
  DiagList *diags;
  AstFile *file;
  bool out_of_memory;
  /* Set while recovering, so a cascade of consequences is not reported as a cascade of errors. */
  bool panicking;
} Parser;

static void advance(Parser *parser) {
  parser->current = lexer_next(&parser->lexer);
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
  diag_add(parser->diags, parser->current.line, parser->current.column, 0, "expected %s, found %s",
           expected, token_describe(&parser->current));
}

static bool expect(Parser *parser, TokenKind kind) {
  if (match(parser, kind)) {
    return true;
  }
  error_at_current(parser, token_kind_name(kind));
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
  int bit_line;
  int bit_column;
  int byte_line;
  int byte_column;
} Attributes;

/* Everything a declaration inherits arrives with the flags cleared: what the enclosing scope said is
 * by definition not what was written here. */
static Attributes inherited(Attributes scope) {
  scope.bit_written = false;
  scope.byte_written = false;
  return scope;
}

static bool word_is(const Token *token, const char *spelling) {
  size_t length = strlen(spelling);
  return token->length == length && memcmp(token->text, spelling, length) == 0;
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
  if (word_is(&name, "bit_order")) {
    if (word_is(&value, "msb_first")) {
      attrs->bit_order = BIT_ORDER_MSB_FIRST;
    } else if (word_is(&value, "lsb_first")) {
      attrs->bit_order = BIT_ORDER_LSB_FIRST;
    } else {
      diag_add(parser->diags, value.line, value.column, 0,
               "'bit_order' is 'msb_first' or 'lsb_first', not '%.*s'", (int)value.length, value.text);
      ok = false;
    }
    attrs->bit_written = ok;
    attrs->bit_line = line;
    attrs->bit_column = column;
  } else if (word_is(&name, "byte_order")) {
    if (word_is(&value, "big_endian")) {
      attrs->byte_order = BYTE_ORDER_BIG_ENDIAN;
    } else if (word_is(&value, "little_endian")) {
      attrs->byte_order = BYTE_ORDER_LITTLE_ENDIAN;
    } else {
      diag_add(parser->diags, value.line, value.column, 0,
               "'byte_order' is 'big_endian' or 'little_endian', not '%.*s'", (int)value.length,
               value.text);
      ok = false;
    }
    attrs->byte_written = ok;
    /* Positioned at the '@', which is what check 10 points the author at. */
    attrs->byte_line = line;
    attrs->byte_column = column;
  } else {
    diag_add(parser->diags, name.line, name.column, 0,
             "unknown attribute '%.*s'; this version knows 'bit_order' and 'byte_order'",
             (int)name.length, name.text);
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
    diag_add(parser->diags, line, column, 0,
             "an attribute group must hold at least one attribute; an empty '[ ]' applies nothing");
    return false;
  }

  while (check(parser, TOK_AT)) {
    if (!parse_attribute(parser, attrs)) {
      return false;
    }
  }
  return expect(parser, TOK_RBRACKET);
}

/* field = identifier [ "[" expression "]" ] ":" width [ "=" integer ] ";" */
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

  if (!check(parser, TOK_IDENT)) {
    error_at_current(parser, "a field name");
    recover_to_field_end(parser);
    packet->field_count -= 1;
    return;
  }
  field->name = region_strndup(parser->file->region, parser->current.text, parser->current.length);
  if (field->name == NULL) {
    parser->out_of_memory = true;
    return;
  }
  advance(parser);

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

  if (!expect(parser, TOK_LBRACE)) {
    recover_to_field_end(parser);
    return;
  }

  /* What the group on the block said is inherited by the fields inside, not written on them. Check 10
   * turns on exactly that: a byte_order a block hands down to a sub-byte field is irrelevant there and
   * meaningful two fields later, while one written on the field means somebody believed otherwise. */
  Attributes inner = inherited(attrs);
  size_t before = packet->field_count;

  while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF) && !check(parser, TOK_INVALID) &&
         !parser->out_of_memory) {
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
    diag_add(parser->diags, parser->current.line, parser->current.column, 0,
             "an attribute here belongs in a '[ ... ]' group before the declaration it applies to; a "
             "bare '@' is a file default and may only appear before the first packet");
    recover_to_field_end(parser);
    return;
  }

  if (check(parser, TOK_BLOCK)) {
    parse_block(parser, packet, attrs);
    return;
  }
  parse_field(parser, packet, attrs);
}

AstFile *parse(const char *source, DiagList *diags) {
  Parser parser = {0};
  parser.diags = diags;
  parser.file = ast_new();
  if (parser.file == NULL) {
    diag_add(diags, 1, 1, 0, "out of memory");
    return NULL;
  }

  lexer_init(&parser.lexer, source, diags);
  advance(&parser);

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
  }

  while (!check(&parser, TOK_EOF) && !check(&parser, TOK_INVALID) && !parser.out_of_memory) {
    parser.panicking = false;

    Attributes packet_attrs = inherited(file_attrs);
    if (check(&parser, TOK_LBRACKET)) {
      if (!parse_attribute_group(&parser, &packet_attrs)) {
        recover_to_packet_end(&parser);
        continue;
      }
    }

    if (!check(&parser, TOK_PKT)) {
      if (check(&parser, TOK_AT)) {
        diag_add(parser.diags, parser.current.line, parser.current.column, 0,
                 "a bare attribute may only appear before the first packet; to set one for a packet, "
                 "put it in a '[ ... ]' group in front of 'pkt'");
      } else {
        error_at_current(&parser, "'pkt' or end of file");
      }
      recover_to_packet_end(&parser);
      continue;
    }

    int line = parser.current.line;
    int column = parser.current.column;
    advance(&parser);

    AstPacket *packet = ast_add_packet(parser.file);
    if (packet == NULL) {
      parser.out_of_memory = true;
      break;
    }
    packet->line = line;
    packet->column = column;

    if (!check(&parser, TOK_IDENT)) {
      error_at_current(&parser, "a packet name");
      recover_to_packet_end(&parser);
      continue;
    }
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
    while (!check(&parser, TOK_RBRACE) && !check(&parser, TOK_EOF) && !check(&parser, TOK_INVALID) &&
           !parser.out_of_memory) {
      parse_declaration(&parser, packet, body_attrs);
    }

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
