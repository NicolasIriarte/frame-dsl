#include "format.h"

#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "lexer.h"
#include "parser.h"

/* ---- Text ---------------------------------------------------------------------------------- */

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
} Text;

static bool text_put(Text *text, const char *bytes, size_t length) {
  if (text->length + length + 1 > text->capacity) {
    size_t capacity = text->capacity == 0 ? 64 : text->capacity;
    while (text->length + length + 1 > capacity) {
      capacity *= 2;
    }
    char *grown = realloc(text->data, capacity);
    if (grown == NULL) {
      return false;
    }
    text->data = grown;
    text->capacity = capacity;
  }
  memcpy(text->data + text->length, bytes, length);
  text->length += length;
  text->data[text->length] = '\0';
  return true;
}

static bool text_spaces(Text *text, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (!text_put(text, " ", 1)) {
      return false;
    }
  }
  return true;
}

/* ---- Lines ----------------------------------------------------------------------------------
 *
 * The output is built as lines first and written out last, because alignment is a property of a run
 * of lines and cannot be decided until the whole run has been seen. */

typedef enum {
  /** @brief Written as it was built. */
  LINE_PLAIN,
  /** @brief `head tail`, with every tail in the run starting in one column: a field's `name:` and its
   * width, or an attribute's `@name:` and its value. */
  LINE_ALIGNED,
  /** @brief A comment on a line of its own. */
  LINE_COMMENT,
} LineKind;

typedef struct {
  LineKind kind;
  int depth;
  bool blank_before;
  /** @brief Lines sharing a run align their tails and their trailing comments. */
  unsigned run;
  Text head;
  Text tail;
  /** @brief A comment after the code on the same line, or NULL. */
  const Token *comment;
  /** @brief The source line of the last token put on it: what a trailing comment is matched against. */
  int source_line;
} Line;

typedef struct {
  const char *source;
  Token *tokens;
  size_t count;
  size_t pos;

  Line *lines;
  size_t line_count;
  size_t line_capacity;

  /** @brief The line being built, or -1 between lines. */
  long open;
  /** @brief What the next line opened is: set by the construct about to write it. */
  LineKind next_kind;
  int next_depth;
  bool in_tail;
  /** @brief Set when a comment has cut the line being built in two: the rest goes on continuation
   * lines, indented further, until the construct ends. */
  bool broken;

  /** @brief The source line of the last token or comment consumed. */
  int previous_line;
  /** @brief Whether a blank line came before the token just consumed. */
  bool blank;
  /** @brief For the next line opened: a blank line before it whatever the source said, or none. */
  bool force_blank;
  bool suppress_blank;

  unsigned run;
  unsigned next_run;
  int depth;
  bool failed;
} Formatter;

static Line *line_at(Formatter *f, long index) {
  return index < 0 ? NULL : &f->lines[index];
}

static Line *last_line(Formatter *f) {
  return f->line_count == 0 ? NULL : &f->lines[f->line_count - 1];
}

static Line *add_line(Formatter *f, LineKind kind, int depth) {
  if (f->line_count == f->line_capacity) {
    size_t capacity = f->line_capacity == 0 ? 64 : f->line_capacity * 2;
    Line *grown = realloc(f->lines, capacity * sizeof(*grown));
    if (grown == NULL) {
      f->failed = true;
      return NULL;
    }
    f->lines = grown;
    f->line_capacity = capacity;
  }
  Line *line = &f->lines[f->line_count++];
  memset(line, 0, sizeof(*line));
  line->kind = kind;
  line->depth = depth;
  line->run = f->run;

  if (f->force_blank) {
    line->blank_before = f->line_count > 1;
  } else if (!f->suppress_blank) {
    line->blank_before = f->blank && f->line_count > 1;
  }
  f->force_blank = false;
  f->suppress_blank = false;
  return line;
}

static void new_run(Formatter *f) {
  f->run = ++f->next_run;
}

/* The construct about to write says what its line is; the line itself is created by the first token
 * put on it, so that a comment consumed on the way to that token lands in front of it. */
static void begin(Formatter *f, LineKind kind, int depth) {
  f->next_kind = kind;
  f->next_depth = depth;
  f->in_tail = false;
  f->broken = false;
}

