/* Story 1: the lexer turns characters into tokens, and says where each one was. */
#include <string.h>

#include "diag.h"
#include "lexer.h"
#include "testsuite.h"

/* Collects every token of `source` into `out`, returning how many there were (excluding EOF). */
static size_t tokenise(const char *source, Token *out, size_t capacity, DiagList *diags) {
  Lexer lexer;
  lexer_init(&lexer, source, diags);

  size_t count = 0;
  for (;;) {
    Token token = lexer_next(&lexer);
    if (token.kind == TOK_EOF || token.kind == TOK_INVALID) {
      break;
    }
    if (count < capacity) {
      out[count] = token;
    }
    count += 1;
  }
  return count;
}

static int us1_tc01_recognises_every_punctuation_token(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[16];
  size_t count = tokenise(": ; { } ( ) = + - * /", tokens, 16, &diags);

  const TokenKind expected[] = {TOK_COLON, TOK_SEMICOLON, TOK_LBRACE, TOK_RBRACE,
                                TOK_LPAREN, TOK_RPAREN,   TOK_EQUALS, TOK_PLUS,
                                TOK_MINUS,  TOK_STAR,     TOK_SLASH};
  size_t expected_count = sizeof(expected) / sizeof(*expected);

  int failure = 0;
  if (count != expected_count || diags.count != 0) {
    failure = __LINE__;
  } else {
    for (size_t i = 0; i < expected_count; ++i) {
      if (tokens[i].kind != expected[i]) {
        failure = __LINE__;
        break;
      }
    }
  }

  diag_free(&diags);
  return failure;
}

static int us1_tc02_recognises_every_keyword(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[16];
  size_t count = tokenise("pkt valof max bit_order msb_first lsb_first byte_order big_endian little_endian",
                          tokens, 16, &diags);

  const TokenKind expected[] = {TOK_PKT,        TOK_VALOF,      TOK_MAX,
                                TOK_BIT_ORDER,  TOK_MSB_FIRST,  TOK_LSB_FIRST,
                                TOK_BYTE_ORDER, TOK_BIG_ENDIAN, TOK_LITTLE_ENDIAN};
  size_t expected_count = sizeof(expected) / sizeof(*expected);

  int failure = 0;
  if (count != expected_count) {
    failure = __LINE__;
  } else {
    for (size_t i = 0; i < expected_count; ++i) {
      if (tokens[i].kind != expected[i]) {
        failure = __LINE__;
        break;
      }
    }
  }

  diag_free(&diags);
  return failure;
}

/* A keyword is reserved: it must not come back as a name, or `pkt` could be a field. */
static int us1_tc03_keywords_are_not_identifiers(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[4];
  size_t count = tokenise("pkt pkts", tokens, 4, &diags);

  int failure = 0;
  if (count != 2 || tokens[0].kind != TOK_PKT || tokens[1].kind != TOK_IDENT) {
    failure = __LINE__;
  } else if (tokens[1].length != 4 || memcmp(tokens[1].text, "pkts", 4) != 0) {
    failure = __LINE__;
  }

  diag_free(&diags);
  return failure;
}

static int us1_tc04_reads_all_three_integer_bases(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[8];
  size_t count = tokenise("170 0xAA 0b10101010 0 0x0 0b0", tokens, 8, &diags);

  int failure = 0;
  if (count != 6 || diags.count != 0) {
    failure = __LINE__;
  } else if (tokens[0].value != 170 || tokens[1].value != 170 || tokens[2].value != 170) {
    failure = __LINE__;
  } else if (tokens[3].value != 0 || tokens[4].value != 0 || tokens[5].value != 0) {
    failure = __LINE__;
  }

  diag_free(&diags);
  return failure;
}

static int us1_tc05_discards_both_comment_forms(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[8];
  size_t count = tokenise("1 // a line comment\n2 /* a block\n comment */ 3", tokens, 8, &diags);

  int failure = 0;
  if (count != 3 || diags.count != 0) {
    failure = __LINE__;
  } else if (tokens[0].value != 1 || tokens[1].value != 2 || tokens[2].value != 3) {
    failure = __LINE__;
  }

  diag_free(&diags);
  return failure;
}

/* The line reported is the one that has to be edited: where the comment opened, not where the file
 * happened to end. */
static int us1_tc06_unterminated_block_comment_names_its_opening_line(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[8];
  tokenise("1\n2\n/* opened here\nand never closed\n", tokens, 8, &diags);

  int failure = 0;
  if (diags.count != 1) {
    failure = __LINE__;
  } else if (diags.items[0].line != 3 || diags.items[0].column != 1) {
    failure = __LINE__;
  }

  diag_free(&diags);
  return failure;
}

static int us1_tc07_tracks_line_and_column(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[8];
  size_t count = tokenise("pkt\n  name\n", tokens, 8, &diags);

  int failure = 0;
  if (count != 2) {
    failure = __LINE__;
  } else if (tokens[0].line != 1 || tokens[0].column != 1) {
    failure = __LINE__;
  } else if (tokens[1].line != 2 || tokens[1].column != 3) {
    failure = __LINE__;
  }

  diag_free(&diags);
  return failure;
}

static int us1_tc08_rejects_a_stray_character(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[8];
  tokenise("a @ b", tokens, 8, &diags);

  int failure = diags.count == 1 ? 0 : __LINE__;
  diag_free(&diags);
  return failure;
}

/* `8bits` is a typo, not the number 8 followed by a name. Lexing it as two tokens would produce a
 * confusing parse error somewhere else. */
static int us1_tc09_rejects_a_number_run_into_a_name(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[8];
  tokenise("8bits", tokens, 8, &diags);

  int failure = diags.count == 1 ? 0 : __LINE__;
  diag_free(&diags);
  return failure;
}

static int us1_tc10_rejects_an_integer_too_large_for_64_bits(const TestArguments *args) {
  (void)args;

  DiagList diags;
  diag_init(&diags);
  Token tokens[8];
  tokenise("99999999999999999999999", tokens, 8, &diags);

  int failure = diags.count == 1 ? 0 : __LINE__;
  diag_free(&diags);
  return failure;
}

void register_user_story1(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us1_tc01_recognises_every_punctuation_token, "every punctuation token is recognised", false);
  ADD_TEST_CASE(us1_tc02_recognises_every_keyword, "every keyword is recognised", false);
  ADD_TEST_CASE(us1_tc03_keywords_are_not_identifiers, "a keyword is reserved, a longer word is not", false);
  ADD_TEST_CASE(us1_tc04_reads_all_three_integer_bases, "decimal, hex and binary integers read alike", false);
  ADD_TEST_CASE(us1_tc05_discards_both_comment_forms, "line and block comments are discarded", false);
  ADD_TEST_CASE(us1_tc06_unterminated_block_comment_names_its_opening_line,
                "an unterminated block comment reports where it opened", false);
  ADD_TEST_CASE(us1_tc07_tracks_line_and_column, "tokens carry their line and column", false);
  ADD_TEST_CASE(us1_tc08_rejects_a_stray_character, "a stray character is reported", false);
  ADD_TEST_CASE(us1_tc09_rejects_a_number_run_into_a_name, "a number running into a name is an error", false);
  ADD_TEST_CASE(us1_tc10_rejects_an_integer_too_large_for_64_bits, "an oversized integer is reported", false);
}
