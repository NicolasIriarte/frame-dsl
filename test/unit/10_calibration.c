/* Story 10: calibrations, from the source to the compiled curve.
 *
 * The language half -- real literals, `cal`, `@cal:`, checks 17 to 22 -- and the compiler half: a
 * curve that is a line must come out as one, whatever route the source took to say so, and one that
 * is not must still compute what the source says. What a caller sees through the API is story 9 of
 * the system tests. */
#include <stdio.h>
#include <string.h>

#include "calibration.h"
#include "check.h"
#include "diag.h"
#include "lexer.h"
#include "parser.h"
#include "testsuite.h"

/* Parses and checks `source`. `wanted` is the check that must fire, or -1 for "must load cleanly". */
static bool checks_as(const char *source, int wanted) {
  DiagList diags;
  diag_init(&diags);

  AstFile *file = parse(source, &diags);
  bool found = false;

  if (diags.count == 0 && file != NULL) {
    Layout *layout = check_file(file, &diags);
    for (size_t i = 0; i < diags.count; ++i) {
      if (diags.items[i].check == wanted) {
        found = true;
      }
    }
    if (wanted < 0) {
      found = diags.count == 0 && layout != NULL;
    }
    layout_free(layout);
  }

  ast_free(file);
  diag_free(&diags);
  return found;
}

/* Whether parsing alone reports something containing `text`. */
static bool parse_reports(const char *source, const char *text) {
  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse(source, &diags);

  bool found = false;
  for (size_t i = 0; i < diags.count; ++i) {
    if (strstr(diags.items[i].message, text) != NULL) {
      found = true;
    }
  }
  ast_free(file);
  diag_free(&diags);
  return found;
}

/* A compiled table for `source`, which must load; the caller frees both. */
typedef struct {
  DiagList diags;
  AstFile *file;
  Layout *layout;
  CalTable *table;
} Compiled;

static bool compile(const char *source, Compiled *out) {
  diag_init(&out->diags);
  out->file = parse(source, &out->diags);
  out->layout = out->diags.count == 0 ? check_file(out->file, &out->diags) : NULL;
  out->table = out->layout != NULL ? cal_compile(out->file, &out->diags) : NULL;
  return out->table != NULL;
}

static void release(Compiled *compiled) {
  cal_table_free(compiled->table);
  layout_free(compiled->layout);
  ast_free(compiled->file);
  diag_free(&compiled->diags);
}

/* Without libm: nothing else in the tests links it. */
static double distance(double a, double b) {
  return a > b ? a - b : b - a;
}

#define PKT "pkt A { id: 8 := 1; [ @cal: k; ] v: 16; };"

static int us10_tc01_lexes_real_literals(const TestArguments *args) {
  (void)args;
  DiagList diags;
  diag_init(&diags);

  const struct {
    const char *text;
    TokenKind kind;
    double value;
  } cases[] = {
    { "0.0625", TOK_REAL, 0.0625 },
    { "4e-3",   TOK_REAL, 0.004  },
    { "1.5E2",  TOK_REAL, 150.0  },
    { "12",     TOK_INT,  12.0   },
    { "0x10",   TOK_INT,  16.0   },
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i) {
    Lexer lexer;
    lexer_init(&lexer, cases[i].text, &diags);
    Token token = lexer_next(&lexer);
    if (token.kind != cases[i].kind || token.real != cases[i].value) {
      diag_free(&diags);
      return __LINE__;
    }
  }

  /* `8.` and `1e` are typos, not reals: a digit has to follow. */
  Lexer lexer;
  lexer_init(&lexer, "8.", &diags);
  Token first = lexer_next(&lexer);
  lexer_init(&lexer, "1e", &diags);
  Token second = lexer_next(&lexer);
  bool ok = first.kind == TOK_INVALID && second.kind == TOK_INVALID && diags.count == 2;
  diag_free(&diags);
  return ok ? 0 : __LINE__;
}

static int us10_tc02_parses_a_cal_with_and_without_an_inverse(const TestArguments *args) {
  (void)args;
  DiagList diags;
  diag_init(&diags);
  AstFile *file = parse(
    "cal k(raw) = -raw * 2.5 + 1;\n"
    "cal d(r) = 10 * r / (4095 - r) inverse (e) = 4095 * e / (10000 + e);\n" PKT,
    &diags
  );

  bool ok = diags.count == 0 && file != NULL && file->cal_count == 2 && strcmp(file->cals[0].param, "raw") == 0
            && file->cals[0].inverse == NULL && file->cals[1].inverse != NULL
            && strcmp(file->cals[1].inverse_param, "e") == 0 && file->packets[0].fields[1].cal != NULL
            && strcmp(file->packets[0].fields[1].cal, "k") == 0;

  ast_free(file);
  diag_free(&diags);
  return ok ? 0 : __LINE__;
}

