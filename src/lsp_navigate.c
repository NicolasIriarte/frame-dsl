/* Hover, go to definition and completion. See lsp_navigate.h for the three views of a document this
 * reads, and why an empty answer is always a legal one. */
#define _XOPEN_SOURCE 700

#include "lsp_navigate.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "diag.h"
#include "framedsl.h"
#include "import.h"
#include "lexer.h"
#include "lsp_semantic.h"
#include "parser.h"

const char *const NAV_ATTRIBUTE_NAMES[] = { "bit_order", "byte_order", "cal", NULL };
static const char *const BIT_ORDER_VALUES[] = { "msb_first", "lsb_first", NULL };
static const char *const BYTE_ORDER_VALUES[] = { "big_endian", "little_endian", NULL };

struct Navigator {
  char *source;
  char *path;

  /** @brief The significant tokens: comments are dropped, since nothing here is about one. */
  Token *tokens;
  size_t count;
  SemanticToken *semantic;
  size_t semantic_count;

  /** @brief Parsed and imports resolved, not composed: every use is still a use, where it was written. */
  AstFile *ast;
  DiagList diags;

  /** @brief The full load, and its definition when every check passed; NULL otherwise. */
  FramedslResult *result;
  const FramedslDefinition *definition;
};

/* ---- Text ------------------------------------------------------------------------------------ */

typedef struct {
  char *data;
  size_t length;
  size_t capacity;
  bool failed;
} Buffer;

static void put(Buffer *buffer, const char *format, ...) {
  if (buffer->failed) {
    return;
  }
  va_list args;
  va_start(args, format);
  va_list again;
  va_copy(again, args);
  int needed = vsnprintf(NULL, 0, format, args);
  va_end(args);
  if (needed < 0) {
    buffer->failed = true;
    va_end(again);
    return;
  }
  if (buffer->length + (size_t)needed + 1 > buffer->capacity) {
    size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
    while (buffer->length + (size_t)needed + 1 > capacity) {
      capacity *= 2;
    }
    char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
      buffer->failed = true;
      va_end(again);
      return;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
  }
  vsnprintf(buffer->data + buffer->length, (size_t)needed + 1, format, again);
  va_end(again);
  buffer->length += (size_t)needed;
}

/* The buffer's text, handed over, or NULL if it is empty or ran out of memory on the way. */
static char *take(Buffer *buffer) {
  if (buffer->failed || buffer->length == 0) {
    free(buffer->data);
    return NULL;
  }
  return buffer->data;
}

static char *copy_of(const char *text, size_t length) {
  char *copy = malloc(length + 1);
  if (copy != NULL) {
    memcpy(copy, text, length);
    copy[length] = '\0';
  }
  return copy;
}

/* Line `line` (from 1) of `text`, without its newline or leading blanks; NULL past the end. */
static char *line_of(const char *text, int line) {
  const char *at = text;
  for (int i = 1; i < line && at != NULL; ++i) {
    at = strchr(at, '\n');
    at = at != NULL ? at + 1 : NULL;
  }
  if (at == NULL) {
    return NULL;
  }
  while (*at == ' ' || *at == '\t') {
    ++at;
  }
  size_t length = strcspn(at, "\r\n");
  return copy_of(at, length);
}

/* The same, from a file on disk: an imported declaration's line lives in its own file. */
static char *line_of_file(const char *path, int line) {
  FILE *handle = fopen(path, "r");
  if (handle == NULL) {
    return NULL;
  }
  char buffer[1024];
  char *found = NULL;
  for (int i = 1; fgets(buffer, sizeof(buffer), handle) != NULL; ++i) {
    if (i == line) {
      const char *at = buffer + strspn(buffer, " \t");
      found = copy_of(at, strcspn(at, "\r\n"));
      break;
    }
  }
  fclose(handle);
  return found;
}

/* The part after the last dot: what is written at an imported declaration's position is `Header`,
 * though this file calls it `link.Header`. */
static const char *last_segment(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot != NULL ? dot + 1 : name;
}

/* ---- Opening a document ---------------------------------------------------------------------- */

