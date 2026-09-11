/* Story 2: the parser builds the tree docs/grammar.ebnf describes, and reports what it cannot. */
#include <stdio.h>
#include <string.h>

#include "ast.h"
#include "diag.h"
#include "parser.h"
#include "testsuite.h"

/* Evaluated here rather than by the compiler's own folding, which belongs to the checks. Walking the
 * tree is the point: it is what proves the parser built the shape the grammar calls for, since a
 * wrong precedence produces a different tree and therefore a different number. */
static long long eval(const Expr *expr) {
  if (expr == NULL) {
    return -1;
  }
  switch (expr->kind) {
  case EXPR_LITERAL:
    return (long long)expr->literal;
  case EXPR_VALOF:
    return -1;
  case EXPR_BINARY:
    switch (expr->op) {
    case '+': return eval(expr->lhs) + eval(expr->rhs);
    case '-': return eval(expr->lhs) - eval(expr->rhs);
    case '*': return eval(expr->lhs) * eval(expr->rhs);
    case '/': return eval(expr->lhs) / eval(expr->rhs);
    default: return -1;
    }
  }
  return -1;
}

/* Parses one packet with one field whose width is `width_source`, and evaluates that width. */
static long long parse_width(const char *width_source, size_t *diag_count) {
  char source[512];
  snprintf(source, sizeof(source), "pkt P { f: %s; };", width_source);

  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse(source, &diags);

  long long result = -1;
  if (diags.count == 0 && file != NULL && file->packet_count == 1 && file->packets[0].field_count == 1) {
    result = eval(file->packets[0].fields[0].width);
  }
  *diag_count = diags.count;

  ast_free(file);
  diag_free(&diags);
  return result;
}