/* `cal` and `inverse` are words only where a calibration is expected, so neither is reserved: a
 * telemetry file with a field called `cal` must go on loading. */
static int us10_tc03_cal_and_inverse_are_not_reserved(const TestArguments *args) {
  (void)args;
  if (!checks_as("pkt cal { cal: 8 = 1; inverse: 8; };", -1)) {
    return __LINE__;
  }
  if (!checks_as("cal inverse(cal) = cal * 2; pkt A { id: 8 := 1; [ @cal: inverse; ] v: 8; };", -1)) {
    return __LINE__;
  }
  return 0;
}

/* `@cal:` belongs to one field. Anywhere it would be inherited, it is refused rather than guessed. */
static int us10_tc04_cal_attaches_only_to_a_field(const TestArguments *args) {
  (void)args;
  const char *sources[] = {
    "@cal: k;\ncal k(x) = x; pkt A { id: 8 := 1; };",
    "cal k(x) = x; [ @cal: k; ] pkt A { id: 8 := 1; };",
    "cal k(x) = x; pkt A { id: 8 := 1; [ @cal: k; ] block { v: 8; }; };",
  };
  for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
    if (!parse_reports(sources[i], "attaches a calibration to one field")) {
      return __LINE__;
    }
  }
  return 0;
}

static int us10_tc05_a_width_is_not_a_real(const TestArguments *args) {
  (void)args;
  if (!parse_reports("pkt A { id: 8 := 1; v: 2.5; };", "not a whole number")) {
    FORCE_FAILURE();
  }
  return 0;
}

static int us10_tc06_check_17_duplicate_calibration(const TestArguments *args) {
  (void)args;
  return checks_as("cal k(x) = x; cal k(x) = x * 2;\n" PKT, 17) ? 0 : __LINE__;
}

static int us10_tc07_check_18_unknown_calibration(const TestArguments *args) {
  (void)args;
  if (!checks_as(PKT, 18)) {
    return __LINE__;
  }
  if (!checks_as("cal k(x) = nope(x);\n" PKT, 18)) {
    return __LINE__;
  }
  return 0;
}

static int us10_tc08_check_19_a_name_that_is_not_the_parameter(const TestArguments *args) {
  (void)args;
  if (!checks_as("cal k(x) = y + 1;\n" PKT, 19)) {
    return __LINE__;
  }
  /* The inverse has its own parameter, and the forward one is not in scope there. */
  if (!checks_as("cal k(x) = x * x inverse (e) = x;\n" PKT, 19)) {
    return __LINE__;
  }
  return 0;
}

static int us10_tc09_check_20_recursion(const TestArguments *args) {
  (void)args;
  if (!checks_as("cal k(x) = k(x);\n" PKT, 20)) {
    return __LINE__;
  }
  if (!checks_as("cal k(x) = j(x) + 1; cal j(x) = k(x);\n" PKT, 20)) {
    return __LINE__;
  }
  /* An inverse calling its own forward curve is a different function, not recursion. */
  if (!checks_as("cal k(x) = x * x inverse (e) = e / k(2);\n" PKT, -1)) {
    return __LINE__;
  }
  return 0;
}

static int us10_tc10_check_21_a_bit_string_is_not_calibrated(const TestArguments *args) {
  (void)args;
  if (!checks_as("cal k(x) = x; pkt A { id: 8 := 1; [ @cal: k; ] v: 128; };", 21)) {
    return __LINE__;
  }
  if (!checks_as("cal k(x) = x; pkt A { id: 8 := 1; n: 8; [ @cal: k; ] v[4]: valof(n) * 8; };", 21)) {
    return __LINE__;
  }
  return 0;
}

static int us10_tc11_check_22_a_constant_zero_divisor(const TestArguments *args) {
  (void)args;
  if (!checks_as("cal k(x) = x / (2 - 2);\n" PKT, 22)) {
    return __LINE__;
  }
  /* A divisor that is zero only for some raw values is a run-time matter, not a load-time one. */
  if (!checks_as("cal k(x) = 1 / (4095 - x);\n" PKT, -1)) {
    return __LINE__;
  }
  return 0;
}