Navigator *nav_open(const char *source, const char *path) {
  Navigator *navigator = calloc(1, sizeof(*navigator));
  if (navigator == NULL) {
    return NULL;
  }
  diag_init(&navigator->diags);
  navigator->source = copy_of(source, strlen(source));
  navigator->path = path != NULL ? copy_of(path, strlen(path)) : NULL;
  if (navigator->source == NULL || (path != NULL && navigator->path == NULL)) {
    nav_close(navigator);
    return NULL;
  }

  size_t all_count = 0;
  Token *all = lsp_lex_all(navigator->source, &all_count);
  navigator->tokens = malloc((all_count == 0 ? 1 : all_count) * sizeof(Token));
  if (navigator->tokens == NULL) {
    free(all);
    nav_close(navigator);
    return NULL;
  }
  for (size_t i = 0; i < all_count; ++i) {
    if (all[i].kind != TOK_COMMENT) {
      navigator->tokens[navigator->count++] = all[i];
    }
  }
  free(all);
  navigator->semantic = semantic_tokens(navigator->source, &navigator->semantic_count);

  /* The tree as far as the parser got. Imports are resolved even past a syntax error -- the names
   * they bring are what completion after `ns.` needs, and that is typed into a file that does not
   * parse yet. Resolving reads only the import lines, which come first. */
  navigator->ast = parse(navigator->source, &navigator->diags);
  if (navigator->ast != NULL) {
    char directory[PATH_MAX] = ".";
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    if (slash != NULL) {
      snprintf(directory, sizeof(directory), "%.*s", slash == path ? 1 : (int)(slash - path), path);
    }
    char real[PATH_MAX];
    const char *self = path != NULL && realpath(path, real) != NULL ? real : NULL;
    imports_resolve(navigator->ast, directory, self, &navigator->diags);
  }

  navigator->result =
    path != NULL ? framedsl_load_source(navigator->source, path) : framedsl_load_string(navigator->source, "<buffer>");
  if (framedsl_ok(navigator->result)) {
    navigator->definition = framedsl_definition(navigator->result);
  }
  return navigator;
}

void nav_close(Navigator *navigator) {
  if (navigator == NULL) {
    return;
  }
  framedsl_result_free(navigator->result);
  ast_free(navigator->ast);
  diag_free(&navigator->diags);
  free(navigator->semantic);
  free(navigator->tokens);
  free(navigator->path);
  free(navigator->source);
  free(navigator);
}

/* ---- Where the cursor is --------------------------------------------------------------------- */

static bool at_or_before(int line, int column, int other_line, int other_column) {
  return line < other_line || (line == other_line && column <= other_column);
}

/* The token under (line, column), or -1. */
static long token_at(const Navigator *navigator, int line, int column) {
  for (size_t i = 0; i < navigator->count; ++i) {
    const Token *token = &navigator->tokens[i];
    if (token->line == line && token->column <= column && column < token->column + (int)token->length) {
      return (long)i;
    }
  }
  return -1;
}

static const Token *token(const Navigator *navigator, long index) {
  return index >= 0 && (size_t)index < navigator->count ? &navigator->tokens[index] : NULL;
}

static bool is(const Token *token, TokenKind kind) {
  return token != NULL && token->kind == kind;
}

static bool spells(const Token *token, const char *word) {
  size_t length = strlen(word);
  return token != NULL && token->length == length && memcmp(token->text, word, length) == 0;
}

static SemanticType semantic_type(const Navigator *navigator, const Token *token) {
  for (size_t i = 0; i < navigator->semantic_count; ++i) {
    if (navigator->semantic[i].line == token->line && navigator->semantic[i].column == token->column) {
      return navigator->semantic[i].type;
    }
  }
  return SEM_TYPE_COUNT;
}

static bool is_declaration(const Navigator *navigator, const Token *token) {
  for (size_t i = 0; i < navigator->semantic_count; ++i) {
    if (navigator->semantic[i].line == token->line && navigator->semantic[i].column == token->column) {
      return navigator->semantic[i].declaration;
    }
  }
  return false;
}

/* The whole dotted name the token at `index` is part of -- `link.Header` from either half. Owned. */
static char *dotted_at(const Navigator *navigator, long index) {
  long first = index;
  while (is(token(navigator, first - 1), TOK_DOT) && is(token(navigator, first - 2), TOK_IDENT)) {
    first -= 2;
  }
  long last = index;
  while (is(token(navigator, last + 1), TOK_DOT) && is(token(navigator, last + 2), TOK_IDENT)) {
    last += 2;
  }
  Buffer buffer = { 0 };
  for (long i = first; i <= last; ++i) {
    const Token *part = token(navigator, i);
    put(&buffer, "%.*s", (int)part->length, part->text);
  }
  return take(&buffer);
}

/* The packet or named block of this file whose braces hold (line, column), or NULL. */
static const AstPacket *container_at(const Navigator *navigator, int line, int column, bool *is_block) {
  const AstPacket *best = NULL;
  const AstFile *ast = navigator->ast;
  if (ast == NULL) {
    return NULL;
  }
  for (int pass = 0; pass < 2; ++pass) {
    const AstPacket *items = pass == 0 ? ast->packets : ast->blocks;
    size_t count = pass == 0 ? ast->packet_count : ast->block_count;
    for (size_t i = 0; i < count; ++i) {
      const AstPacket *candidate = &items[i];
      if (candidate->origin != NULL || candidate->name == NULL
          || !at_or_before(candidate->line, candidate->column, line, column)) {
        continue;
      }
      /* A body the parser never saw close runs to wherever the next declaration starts. */
      if (candidate->end_line > 0 && !at_or_before(line, column, candidate->end_line, candidate->end_column)) {
        continue;
      }
      if (best == NULL || at_or_before(best->line, best->column, candidate->line, candidate->column)) {
        best = candidate;
        *is_block = pass == 1;
      }
    }
  }
  return best;
}