static int us2_tc01_multiplication_binds_tighter_than_addition(const TestArguments *args) {
  (void)args;
  size_t diags = 0;
  if (parse_width("1 + 2 * 3", &diags) != 7 || diags != 0) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us2_tc02_subtraction_is_left_associative(const TestArguments *args) {
  (void)args;
  size_t diags = 0;
  if (parse_width("8 - 2 - 3", &diags) != 3 || diags != 0) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us2_tc03_parentheses_override_precedence(const TestArguments *args) {
  (void)args;
  size_t diags = 0;
  if (parse_width("(1 + 2) * 3", &diags) != 9 || diags != 0) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us2_tc04_division_binds_like_multiplication(const TestArguments *args) {
  (void)args;
  size_t diags = 0;
  if (parse_width("2 + 12 / 3 / 2", &diags) != 4 || diags != 0) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us2_tc05_parses_a_whole_packet(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse("bit_order msb_first;\n"
                        "pkt TM {\n"
                        "  hdr: 8 = 0xAA;\n"
                        "  len: 32;\n"
                        "  data: valof(len) * 8 max 1024;\n"
                        "};\n",
                        &diags);

  int failure = 0;
  if (diags.count != 0 || file == NULL || file->packet_count != 1) {
    failure = __LINE__;
  } else {
    const AstPacket *packet = &file->packets[0];
    if (strcmp(packet->name, "TM") != 0 || packet->field_count != 3) {
      failure = __LINE__;
    } else if (!packet->fields[0].has_fixed || packet->fields[0].fixed != 0xAA) {
      failure = __LINE__;
    } else if (packet->fields[1].has_fixed || packet->fields[1].max != NULL) {
      failure = __LINE__;
    } else if (packet->fields[2].max == NULL || eval(packet->fields[2].max) != 1024) {
      failure = __LINE__;
    } else if (packet->fields[2].width == NULL || packet->fields[2].width->kind != EXPR_BINARY) {
      failure = __LINE__;
    } else if (strcmp(packet->fields[2].width->lhs->reference, "len") != 0) {
      failure = __LINE__;
    }
  }

  ast_free(file);
  diag_free(&diags);
  return failure;
}

/* Precedence is field > packet > file, and it falls out of position rather than a separate rule. */
static int us2_tc06_resolves_settings_innermost_first(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse("bit_order msb_first;\n"
                        "byte_order big_endian;\n"
                        "pkt TM {\n"
                        "  byte_order little_endian;\n"
                        "  a: 32;\n"
                        "  b: 32 byte_order big_endian;\n"
                        "  c: 4 bit_order lsb_first;\n"
                        "};\n",
                        &diags);

  int failure = 0;
  if (diags.count != 0 || file == NULL || file->packet_count != 1) {
    failure = __LINE__;
  } else {
    const AstField *fields = file->packets[0].fields;
    if (fields[0].byte_order != BYTE_ORDER_LITTLE_ENDIAN || fields[0].byte_order_written) {
      failure = __LINE__; /* inherited from the packet */
    } else if (fields[1].byte_order != BYTE_ORDER_BIG_ENDIAN || !fields[1].byte_order_written) {
      failure = __LINE__; /* written on the field, overriding the packet */
    } else if (fields[2].bit_order != BIT_ORDER_LSB_FIRST || !fields[2].bit_order_written) {
      failure = __LINE__;
    } else if (fields[2].byte_order != BYTE_ORDER_LITTLE_ENDIAN || fields[2].byte_order_written) {
      failure = __LINE__;
    }
  }

  ast_free(file);
  diag_free(&diags);
  return failure;
}

static int us2_tc07_syntax_error_reports_line_and_column(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse("pkt TM {\n  hdr 8;\n};\n", &diags);

  int failure = 0;
  if (diags.count < 1) {
    failure = __LINE__;
  } else if (diags.items[0].line != 2 || diags.items[0].column != 7) {
    failure = __LINE__; /* the ':' should have been at column 7 */
  } else if (strstr(diags.items[0].message, "expected") == NULL) {
    failure = __LINE__;
  }

  ast_free(file);
  diag_free(&diags);
  return failure;
}

/* One run, several mistakes. Stopping at the first means fixing a file takes as many runs as it has
 * errors. */
static int us2_tc08_reports_more_than_the_first_error(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse("pkt TM {\n"
                        "  a 8;\n"   /* missing ':' */
                        "  b: ;\n"   /* missing width */
                        "  c: 8\n"   /* missing ';' */
                        "};\n",
                        &diags);

  int failure = diags.count >= 3 ? 0 : __LINE__;

  ast_free(file);
  diag_free(&diags);
  return failure;
}

static int us2_tc09_rejects_a_reserved_word_as_a_field_name(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse("pkt TM { max: 8; };", &diags);

  int failure = 0;
  if (diags.count < 1) {
    failure = __LINE__;
  } else if (strstr(diags.items[0].message, "field name") == NULL) {
    failure = __LINE__;
  }

  ast_free(file);
  diag_free(&diags);
  return failure;
}

/* Diagnostics are sorted by position, so output reads in file order however the passes found them. */
static int us2_tc10_diagnostics_come_back_in_file_order(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse("pkt A {\n  x 1;\n};\npkt B {\n  y 2;\n};\n", &diags);

  int failure = 0;
  if (diags.count < 2) {
    failure = __LINE__;
  } else {
    for (size_t i = 1; i < diags.count; ++i) {
      if (diags.items[i - 1].line > diags.items[i].line) {
        failure = __LINE__;
        break;
      }
    }
  }

  ast_free(file);
  diag_free(&diags);
  return failure;
}

void register_user_story2(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us2_tc01_multiplication_binds_tighter_than_addition, "1 + 2 * 3 parses as 7", false);
  ADD_TEST_CASE(us2_tc02_subtraction_is_left_associative, "8 - 2 - 3 parses as 3", false);
  ADD_TEST_CASE(us2_tc03_parentheses_override_precedence, "(1 + 2) * 3 parses as 9", false);
  ADD_TEST_CASE(us2_tc04_division_binds_like_multiplication, "2 + 12 / 3 / 2 parses as 4", false);
  ADD_TEST_CASE(us2_tc05_parses_a_whole_packet, "a packet's fields, fixed value and max all survive", false);
  ADD_TEST_CASE(us2_tc06_resolves_settings_innermost_first, "settings resolve field > packet > file", false);
  ADD_TEST_CASE(us2_tc07_syntax_error_reports_line_and_column, "a syntax error carries its position", false);
  ADD_TEST_CASE(us2_tc08_reports_more_than_the_first_error, "several errors are reported in one run", false);
  ADD_TEST_CASE(us2_tc09_rejects_a_reserved_word_as_a_field_name, "a reserved word is not a field name", false);
  ADD_TEST_CASE(us2_tc10_diagnostics_come_back_in_file_order, "diagnostics are ordered by position", false);
}