static void end(Formatter *f) {
  f->open = -1;
  f->broken = false;
}

/* Cuts the line being built where it stands. Whatever follows goes on a continuation line, and the
 * cut line no longer aligns: its head and tail are no longer the two halves of one declaration. */
static void cut(Formatter *f) {
  Line *line = line_at(f, f->open);
  if (line != NULL && line->kind == LINE_ALIGNED) {
    line->kind = LINE_PLAIN;
  }
  f->open = -1;
  f->broken = true;
}

/* ---- Tokens and comments ---------------------------------------------------------------------- */

static void comment_line(Formatter *f, const Token *comment) {
  int depth = f->broken ? f->next_depth + 2 : f->next_depth;
  Line *line = add_line(f, LINE_COMMENT, depth);
  if (line != NULL) {
    line->comment = comment;
    line->source_line = comment->line;
  }
}

/* Consumes the comments in front of the next token. One on the same line as what came before it is
 * that line's trailing comment; one on a line of its own is a line of its own. */
static void take_comments(Formatter *f) {
  while (f->pos < f->count && f->tokens[f->pos].kind == TOK_COMMENT) {
    const Token *comment = &f->tokens[f->pos++];
    f->blank = comment->line > f->previous_line + 1;

    Line *open = line_at(f, f->open);
    Line *previous = open != NULL ? open : last_line(f);
    if (previous != NULL && previous->kind != LINE_COMMENT && previous->comment == NULL
        && previous->source_line == comment->line) {
      previous->comment = comment;
    } else {
      if (open != NULL && (open->head.length > 0 || open->tail.length > 0)) {
        cut(f);
      }
      comment_line(f, comment);
    }
    f->previous_line = comment->line;
  }
}

static const Token *peek(Formatter *f) {
  size_t at = f->pos;
  while (at < f->count && f->tokens[at].kind == TOK_COMMENT) {
    at += 1;
  }
  return &f->tokens[at < f->count ? at : f->count - 1];
}

static const Token *next(Formatter *f) {
  take_comments(f);
  const Token *token = &f->tokens[f->pos < f->count ? f->pos : f->count - 1];
  if (f->pos < f->count - 1) {
    f->pos += 1;
  }
  f->blank = token->line > f->previous_line + 1;
  f->previous_line = token->line;
  return token;
}

static bool is_word(const Token *token, const char *word) {
  size_t length = strlen(word);
  return token->kind == TOK_IDENT && token->length == length && memcmp(token->text, word, length) == 0;
}

/* Puts `token` on the line being built, with a space in front of it unless `space` is false or it is
 * the first thing on its half of the line. */
static void put(Formatter *f, const Token *token, bool space) {
  Line *line = line_at(f, f->open);
  if (line != NULL && line->comment != NULL) {
    cut(f);
    line = NULL;
  }
  if (line == NULL) {
    LineKind kind = f->broken ? LINE_PLAIN : f->next_kind;
    int depth = f->broken ? f->next_depth + 2 : f->next_depth;
    line = add_line(f, kind, depth);
    if (line == NULL) {
      return;
    }
    f->open = (long)(line - f->lines);
  }

  Text *half = f->in_tail && line->kind == LINE_ALIGNED ? &line->tail : &line->head;
  if (space && half->length > 0) {
    f->failed |= !text_put(half, " ", 1);
  }
  f->failed |= !text_put(half, token->text, token->length);
  line->source_line = token->line;
}

/* The next token, put on the line. */
static const Token *take(Formatter *f, bool space) {
  const Token *token = next(f);
  put(f, token, space);
  return token;
}

/* From here on the line is its tail: what aligns. */
static void to_tail(Formatter *f) {
  f->in_tail = true;
}

/* ---- Expressions -------------------------------------------------------------------------------
 *
 * One spacing rule for widths, reservations and calibration bodies: a space around every binary
 * operator, none inside parentheses, none between a name and the parenthesis of its call, and none
 * after a unary minus. */

static bool is_operator(TokenKind kind) {
  return kind == TOK_PLUS || kind == TOK_MINUS || kind == TOK_STAR || kind == TOK_SLASH;
}