static const AstPacket *find_block(const AstFile *ast, const char *name) {
  for (size_t i = 0; ast != NULL && i < ast->block_count; ++i) {
    if (ast->blocks[i].name != NULL && strcmp(ast->blocks[i].name, name) == 0) {
      return &ast->blocks[i];
    }
  }
  return NULL;
}

static const AstCal *find_cal(const AstFile *ast, const char *name) {
  for (size_t i = 0; ast != NULL && i < ast->cal_count; ++i) {
    if (ast->cals[i].name != NULL && strcmp(ast->cals[i].name, name) == 0) {
      return &ast->cals[i];
    }
  }
  return NULL;
}

static const AstImport *find_import(const AstFile *ast, const char *alias) {
  for (const AstImport *import = ast != NULL ? ast->imports : NULL; import != NULL; import = import->next) {
    if (import->alias != NULL && strcmp(import->alias, alias) == 0) {
      return import;
    }
  }
  return NULL;
}

static const AstField *field_in(const AstPacket *container, const char *name) {
  for (size_t i = 0; container != NULL && i < container->field_count; ++i) {
    const AstField *field = &container->fields[i];
    if (!field->anonymous && field->name != NULL && strcmp(field->name, name) == 0) {
      return field;
    }
  }
  return NULL;
}

/* A field `path` of `block`, following instances down: `inner.id` is `id` of the block `inner` uses.
 * An imported block was composed in its own file, so its fields are already flat and a path matches
 * whole. `*owner` is the block the field is written in. */
static const AstField *resolve_path(
  const AstFile *ast,
  const AstPacket *block,
  const char *path,
  const AstPacket **owner
) {
  for (int depth = 0; block != NULL && depth < 64; ++depth) {
    const AstField *whole = field_in(block, path);
    if (whole != NULL) {
      *owner = block;
      return whole;
    }
    const char *dot = strchr(path, '.');
    if (dot == NULL) {
      /* Pasted in without a name: look inside each such use for it. */
      for (size_t i = 0; i < block->field_count; ++i) {
        const AstField *use = &block->fields[i];
        if (use->anonymous && use->block != NULL) {
          const AstField *found = resolve_path(ast, find_block(ast, use->block), path, owner);
          if (found != NULL) {
            return found;
          }
        }
      }
      return NULL;
    }
    char head[256];
    snprintf(head, sizeof(head), "%.*s", (int)(dot - path), path);
    const AstField *instance = field_in(block, head);
    if (instance == NULL || instance->block == NULL) {
      return NULL;
    }
    block = find_block(ast, instance->block);
    path = dot + 1;
  }
  return NULL;
}

/* The use whose override list has one written at (line, column), and which part of its path the
 * column is on: `inner` or `inner.id` for a cursor on either half of `inner.id`. */
static const AstField *override_at(const AstPacket *container, int line, int column, char *path, size_t capacity) {
  for (size_t i = 0; container != NULL && i < container->field_count; ++i) {
    const AstField *use = &container->fields[i];
    for (const AstOverride *override = use->overrides; override != NULL; override = override->next) {
      int length = (int)strlen(override->name);
      if (override->line != line || column < override->column || column >= override->column + length) {
        continue;
      }
      const char *end = strchr(override->name + (column - override->column), '.');
      int keep = end != NULL ? (int)(end - override->name) : length;
      snprintf(path, capacity, "%.*s", keep, override->name);
      return use;
    }
  }
  return NULL;
}

/* ---- Facts ----------------------------------------------------------------------------------- */

static bool fold(const Expr *expr, uint64_t *out) {
  if (expr == NULL) {
    return false;
  }
  uint64_t lhs = 0;
  uint64_t rhs = 0;
  switch (expr->kind) {
  case EXPR_LITERAL:
    *out = expr->literal;
    return true;
  case EXPR_VALOF:
    return false;
  case EXPR_BINARY:
    if (!fold(expr->lhs, &lhs) || !fold(expr->rhs, &rhs)) {
      return false;
    }
    switch (expr->op) {
    case '+':
      *out = lhs + rhs;
      return true;
    case '-':
      *out = lhs - rhs;
      return lhs >= rhs;
    case '*':
      *out = lhs * rhs;
      return true;
    case '/':
      *out = rhs != 0 ? lhs / rhs : 0;
      return rhs != 0;
    default:
      return false;
    }
  }
  return false;
}

static const char *bit_order_name(BitOrder order) {
  return order == BIT_ORDER_LSB_FIRST ? "lsb_first" : "msb_first";
}

