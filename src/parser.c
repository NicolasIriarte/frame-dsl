#include "parser.h"

#include <stdlib.h>

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

/* One setting, without its terminator: the field form has none. */
static bool parse_setting_body(Parser *parser, BitOrder *bit_order, bool *bit_written,
                               ByteOrder *byte_order, bool *byte_written, int *byte_line,
                               int *byte_column) {
  if (match(parser, TOK_BIT_ORDER)) {
    if (match(parser, TOK_MSB_FIRST)) {
      *bit_order = BIT_ORDER_MSB_FIRST;
    } else if (match(parser, TOK_LSB_FIRST)) {
      *bit_order = BIT_ORDER_LSB_FIRST;
    } else {
      error_at_current(parser, "'msb_first' or 'lsb_first'");
      return false;
    }
    *bit_written = true;
    return true;
  }

  int line = parser->current.line;
  int column = parser->current.column;
  if (match(parser, TOK_BYTE_ORDER)) {
    if (match(parser, TOK_BIG_ENDIAN)) {
      *byte_order = BYTE_ORDER_BIG_ENDIAN;
    } else if (match(parser, TOK_LITTLE_ENDIAN)) {
      *byte_order = BYTE_ORDER_LITTLE_ENDIAN;
    } else {
      error_at_current(parser, "'big_endian' or 'little_endian'");
      return false;
    }
    *byte_written = true;
    if (byte_line != NULL) {
      *byte_line = line;
      *byte_column = column;
    }
    return true;
  }

  return false;
}

static bool at_setting(const Parser *parser) {
  return check(parser, TOK_BIT_ORDER) || check(parser, TOK_BYTE_ORDER);
}

/* field = identifier ":" width [ "max" expression ] [ "=" integer ] { field_setting } ";" */
static void parse_field(Parser *parser, AstPacket *packet, BitOrder bit_order, ByteOrder byte_order) {
  /* Each field starts with a clean slate. Suppression exists to stop one mistake being reported as
   * four consequences of itself, and a field boundary is where those consequences end -- without
   * this, a field that errors and then happens to resynchronise on its own ';' silently swallows the
   * next field's error, because nothing ever cleared the flag. */
  parser->panicking = false;

  AstField *field = ast_add_field(packet);
  if (field == NULL) {
    parser->out_of_memory = true;
    return;
  }

  field->line = parser->current.line;
  field->column = parser->current.column;
  field->bit_order = bit_order;
  field->byte_order = byte_order;

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

  if (!expect(parser, TOK_COLON)) {
    recover_to_field_end(parser);
    packet->field_count -= 1;
    return;
  }

  field->width = parse_expression(parser);

  if (match(parser, TOK_MAX)) {
    field->max = parse_expression(parser);
  }

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

  while (at_setting(parser)) {
    if (!parse_setting_body(parser, &field->bit_order, &field->bit_order_written, &field->byte_order,
                            &field->byte_order_written, &field->byte_order_line,
                            &field->byte_order_column)) {
      recover_to_field_end(parser);
      return;
    }
  }

  if (!expect(parser, TOK_SEMICOLON)) {
    recover_to_field_end(parser);
  }
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

  BitOrder file_bit_order = BIT_ORDER_MSB_FIRST;
  ByteOrder file_byte_order = BYTE_ORDER_BIG_ENDIAN;

  /* File-level settings come before the first packet and apply to the whole file, so there is no
   * "from this point on" subtlety to reason about. */
  while (at_setting(&parser)) {
    bool written = false;
    if (!parse_setting_body(&parser, &file_bit_order, &written, &file_byte_order, &written, NULL, NULL)) {
      recover_to_field_end(&parser);
      continue;
    }
    if (!expect(&parser, TOK_SEMICOLON)) {
      recover_to_field_end(&parser);
    }
  }

  while (!check(&parser, TOK_EOF) && !check(&parser, TOK_INVALID) && !parser.out_of_memory) {
    if (!check(&parser, TOK_PKT)) {
      error_at_current(&parser, "'pkt' or end of file");
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

    BitOrder packet_bit_order = file_bit_order;
    ByteOrder packet_byte_order = file_byte_order;
    while (at_setting(&parser)) {
      bool written = false;
      if (!parse_setting_body(&parser, &packet_bit_order, &written, &packet_byte_order, &written, NULL,
                              NULL)) {
        recover_to_field_end(&parser);
        continue;
      }
      if (!expect(&parser, TOK_SEMICOLON)) {
        recover_to_field_end(&parser);
      }
    }

    while (!check(&parser, TOK_RBRACE) && !check(&parser, TOK_EOF) && !check(&parser, TOK_INVALID) &&
           !parser.out_of_memory) {
      parse_field(&parser, packet, packet_bit_order, packet_byte_order);
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