/* Everything up to one of the stop kinds at parenthesis depth zero, which is left unconsumed. */
static void expression(Formatter *f, TokenKind stop_a, TokenKind stop_b, bool first_spaced) {
  int depth = 0;
  const Token *previous = NULL;
  bool unary = false;

  for (;;) {
    const Token *token = peek(f);
    if (token->kind == TOK_EOF || (depth == 0 && (token->kind == stop_a || token->kind == stop_b))) {
      return;
    }
    if (depth == 0 && is_word(token, "inverse")) {
      return;
    }

    bool space = true;
    if (previous == NULL) {
      space = first_spaced;
    } else if (previous->kind == TOK_LPAREN || token->kind == TOK_RPAREN || unary || token->kind == TOK_DOT
               || previous->kind == TOK_DOT) {
      space = false;
    } else if (token->kind == TOK_LPAREN && (previous->kind == TOK_IDENT || previous->kind == TOK_VALOF)) {
      space = false;
    }

    unary =
      token->kind == TOK_MINUS
      && (previous == NULL || previous->kind == TOK_LPAREN || previous->kind == TOK_EQUALS || is_operator(previous->kind));
    if (token->kind == TOK_LPAREN) {
      depth += 1;
    } else if (token->kind == TOK_RPAREN) {
      depth -= 1;
    }
    previous = take(f, space);
  }
}

/* `a.b.c`, written without spaces. */
static void dotted(Formatter *f, bool space) {
  take(f, space);
  while (peek(f)->kind == TOK_DOT) {
    take(f, false);
    take(f, false);
  }
}

/* ---- Declarations ------------------------------------------------------------------------------ */

/* `@name: value;`, aligned on the value. */
static void attribute(Formatter *f, int depth) {
  begin(f, LINE_ALIGNED, depth);
  take(f, false); /* @ */
  take(f, false); /* name */
  take(f, false); /* : */
  to_tail(f);
  dotted(f, true);
  take(f, false); /* ; */
  end(f);
}

/* `[ @a: b; ]` on one line when it holds one attribute and nothing else; otherwise one attribute per
 * line between `[` and `]`, each aligned on its value. */
static void group(Formatter *f, int depth) {
  size_t open = f->pos;
  while (open < f->count && f->tokens[open].kind == TOK_COMMENT) {
    open += 1;
  }
  size_t attributes = 0;
  bool commented = false;
  for (size_t at = open + 1; at < f->count && f->tokens[at].kind != TOK_RBRACKET && f->tokens[at].kind != TOK_EOF;
       ++at) {
    attributes += f->tokens[at].kind == TOK_AT;
    commented |= f->tokens[at].kind == TOK_COMMENT;
  }

  if (attributes == 1 && !commented) {
    begin(f, LINE_PLAIN, depth);
    take(f, false); /* [ */
    take(f, true); /* @ */
    take(f, false);
    take(f, false); /* : */
    dotted(f, true);
    take(f, false); /* ; */
    take(f, true); /* ] */
    end(f);
    return;
  }

  begin(f, LINE_PLAIN, depth);
  take(f, false); /* [ */
  end(f);

  unsigned outer = f->run;
  new_run(f);
  f->suppress_blank = true;
  while (peek(f)->kind == TOK_AT) {
    attribute(f, depth + 1);
  }
  /* Comments after the last attribute belong inside the group, not in front of its `]`. */
  begin(f, LINE_PLAIN, depth + 1);
  take_comments(f);
  f->run = outer;

  begin(f, LINE_PLAIN, depth);
  f->suppress_blank = true;
  take(f, false); /* ] */
  end(f);
}

static void body(Formatter *f, int depth);

/* ` { a = 1; b.c = 2; }` after a block's name in a use, when there is one. */
static void overrides(Formatter *f) {
  if (peek(f)->kind != TOK_LBRACE) {
    return;
  }
  take(f, true);
  while (peek(f)->kind == TOK_IDENT) {
    dotted(f, true);
    take(f, true); /* = */
    take(f, true); /* value */
    take(f, false); /* ; */
  }
  take(f, true); /* } */
}

/* Whether the declaration ahead is a block pasted in without an instance name, `Block [{ ... }];`: a
 * name, dotted or not, followed by `{` or `;` where a field would have `:` or `[`. */