static const char *byte_order_name(ByteOrder order) {
  return order == BYTE_ORDER_LITTLE_ENDIAN ? "little_endian" : "big_endian";
}

/* One field as the loaded definition lays it out: where it is and how it is read. */
static bool laid_out(const Navigator *navigator, const char *packet, const char *name, FramedslField *out) {
  size_t packet_index = 0;
  size_t field_index = 0;
  return navigator->definition != NULL && framedsl_find_packet(navigator->definition, packet, &packet_index)
         && framedsl_find_field(navigator->definition, packet_index, name, &field_index)
         && framedsl_field(navigator->definition, packet_index, field_index, out);
}

static void describe_layout(Buffer *buffer, const FramedslField *field) {
  if (field->variable) {
    put(buffer, "- width: variable, up to %llu bits reserved\n", (unsigned long long)field->max_bits);
  } else {
    put(buffer, "- width: %llu bits\n", (unsigned long long)field->width_bits);
  }
  if (field->offset_known) {
    put(buffer, "- offset: bit %llu", (unsigned long long)field->offset_bits);
    if (field->offset_bits % 8 == 0) {
      put(buffer, " (byte %llu)", (unsigned long long)(field->offset_bits / 8));
    }
    put(buffer, "\n");
  } else {
    put(buffer, "- offset: not fixed -- it follows a field whose width varies\n");
  }
  put(
    buffer,
    "- bit order: %s, byte order: %s\n",
    field->bit_order == FRAMEDSL_BIT_ORDER_LSB_FIRST ? "lsb_first" : "msb_first",
    field->byte_order == FRAMEDSL_BYTE_ORDER_LITTLE_ENDIAN ? "little_endian" : "big_endian"
  );
  if (field->has_fixed) {
    put(buffer, "- fixed at 0x%llX\n", (unsigned long long)field->fixed);
  }
  if (field->cal != NULL) {
    put(buffer, "- calibrated by `%s`\n", field->cal);
  }
}

/* What the tree alone can say about a declaration: enough for a block, whose fields have no offset of
 * their own, and for a packet whose file does not load yet. */
static void describe_declaration(Buffer *buffer, const AstField *decl) {
  uint64_t width = 0;
  if (fold(decl->width, &width)) {
    put(buffer, "- width: %llu bits\n", (unsigned long long)width);
  } else if (decl->width != NULL) {
    put(buffer, "- width: variable%s\n", decl->reserve != NULL ? ", with a reservation" : "");
  }
  put(buffer, "- bit order: %s, byte order: %s\n", bit_order_name(decl->bit_order), byte_order_name(decl->byte_order));
  if (decl->has_fixed) {
    put(buffer, "- fixed at 0x%llX\n", (unsigned long long)decl->fixed);
  }
  if (decl->cal != NULL) {
    put(buffer, "- calibrated by `%s`\n", decl->cal);
  }
}

static void describe_field(
  Buffer *buffer,
  const Navigator *navigator,
  const AstPacket *container,
  bool is_block,
  const AstField *decl,
  const char *expanded
) {
  put(buffer, "**%s** -- field of %s `%s`\n\n", expanded, is_block ? "block" : "pkt", container->name);
  FramedslField field;
  if (!is_block && laid_out(navigator, container->name, expanded, &field)) {
    describe_layout(buffer, &field);
    return;
  }
  if (decl != NULL) {
    describe_declaration(buffer, decl);
  }
  put(
    buffer,
    is_block ? "\nWhere it lands depends on the packet using the block.\n"
             : "\nIts offset is known once the file loads without errors.\n"
  );
}

/* Every field an instance expanded to, `hdr.*`, taken together. */
static void describe_instance(
  Buffer *buffer,
  const Navigator *navigator,
  const AstPacket *container,
  bool is_block,
  const AstField *use,
  const char *expanded
) {
  put(
    buffer,
    "**%s** -- instance of block `%s` in %s `%s`\n\n",
    expanded,
    use->block,
    is_block ? "block" : "pkt",
    container->name
  );
  size_t packet = 0;
  if (is_block || navigator->definition == NULL
      || !framedsl_find_packet(navigator->definition, container->name, &packet)) {
    return;
  }
  size_t prefix = strlen(expanded);
  uint64_t first = UINT64_MAX;
  uint64_t end = 0;
  bool known = true;
  size_t fields = 0;
  for (size_t i = 0; i < framedsl_field_count(navigator->definition, packet); ++i) {
    FramedslField field;
    if (!framedsl_field(navigator->definition, packet, i, &field) || strncmp(field.name, expanded, prefix) != 0
        || field.name[prefix] != '.') {
      continue;
    }
    fields += 1;
    known = known && field.offset_known && !field.variable;
    if (known) {
      first = field.offset_bits < first ? field.offset_bits : first;
      end = field.offset_bits + field.width_bits > end ? field.offset_bits + field.width_bits : end;
    }
  }
  if (fields > 0 && known) {
    put(
      buffer,
      "- bits %llu to %llu: %llu bits, %zu fields\n",
      (unsigned long long)first,
      (unsigned long long)(end - 1),
      (unsigned long long)(end - first),
      fields
    );
  } else if (fields > 0) {
    put(buffer, "- %zu fields; where they land varies with the frame\n", fields);
  }
}