/* However a line is written -- divided, negated, through a call -- it compiles to scale and offset. */
static int us10_tc12_a_line_compiles_to_scale_and_offset(const TestArguments *args) {
  (void)args;
  Compiled compiled;
  if (!compile(
        "cal v(raw) = raw * 3.3 / 4095;\n"
        "cal i(raw) = v(raw) / 0.05;\n"
        "cal t(raw) = -(40 - raw / 16);\n"
        "cal k(raw) = 7;\n" PKT,
        &compiled
      )) {
    release(&compiled);
    return __LINE__;
  }

  const CalCompiled *cals = compiled.table->cals;
  int failure = 0;
  if (!cals[0].forward.linear || distance(cals[0].forward.scale, 3.3 / 4095) > 1e-15 || cals[0].forward.offset != 0) {
    failure = __LINE__;
  } else if (!cals[1].forward.linear || distance(cals[1].forward.scale, 3.3 / 4095 / 0.05) > 1e-15) {
    failure = __LINE__;
  } else if (!cals[2].forward.linear || cals[2].forward.scale != 0.0625 || cals[2].forward.offset != -40) {
    failure = __LINE__;
  } else if (!cals[0].invertible || !cals[0].inverse_solves_line) {
    failure = __LINE__;
  } else if (!cals[3].forward.linear || cals[3].forward.scale != 0 || cals[3].invertible) {
    /* A constant is a line with no slope, and a line with no slope has no inverse. */
    failure = __LINE__;
  }

  release(&compiled);
  return failure;
}

/* Not a line: code, with calls inlined and constants folded, computing what the source says. */
static int us10_tc13_a_curve_compiles_to_code_that_agrees_with_the_source(const TestArguments *args) {
  (void)args;
  Compiled compiled;
  if (!compile(
        "cal sq(x) = x * x;\n"
        "cal k(raw) = sq(raw + 1) / (2 * 4) - sq(3);\n" PKT,
        &compiled
      )) {
    release(&compiled);
    return __LINE__;
  }

  const CalCompiled *k = &compiled.table->cals[1];
  int failure = 0;
  if (k->forward.linear || k->invertible) {
    failure = __LINE__;
  }

  /* `(2 * 4)` and `sq(3)` are constants and must be single literals, never computed per call. */
  size_t literals = 0;
  for (uint32_t i = 0; i < k->forward.length; ++i) {
    if (k->forward.code[i].op == CAL_OP_LIT) {
      literals += 1;
      double value = k->forward.code[i].literal;
      if (value != 1 && value != 8 && value != 9) {
        failure = __LINE__;
      }
    }
  }
  if (literals != 3) {
    failure = __LINE__;
  }

  for (double raw = 0; raw < 100 && failure == 0; raw += 7) {
    double got = 0;
    if (!cal_forward(k, raw, &got) || got != (raw + 1) * (raw + 1) / 8 - 9) {
      failure = __LINE__;
    }
  }

  release(&compiled);
  return failure;
}

void register_user_story10(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us10_tc01_lexes_real_literals, "reals lex with a fraction or an exponent, and only then", true);
  ADD_TEST_CASE(us10_tc02_parses_a_cal_with_and_without_an_inverse, "a cal parses, with or without an inverse", false);
  ADD_TEST_CASE(us10_tc03_cal_and_inverse_are_not_reserved, "'cal' and 'inverse' remain usable as names", false);
  ADD_TEST_CASE(us10_tc04_cal_attaches_only_to_a_field, "@cal is refused on a file, a packet or a block", false);
  ADD_TEST_CASE(us10_tc05_a_width_is_not_a_real, "a width written as a real says why it is refused", false);
  ADD_TEST_CASE(us10_tc06_check_17_duplicate_calibration, "check 17: calibration names are unique", false);
  ADD_TEST_CASE(us10_tc07_check_18_unknown_calibration, "check 18: a named calibration must exist", false);
  ADD_TEST_CASE(
    us10_tc08_check_19_a_name_that_is_not_the_parameter, "check 19: a bare name must be the parameter", false
  );
  ADD_TEST_CASE(us10_tc09_check_20_recursion, "check 20: no recursion, and an inverse may call its curve", false);
  ADD_TEST_CASE(us10_tc10_check_21_a_bit_string_is_not_calibrated, "check 21: only a scalar is calibrated", false);
  ADD_TEST_CASE(
    us10_tc11_check_22_a_constant_zero_divisor, "check 22: a constant zero divisor is refused at load", false
  );
  ADD_TEST_CASE(us10_tc12_a_line_compiles_to_scale_and_offset, "a line compiles to scale and offset", false);
  ADD_TEST_CASE(
    us10_tc13_a_curve_compiles_to_code_that_agrees_with_the_source,
    "a curve compiles to folded, inlined code that agrees with the source",
    false
  );
}