static bool at_anonymous_use(const Formatter *f) {
  size_t at = f->pos;
  bool expect_name = true;
  for (; at < f->count; ++at) {
    TokenKind kind = f->tokens[at].kind;
    if (kind == TOK_COMMENT) {
      continue;
    }
    if (expect_name ? kind != TOK_IDENT : kind != TOK_DOT) {
      break;
    }
    expect_name = !expect_name;
  }
  return !expect_name && at < f->count && (f->tokens[at].kind == TOK_LBRACE || f->tokens[at].kind == TOK_SEMICOLON);
}

/* `Block [{ overrides }];` on one plain line: it has no `name:` to align on, and does not break the run
 * of fields around it, which still align with each other across it. */
static void anonymous_use(Formatter *f, int depth) {
  begin(f, LINE_PLAIN, depth);
  dotted(f, false);
  overrides(f);
  take(f, false); /* ; */
  end(f);
}

/* `name: width [= value];`, `name[bytes]: width;`, `name: Block [{ overrides }];` or `name: Block[count];` */
static void field(Formatter *f, int depth) {
  begin(f, LINE_ALIGNED, depth);
  take(f, false); /* name */
  if (peek(f)->kind == TOK_LBRACKET) {
    take(f, false);
    expression(f, TOK_RBRACKET, TOK_RBRACKET, false);
    take(f, false); /* ] */
  }
  take(f, false); /* : */
  to_tail(f);

  if (peek(f)->kind == TOK_IDENT) {
    dotted(f, true);
    /* `Block[count]`: a repetition's count, against the name like a reservation (#55). */
    if (peek(f)->kind == TOK_LBRACKET) {
      take(f, false);
      expression(f, TOK_RBRACKET, TOK_RBRACKET, false);
      take(f, false); /* ] */
    }
    overrides(f);
    /* A field typed by an enum, fixed by one of its names: `type: PktType = HK;` (#11). */
    if (peek(f)->kind == TOK_EQUALS) {
      take(f, true);
      take(f, true);
    }
  } else {
    expression(f, TOK_EQUALS, TOK_SEMICOLON, true);
    if (peek(f)->kind == TOK_EQUALS) {
      take(f, true);
      take(f, true);
    }
  }
  take(f, false); /* ; */
  end(f);
}

/* `name { ... };` for a packet or a named block, `block { ... };` for a scope: the opening line, the
 * body a level in, and the closing line. */
static void braced(Formatter *f, int depth) {
  begin(f, LINE_PLAIN, depth);
  while (peek(f)->kind != TOK_LBRACE && peek(f)->kind != TOK_EOF) {
    take(f, true);
  }
  take(f, true); /* { */
  end(f);

  unsigned outer = f->run;
  new_run(f);
  f->suppress_blank = true;
  body(f, depth + 1);
  begin(f, LINE_PLAIN, depth + 1);
  take_comments(f);
  f->run = outer;

  begin(f, LINE_PLAIN, depth);
  f->suppress_blank = true;
  take(f, false); /* } */
  take(f, false); /* ; */
  end(f);
}

static void body(Formatter *f, int depth) {
  while (peek(f)->kind != TOK_RBRACE && peek(f)->kind != TOK_EOF && !f->failed) {
    if (peek(f)->kind == TOK_LBRACKET) {
      group(f, depth);
    }
    if (peek(f)->kind == TOK_BLOCK) {
      braced(f, depth);
      /* A nested block ends the run it interrupted: what follows aligns on its own. */
      new_run(f);
    } else if (at_anonymous_use(f)) {
      anonymous_use(f, depth);
    } else {
      field(f, depth);
    }
  }
}

/* `cal name(x) = body [inverse (y) = body];` on one line. */
static void calibration(Formatter *f) {
  begin(f, LINE_PLAIN, 0);
  take(f, false); /* cal */
  take(f, true); /* name */
  take(f, false); /* ( */
  take(f, false); /* param */
  take(f, false); /* ) */
  take(f, true); /* = */
  expression(f, TOK_SEMICOLON, TOK_SEMICOLON, true);
  if (is_word(peek(f), "inverse")) {
    take(f, true);
    take(f, true); /* ( */
    take(f, false); /* param */
    take(f, false); /* ) */
    take(f, true); /* = */
    expression(f, TOK_SEMICOLON, TOK_SEMICOLON, true);
  }
  take(f, false); /* ; */
  end(f);
}