static void describe_block(Buffer *buffer, const AstPacket *block) {
  put(buffer, "**block %s**", block->name);
  if (block->origin != NULL) {
    put(buffer, " -- from `%s`", block->origin);
  }
  put(buffer, "\n\n```fdsl\n");
  for (size_t i = 0; i < block->field_count; ++i) {
    const AstField *field = &block->fields[i];
    uint64_t width = 0;
    if (field->block != NULL && field->anonymous) {
      put(buffer, "%s;\n", field->block);
    } else if (field->block != NULL) {
      put(buffer, "%s: %s;\n", field->name, field->block);
    } else if (fold(field->width, &width)) {
      put(buffer, "%s: %llu", field->name, (unsigned long long)width);
      if (field->has_fixed) {
        put(buffer, " = 0x%llX", (unsigned long long)field->fixed);
      }
      put(buffer, ";\n");
    } else {
      put(buffer, "%s: (variable);\n", field->name);
    }
  }
  put(buffer, "```\n");
}

static void describe_cal(Buffer *buffer, const Navigator *navigator, const AstCal *cal) {
  put(buffer, "**cal %s**", cal->name);
  if (cal->origin != NULL) {
    put(buffer, " -- from `%s`", cal->origin);
  }
  put(buffer, "\n\n");
  char *line = cal->origin != NULL ? line_of_file(cal->origin, cal->line) : line_of(navigator->source, cal->line);
  if (line != NULL) {
    put(buffer, "```fdsl\n%s\n```\n", line);
    free(line);
  }
}

static void describe_import(Buffer *buffer, const AstImport *import) {
  put(buffer, "**import** `\"%s\"` as `%s`\n\n", import->path, import->alias);
  for (size_t i = 0; i < import->brought_count; ++i) {
    put(buffer, "%s`%s`", i == 0 ? "brings " : ", ", last_segment(import->brought[i]));
  }
  if (import->brought_count > 0) {
    put(buffer, "\n");
  }
}

static void describe_packet(Buffer *buffer, const Navigator *navigator, const AstPacket *packet) {
  put(buffer, "**pkt %s**\n\n", packet->name);
  size_t index = 0;
  if (navigator->definition == NULL || !framedsl_find_packet(navigator->definition, packet->name, &index)) {
    return;
  }
  size_t count = framedsl_field_count(navigator->definition, index);
  uint64_t end = 0;
  bool fixed = true;
  for (size_t i = 0; i < count; ++i) {
    FramedslField field;
    if (framedsl_field(navigator->definition, index, i, &field)) {
      fixed = fixed && field.offset_known && !field.variable;
      if (fixed && field.offset_bits + field.width_bits > end) {
        end = field.offset_bits + field.width_bits;
      }
    }
  }
  put(buffer, "- %zu fields\n", count);
  if (fixed) {
    put(buffer, "- %llu bits, %llu bytes\n", (unsigned long long)end, (unsigned long long)(end / 8));
  } else {
    put(buffer, "- its length varies with the frame\n");
  }
}

static void describe_attribute(Buffer *buffer, const Token *name) {
  if (spells(name, "bit_order")) {
    put(
      buffer, "**@bit_order**: `msb_first` (the default) or `lsb_first` -- the order of a field's bits on the wire.\n"
    );
  } else if (spells(name, "byte_order")) {
    put(
      buffer,
      "**@byte_order**: `big_endian` (the default) or `little_endian` -- the order of the bytes of a "
      "byte-aligned field a whole number of bytes wide.\n"
    );
  } else if (spells(name, "cal")) {
    put(buffer, "**@cal**: the calibration that turns this field's raw value into engineering units.\n");
  }
}

/* ---- Answers --------------------------------------------------------------------------------- */

typedef enum {
  TARGET_NONE,
  TARGET_PACKET,
  TARGET_BLOCK,
  TARGET_CAL,
  TARGET_IMPORT,
  TARGET_FIELD,
  TARGET_INSTANCE,
  TARGET_ATTRIBUTE,
} TargetKind;

/* What a name under the cursor refers to, worked out once for hover and definition alike. */
typedef struct {
  TargetKind kind;
  const Token *token;
  const AstPacket *packet; /* TARGET_PACKET, TARGET_BLOCK */
  const AstCal *cal;
  const AstImport *import;
  /* TARGET_FIELD / TARGET_INSTANCE: where the name is used, and the declaration it means. */
  const AstPacket *container;
  bool container_is_block;
  const AstField *decl;
  const AstPacket *decl_owner;
  char expanded[512];
} Target;

static Target find_target(const Navigator *navigator, int line, int column) {
  Target target = { .kind = TARGET_NONE };
  long index = token_at(navigator, line, column);
  const Token *here = token(navigator, index);
  if (here == NULL || here->kind != TOK_IDENT || navigator->ast == NULL) {
    return target;
  }
  target.token = here;
  const AstFile *ast = navigator->ast;
  const SemanticType type = semantic_type(navigator, here);

  if (type == SEM_DECORATOR) {
    target.kind = is(token(navigator, index - 1), TOK_AT) ? TARGET_ATTRIBUTE : TARGET_NONE;
    return target;
  }

  char *dotted = dotted_at(navigator, index);
  if (dotted == NULL) {
    return target;
  }

  if (type == SEM_TYPE) {
    for (size_t i = 0; i < ast->packet_count; ++i) {
      if (ast->packets[i].name != NULL && strcmp(ast->packets[i].name, dotted) == 0) {
        target.kind = TARGET_PACKET;
        target.packet = &ast->packets[i];
      }
    }
  } else if (type == SEM_STRUCT) {
    target.packet = find_block(ast, dotted);
    target.kind = target.packet != NULL ? TARGET_BLOCK : TARGET_NONE;
  } else if (type == SEM_FUNCTION) {
    target.cal = find_cal(ast, dotted);
    target.kind = target.cal != NULL ? TARGET_CAL : TARGET_NONE;
  } else if (type == SEM_NAMESPACE) {
    char alias[256];
    snprintf(alias, sizeof(alias), "%.*s", (int)here->length, here->text);
    target.import = find_import(ast, alias);
    target.kind = target.import != NULL ? TARGET_IMPORT : TARGET_NONE;
  } else if (type == SEM_PROPERTY) {
    target.container = container_at(navigator, here->line, here->column, &target.container_is_block);
    char name[256];
    snprintf(name, sizeof(name), "%.*s", (int)here->length, here->text);
    char path[256];
    const AstField *use = NULL;

    if (target.container == NULL) {
      /* Nothing to look in. */
    } else if (is_declaration(navigator, here)) {
      target.decl = field_in(target.container, name);
      target.decl_owner = target.container;
      if (target.decl != NULL) {
        target.kind = target.decl->block != NULL ? TARGET_INSTANCE : TARGET_FIELD;
        snprintf(target.expanded, sizeof(target.expanded), "%s", name);
      }
    } else if (is(token(navigator, index - 1), TOK_LPAREN) && is(token(navigator, index - 2), TOK_VALOF)) {
      target.decl = field_in(target.container, name);
      target.decl_owner = target.container;
      if (target.decl != NULL) {
        target.kind = TARGET_FIELD;
        snprintf(target.expanded, sizeof(target.expanded), "%s", name);
      }
    } else if ((use = override_at(target.container, here->line, here->column, path, sizeof(path))) != NULL) {
      target.decl = resolve_path(ast, find_block(ast, use->block), path, &target.decl_owner);
      if (target.decl != NULL) {
        target.kind = target.decl->block != NULL ? TARGET_INSTANCE : TARGET_FIELD;
        if (use->anonymous) {
          snprintf(target.expanded, sizeof(target.expanded), "%s", path);
        } else {
          snprintf(target.expanded, sizeof(target.expanded), "%s.%s", use->name, path);
        }
      }
    }
  }
  free(dotted);
  return target;
}

char *nav_hover(const Navigator *navigator, int line, int column, NavLocation *range) {
  Target target = find_target(navigator, line, column);
  if (target.kind == TARGET_NONE) {
    return NULL;
  }

  Buffer buffer = { 0 };
  switch (target.kind) {
  case TARGET_PACKET:
    describe_packet(&buffer, navigator, target.packet);
    break;
  case TARGET_BLOCK:
    describe_block(&buffer, target.packet);
    break;
  case TARGET_CAL:
    describe_cal(&buffer, navigator, target.cal);
    break;
  case TARGET_IMPORT:
    describe_import(&buffer, target.import);
    break;
  case TARGET_FIELD:
    describe_field(&buffer, navigator, target.container, target.container_is_block, target.decl, target.expanded);
    break;
  case TARGET_INSTANCE:
    describe_instance(&buffer, navigator, target.container, target.container_is_block, target.decl, target.expanded);
    break;
  case TARGET_ATTRIBUTE:
    describe_attribute(&buffer, target.token);
    break;
  case TARGET_NONE:
    break;
  }

  char *text = take(&buffer);
  if (text != NULL && range != NULL) {
    *range = (NavLocation){
      .file = NULL,
      .line = target.token->line,
      .column = target.token->column,
      .end_line = target.token->line,
      .end_column = target.token->column + (int)target.token->length,
    };
  }
  return text;
}