/* The kind of the second token ahead that is not a comment: what follows the name about to be read. */
static TokenKind second_kind(const Formatter *f) {
  size_t at = f->pos;
  int seen = 0;
  for (; at < f->count; ++at) {
    if (f->tokens[at].kind == TOK_COMMENT) {
      continue;
    }
    if (seen++ == 1) {
      return f->tokens[at].kind;
    }
  }
  return TOK_EOF;
}

/* `enum Name: width {`, one name to a line a level in, and `};` (#11). A name given a value aligns on
 * its `=` with the others that have one; a counted name is only itself. */
static void enumeration(Formatter *f) {
  begin(f, LINE_PLAIN, 0);
  take(f, false); /* enum */
  take(f, true); /* name */
  take(f, false); /* : */
  take(f, true); /* width */
  take(f, true); /* { */
  end(f);

  unsigned outer = f->run;
  new_run(f);
  f->suppress_blank = true;
  while (peek(f)->kind == TOK_IDENT && !f->failed) {
    const bool valued = second_kind(f) == TOK_EQUALS;
    begin(f, valued ? LINE_ALIGNED : LINE_PLAIN, 1);
    take(f, false); /* name */
    if (valued) {
      to_tail(f);
      take(f, false); /* = */
      take(f, true); /* value */
    }
    take(f, false); /* ; */
    end(f);
  }
  begin(f, LINE_PLAIN, 1);
  take_comments(f);
  f->run = outer;

  begin(f, LINE_PLAIN, 0);
  f->suppress_blank = true;
  take(f, false); /* } */
  take(f, false); /* ; */
  end(f);
}

static void import(Formatter *f) {
  begin(f, LINE_PLAIN, 0);
  take(f, false); /* import */
  take(f, true); /* "path" */
  if (is_word(peek(f), "as")) {
    take(f, true);
    take(f, true);
  }
  take(f, false); /* ; */
  end(f);
}

typedef enum {
  ITEM_NONE,
  ITEM_IMPORT,
  ITEM_ATTRIBUTE,
  ITEM_OTHER,
} ItemKind;

static void file(Formatter *f) {
  ItemKind previous = ITEM_NONE;

  while (peek(f)->kind != TOK_EOF && !f->failed) {
    const Token *token = peek(f);
    ItemKind kind = is_word(token, "import") ? ITEM_IMPORT : token->kind == TOK_AT ? ITEM_ATTRIBUTE : ITEM_OTHER;

    /* A run of imports, or of file settings, keeps whatever single blank line it had; anything else
     * is set off from what is above it by exactly one. */
    if (previous == ITEM_NONE) {
      f->suppress_blank = true;
      new_run(f);
    } else if (kind != previous || kind == ITEM_OTHER) {
      f->force_blank = true;
      new_run(f);
    } else if (f->pos < f->count && f->tokens[f->pos].line > f->previous_line + 1) {
      new_run(f);
    }

    begin(f, LINE_PLAIN, 0);
    if (kind == ITEM_IMPORT) {
      import(f);
    } else if (kind == ITEM_ATTRIBUTE) {
      attribute(f, 0);
    } else if (is_word(token, "cal")) {
      calibration(f);
    } else if (is_word(token, "enum")) {
      enumeration(f);
    } else {
      if (token->kind == TOK_LBRACKET) {
        group(f, 0);
      }
      braced(f, 0);
    }
    previous = kind;
  }

  /* Comments after the last declaration. */
  begin(f, LINE_PLAIN, 0);
  take_comments(f);
}

/* ---- Writing it out ---------------------------------------------------------------------------- */

static size_t code_width(const Line *line, size_t head_width) {
  if (line->kind == LINE_ALIGNED) {
    return (size_t)line->depth * 2 + head_width + 1 + line->tail.length;
  }
  return (size_t)line->depth * 2 + line->head.length + (line->tail.length > 0 ? 1 + line->tail.length : 0);
}