static bool locate(const char *origin, int line, int column, const char *name, NavLocation *out) {
  *out = (NavLocation){
    .file = origin != NULL ? copy_of(origin, strlen(origin)) : NULL,
    .line = line,
    .column = column,
    .end_line = line,
    .end_column = column + (int)strlen(last_segment(name)),
  };
  return origin == NULL || out->file != NULL;
}

bool nav_definition(const Navigator *navigator, int line, int column, NavLocation *out) {
  Target target = find_target(navigator, line, column);
  switch (target.kind) {
  case TARGET_PACKET:
  case TARGET_BLOCK:
    return locate(
      target.packet->origin, target.packet->name_line, target.packet->name_column, target.packet->name, out
    );
  case TARGET_CAL:
    return locate(target.cal->origin, target.cal->name_line, target.cal->name_column, target.cal->name, out);
  case TARGET_IMPORT:
    return locate(NULL, target.import->line, target.import->column, "import", out);
  case TARGET_FIELD:
  case TARGET_INSTANCE:
    return locate(
      target.decl_owner != NULL ? target.decl_owner->origin : NULL,
      target.decl->line,
      target.decl->column,
      target.decl->name,
      out
    );
  case TARGET_ATTRIBUTE:
  case TARGET_NONE:
    break;
  }
  return false;
}

/* ---- Completion ------------------------------------------------------------------------------ */

typedef struct {
  NavCompletion *items;
  size_t count;
  size_t capacity;
  const char *prefix;
  size_t prefix_length;
  bool failed;
} Completions;

static void offer(Completions *completions, const char *label, NavKind kind, const char *detail) {
  if (completions->failed || strncmp(label, completions->prefix, completions->prefix_length) != 0) {
    return;
  }
  for (size_t i = 0; i < completions->count; ++i) {
    if (strcmp(completions->items[i].label, label) == 0) {
      return;
    }
  }
  if (completions->count == completions->capacity) {
    size_t capacity = completions->capacity == 0 ? 16 : completions->capacity * 2;
    NavCompletion *grown = realloc(completions->items, capacity * sizeof(*grown));
    if (grown == NULL) {
      completions->failed = true;
      return;
    }
    completions->items = grown;
    completions->capacity = capacity;
  }
  NavCompletion *item = &completions->items[completions->count];
  item->label = copy_of(label, strlen(label));
  item->kind = kind;
  item->detail = detail != NULL ? copy_of(detail, strlen(detail)) : NULL;
  if (item->label == NULL) {
    completions->failed = true;
    return;
  }
  completions->count += 1;
}

static void offer_list(Completions *completions, const char *const *labels, NavKind kind) {
  for (size_t i = 0; labels[i] != NULL; ++i) {
    offer(completions, labels[i], kind, NULL);
  }
}

static void offer_blocks(Completions *completions, const AstFile *ast) {
  for (size_t i = 0; ast != NULL && i < ast->block_count; ++i) {
    const AstPacket *block = &ast->blocks[i];
    if (block->name != NULL) {
      offer(completions, block->name, NAV_BLOCK, block->origin);
    }
  }
}

static void offer_cals(Completions *completions, const AstFile *ast) {
  for (size_t i = 0; ast != NULL && i < ast->cal_count; ++i) {
    if (ast->cals[i].name != NULL) {
      offer(completions, ast->cals[i].name, NAV_CAL, ast->cals[i].origin);
    }
  }
}

/* The scalar fields of the container declared above (line, column): what a valof() may name. */
static void offer_scalars(Completions *completions, const AstPacket *container, int line, int column) {
  for (size_t i = 0; container != NULL && i < container->field_count; ++i) {
    const AstField *field = &container->fields[i];
    uint64_t width = 0;
    if (field->block == NULL && field->name != NULL && at_or_before(field->line, field->column, line, column)
        && fold(field->width, &width) && width > 0 && width <= FRAMEDSL_SCALAR_MAX_BITS) {
      char detail[64];
      snprintf(detail, sizeof(detail), "%llu bits", (unsigned long long)width);
      offer(completions, field->name, NAV_FIELD, detail);
    }
  }
}

/* The block whose override braces the token at `index` sits inside, or NULL when the innermost open
 * brace is a packet's or block's body. */
static const AstPacket *override_block(const Navigator *navigator, long index) {
  int depth = 0;
  for (long i = index; i >= 0; --i) {
    const Token *here = token(navigator, i);
    if (here->kind == TOK_RBRACE) {
      depth += 1;
    } else if (here->kind == TOK_LBRACE && depth > 0) {
      depth -= 1;
    } else if (here->kind == TOK_LBRACE) {
      const Token *name = token(navigator, i - 1);
      const Token *before = token(navigator, i - 2);
      if (!is(name, TOK_IDENT) || is(before, TOK_PKT) || is(before, TOK_BLOCK)) {
        return NULL;
      }
      char *dotted = dotted_at(navigator, i - 1);
      const AstPacket *block = dotted != NULL ? find_block(navigator->ast, dotted) : NULL;
      free(dotted);
      return block;
    }
  }
  return NULL;
}

NavCompletion *nav_complete(const Navigator *navigator, int line, int column, size_t *count) {
  *count = 0;
  Completions completions = { .prefix = "" };

  /* A name being typed is the prefix, and the question is about what comes before it. */
  long before = -1;
  for (size_t i = 0; i < navigator->count; ++i) {
    const Token *here = &navigator->tokens[i];
    if (!at_or_before(here->line, here->column, line, column) || (here->line == line && here->column == column)) {
      break;
    }
    before = (long)i;
  }
  const Token *last = token(navigator, before);
  if (is(last, TOK_IDENT) && last->line == line && last->column + (int)last->length >= column) {
    completions.prefix = last->text;
    completions.prefix_length = (size_t)(column - last->column);
    before -= 1;
  }

  const Token *previous = token(navigator, before);
  const Token *second = token(navigator, before - 1);
  const Token *third = token(navigator, before - 2);
  int depth = 0;
  for (long i = 0; i <= before; ++i) {
    depth += is(token(navigator, i), TOK_LBRACE) ? 1 : is(token(navigator, i), TOK_RBRACE) ? -1 : 0;
  }
  bool is_block = false;
  const AstPacket *container = container_at(navigator, line, column, &is_block);

  if (is(previous, TOK_AT)) {
    offer_list(&completions, NAV_ATTRIBUTE_NAMES, NAV_ATTRIBUTE);
  } else if (is(previous, TOK_COLON) && is(second, TOK_IDENT) && is(third, TOK_AT)) {
    if (spells(second, "bit_order")) {
      offer_list(&completions, BIT_ORDER_VALUES, NAV_VALUE);
    } else if (spells(second, "byte_order")) {
      offer_list(&completions, BYTE_ORDER_VALUES, NAV_VALUE);
    } else if (spells(second, "cal")) {
      offer_cals(&completions, navigator->ast);
    }
  } else if (is(previous, TOK_LPAREN) && is(second, TOK_VALOF)) {
    offer_scalars(&completions, container, line, column);
  } else if (is(previous, TOK_DOT) && is(second, TOK_IDENT)) {
    char alias[256];
    snprintf(alias, sizeof(alias), "%.*s", (int)second->length, second->text);
    const AstImport *import = find_import(navigator->ast, alias);
    /* `@cal: ns.` wants a calibration; anywhere else a namespaced name is a block. */
    const bool want_cal = is(token(navigator, before - 2), TOK_COLON) && spells(token(navigator, before - 3), "cal")
                          && is(token(navigator, before - 4), TOK_AT);
    for (size_t i = 0; import != NULL && i < import->brought_count; ++i) {
      bool block = find_block(navigator->ast, import->brought[i]) != NULL;
      if (block != want_cal) {
        offer(&completions, last_segment(import->brought[i]), block ? NAV_BLOCK : NAV_CAL, import->path);
      }
    }
  } else if (is(previous, TOK_COLON) && depth > 0) {
    offer_blocks(&completions, navigator->ast);
    offer(&completions, "valof", NAV_KEYWORD, NULL);
  } else if (
    depth > 0
    && (previous == NULL || is(previous, TOK_SEMICOLON) || is(previous, TOK_LBRACE) || is(previous, TOK_RBRACKET))) {
    const AstPacket *block = override_block(navigator, before);
    if (block != NULL) {
      for (size_t i = 0; i < block->field_count; ++i) {
        if (block->fields[i].name != NULL && !block->fields[i].anonymous) {
          offer(&completions, block->fields[i].name, NAV_FIELD, NULL);
        }
      }
    } else {
      offer_blocks(&completions, navigator->ast);
      offer(&completions, "block", NAV_KEYWORD, NULL);
    }
  } else if (depth == 0 && (previous == NULL || is(previous, TOK_SEMICOLON) || is(previous, TOK_RBRACKET))) {
    static const char *const TOP[] = { "pkt", "block", "cal", "import", NULL };
    offer_list(&completions, TOP, NAV_KEYWORD);
  }

  if (completions.failed) {
    nav_completions_free(completions.items, completions.count);
    return NULL;
  }
  *count = completions.count;
  return completions.items != NULL ? completions.items : calloc(1, sizeof(NavCompletion));
}

void nav_completions_free(NavCompletion *items, size_t count) {
  for (size_t i = 0; items != NULL && i < count; ++i) {
    free(items[i].label);
    free(items[i].detail);
  }
  free(items);
}