static char *render(Formatter *f) {
  /* Per run: the widest head among its aligned lines, then the widest code among its lines that carry
   * a comment. Runs are numbered densely from 1, so an array indexed by run is enough. */
  size_t runs = f->next_run + 1;
  size_t *head = calloc(runs, sizeof(*head));
  size_t *comment = calloc(runs, sizeof(*comment));
  Text out = { 0 };
  if (head == NULL || comment == NULL) {
    free(head);
    free(comment);
    return NULL;
  }

  for (size_t i = 0; i < f->line_count; ++i) {
    const Line *line = &f->lines[i];
    if (line->kind == LINE_ALIGNED && line->head.length > head[line->run]) {
      head[line->run] = line->head.length;
    }
  }
  for (size_t i = 0; i < f->line_count; ++i) {
    const Line *line = &f->lines[i];
    if (line->kind != LINE_COMMENT && line->comment != NULL) {
      size_t width = code_width(line, head[line->run]);
      if (width > comment[line->run]) {
        comment[line->run] = width;
      }
    }
  }

  bool ok = true;
  for (size_t i = 0; i < f->line_count && ok; ++i) {
    const Line *line = &f->lines[i];
    if (line->blank_before) {
      ok &= text_put(&out, "\n", 1);
    }
    ok &= text_spaces(&out, (size_t)line->depth * 2);

    if (line->kind == LINE_COMMENT) {
      ok &= text_put(&out, line->comment->text, line->comment->length);
      ok &= text_put(&out, "\n", 1);
      continue;
    }

    size_t width = (size_t)line->depth * 2;
    if (line->kind == LINE_ALIGNED) {
      ok &= text_put(&out, line->head.data, line->head.length);
      ok &= text_spaces(&out, head[line->run] - line->head.length + 1);
      ok &= text_put(&out, line->tail.data, line->tail.length);
    } else {
      ok &= text_put(&out, line->head.data != NULL ? line->head.data : "", line->head.length);
      if (line->tail.length > 0) {
        ok &= text_put(&out, " ", 1);
        ok &= text_put(&out, line->tail.data, line->tail.length);
      }
    }
    width = code_width(line, head[line->run]);

    if (line->comment != NULL) {
      ok &= text_spaces(&out, comment[line->run] - width + 2);
      ok &= text_put(&out, line->comment->text, line->comment->length);
    }
    ok &= text_put(&out, "\n", 1);
  }

  free(head);
  free(comment);
  if (!ok || out.data == NULL) {
    free(out.data);
    return ok ? calloc(1, 1) : NULL;
  }
  return out.data;
}

char *format_source(const char *source, DiagList *diags) {
  /* Refused unless it parses: a formatter that guessed at a broken file would be rewriting the one
   * thing the author most needs to see as they wrote it. */
  size_t errors = diag_error_count(diags);
  AstFile *ast = parse(source, diags);
  bool parsed = ast != NULL && diag_error_count(diags) == errors;
  ast_free(ast);
  if (!parsed) {
    return NULL;
  }

  Formatter f = { .source = source, .open = -1, .previous_line = 1 };
  size_t capacity = 256;
  f.tokens = malloc(capacity * sizeof(*f.tokens));
  if (f.tokens == NULL) {
    return NULL;
  }

  DiagList scratch;
  diag_init(&scratch);
  Lexer lexer;
  lexer_init(&lexer, source, &scratch);
  for (;;) {
    if (f.count == capacity) {
      capacity *= 2;
      Token *grown = realloc(f.tokens, capacity * sizeof(*grown));
      if (grown == NULL) {
        free(f.tokens);
        diag_free(&scratch);
        return NULL;
      }
      f.tokens = grown;
    }
    Token token = lexer_next_spelled(&lexer);
    f.tokens[f.count++] = token;
    if (token.kind == TOK_EOF || token.kind == TOK_INVALID) {
      break;
    }
  }
  diag_free(&scratch);

  file(&f);
  char *out = f.failed ? NULL : render(&f);

  for (size_t i = 0; i < f.line_count; ++i) {
    free(f.lines[i].head.data);
    free(f.lines[i].tail.data);
  }
  free(f.lines);
  free(f.tokens);
  return out;
}
