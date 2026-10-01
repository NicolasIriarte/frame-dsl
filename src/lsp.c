/* The language server. See lsp.h for what it is and why it may reach inside the library.
 *
 * The shape of the thing: read a message, act on it, write what the protocol asks for. Every answer
 * about a document comes from loading that document's text through framedsl_load_source(), the same
 * call `--check` makes, so nothing here decides what a mistake is. */
#define _XOPEN_SOURCE 700

#include "lsp.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "format.h"
#include "framedsl.h"
#include "json.h"
#include "lexer.h"
#include "lsp_navigate.h"
#include "lsp_semantic.h"
#include "lsp_text.h"

/* A definition file is a few dozen lines. A message larger than this is not one an editor sent about
 * one, and reading it would mean trusting a length field to size an allocation. */
#define LSP_MAX_MESSAGE (16u * 1024u * 1024u)

typedef struct {
  char *uri;
  /** @brief The file the URI names, so imports resolve beside it; NULL for a URI that is not a file. */
  char *path;
  char *text;
  int64_t version;
} Document;

typedef struct {
  FILE *in;
  FILE *out;
  bool initialized;
  bool shutdown_requested;
  /** @brief UTF-16 positions unless the editor accepted UTF-8 ones. The protocol's default. */
  bool utf16;

  Document *documents;
  size_t count;
  size_t capacity;
} Server;

/* ---- Messages in and out ----------------------------------------------------------------------- */

/* One message: `Content-Length: N`, whatever other headers, a blank line, then N bytes. Any header
 * that is not the length is ignored -- including Content-Type, which has one legal value. */
static char *read_message(FILE *in, size_t *length) {
  size_t content = 0;
  bool have_length = false;

  for (;;) {
    char line[512];
    if (fgets(line, sizeof(line), in) == NULL) {
      return NULL;
    }
    size_t used = strlen(line);
    while (used > 0 && (line[used - 1] == '\n' || line[used - 1] == '\r')) {
      line[--used] = '\0';
    }
    if (used == 0) {
      break;
    }
    if (strncmp(line, "Content-Length:", 15) == 0) {
      char *end = NULL;
      unsigned long value = strtoul(line + 15, &end, 10);
      if (end != line + 15 && value <= LSP_MAX_MESSAGE) {
        content = (size_t)value;
        have_length = true;
      } else {
        return NULL;
      }
    }
  }
  if (!have_length) {
    return NULL;
  }

  char *body = malloc(content + 1);
  if (body == NULL) {
    return NULL;
  }
  if (fread(body, 1, content, in) != content) {
    free(body);
    return NULL;
  }
  body[content] = '\0';
  *length = content;
  return body;
}

static void write_message(Server *server, JsonWriter *writer) {
  if (writer->failed) {
    return;
  }
  fprintf(server->out, "Content-Length: %zu\r\n\r\n", writer->length);
  fwrite(writer->data, 1, writer->length, server->out);
  fflush(server->out);
}

static void begin_message(JsonWriter *writer) {
  json_writer_init(writer);
  json_object_begin(writer);
  json_key(writer, "jsonrpc");
  json_write_cstring(writer, "2.0");
}

/* The envelope of a response, up to and including its "result" key: the caller writes the value. */
static void begin_response(JsonWriter *writer, const Json *id) {
  begin_message(writer);
  json_key(writer, "id");
  json_write_value(writer, id);
  json_key(writer, "result");
}

static void send_error(Server *server, const Json *id, int code, const char *message) {
  JsonWriter writer;
  begin_message(&writer);
  json_key(&writer, "id");
  json_write_value(&writer, id);
  json_key(&writer, "error");
  json_object_begin(&writer);
  json_key(&writer, "code");
  json_write_integer(&writer, code);
  json_key(&writer, "message");
  json_write_cstring(&writer, message);
  json_object_end(&writer);
  json_object_end(&writer);
  write_message(server, &writer);
  json_writer_free(&writer);
}

/* ---- Documents ---------------------------------------------------------------------------------- */

static int hex_digit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/* `file:///path/with%20spaces.fdsl` to a path. NULL for anything else: an unsaved buffer arrives as
 * `untitled:` and has no directory for its imports to be relative to. */
static char *path_of_uri(const char *uri) {
  if (strncmp(uri, "file://", 7) != 0) {
    return NULL;
  }
  const char *at = uri + 7;
  /* An authority, when there is one, is the host; only an empty one (a local file) is understood. */
  const char *slash = strchr(at, '/');
  if (slash == NULL) {
    return NULL;
  }
  at = slash;

  char *path = malloc(strlen(at) + 1);
  if (path == NULL) {
    return NULL;
  }
  size_t used = 0;
  while (*at != '\0') {
    if (at[0] == '%' && hex_digit(at[1]) >= 0 && hex_digit(at[2]) >= 0) {
      path[used++] = (char)(hex_digit(at[1]) * 16 + hex_digit(at[2]));
      at += 3;
    } else {
      path[used++] = *at++;
    }
  }
  path[used] = '\0';
  return path;
}

/* The other way, for pointing at a file the document imports. */
static char *uri_of_path(const char *path) {
  char absolute[PATH_MAX];
  if (realpath(path, absolute) == NULL) {
    snprintf(absolute, sizeof(absolute), "%s", path);
  }
  size_t length = strlen(absolute);
  char *uri = malloc(length * 3 + 8);
  if (uri == NULL) {
    return NULL;
  }
  memcpy(uri, "file://", 7);
  size_t used = 7;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)absolute[i];
    bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '-'
                 || c == '_' || c == '.' || c == '~';
    if (plain) {
      uri[used++] = (char)c;
    } else {
      used += (size_t)snprintf(uri + used, 4, "%%%02X", c);
    }
  }
  uri[used] = '\0';
  return uri;
}

static Document *find_document(Server *server, const char *uri) {
  for (size_t i = 0; i < server->count; ++i) {
    if (strcmp(server->documents[i].uri, uri) == 0) {
      return &server->documents[i];
    }
  }
  return NULL;
}

static Document *open_document(Server *server, const char *uri, char *text, int64_t version) {
  Document *document = find_document(server, uri);
  if (document != NULL) {
    free(document->text);
    document->text = text;
    document->version = version;
    return document;
  }

  if (server->count == server->capacity) {
    size_t capacity = server->capacity == 0 ? 8 : server->capacity * 2;
    Document *grown = realloc(server->documents, capacity * sizeof(*grown));
    if (grown == NULL) {
      free(text);
      return NULL;
    }
    server->documents = grown;
    server->capacity = capacity;
  }

  document = &server->documents[server->count];
  memset(document, 0, sizeof(*document));
  document->uri = malloc(strlen(uri) + 1);
  if (document->uri == NULL) {
    free(text);
    return NULL;
  }
  strcpy(document->uri, uri);
  document->path = path_of_uri(uri);
  document->text = text;
  document->version = version;
  server->count += 1;
  return document;
}

static void close_document(Server *server, const char *uri) {
  for (size_t i = 0; i < server->count; ++i) {
    if (strcmp(server->documents[i].uri, uri) != 0) {
      continue;
    }
    free(server->documents[i].uri);
    free(server->documents[i].path);
    free(server->documents[i].text);
    server->documents[i] = server->documents[--server->count];
    return;
  }
}

/* ---- Diagnostics -------------------------------------------------------------------------------- */

static void write_range(JsonWriter *writer, const LspText *text, int line, int column, int end_line, int end_column) {
  size_t start_l;
  size_t start_c;
  size_t end_l;
  size_t end_c;
  lsp_text_position(text, line, column, &start_l, &start_c);
  lsp_text_position(text, end_line > 0 ? end_line : line, end_line > 0 ? end_column : column, &end_l, &end_c);

  json_object_begin(writer);
  json_key(writer, "start");
  json_object_begin(writer);
  json_key(writer, "line");
  json_write_integer(writer, (int64_t)start_l);
  json_key(writer, "character");
  json_write_integer(writer, (int64_t)start_c);
  json_object_end(writer);
  json_key(writer, "end");
  json_object_begin(writer);
  json_key(writer, "line");
  json_write_integer(writer, (int64_t)end_l);
  json_key(writer, "character");
  json_write_integer(writer, (int64_t)end_c);
  json_object_end(writer);
  json_object_end(writer);
}

/* The line of the `import` that brought `file` in, or 0 when this document imports it through
 * something else -- a library importing a library -- and there is no line of its own to point at. */
static int import_line_of(const Document *document, const char *file) {
  char directory[PATH_MAX];
  const char *base = document->path != NULL ? document->path : "";
  const char *slash = strrchr(base, '/');
  if (slash == NULL) {
    snprintf(directory, sizeof(directory), ".");
  } else {
    snprintf(directory, sizeof(directory), "%.*s", slash == base ? 1 : (int)(slash - base), base);
  }

  DiagList scratch;
  diag_init(&scratch);
  Lexer lexer;
  lexer_init(&lexer, document->text, &scratch);
  int found = 0;
  Token previous = { 0 };
  for (;;) {
    Token token = lexer_next(&lexer);
    if (token.kind == TOK_EOF || token.kind == TOK_INVALID) {
      break;
    }
    if (token.kind == TOK_STRING && previous.kind == TOK_IDENT && previous.length == 6
        && memcmp(previous.text, "import", 6) == 0) {
      /* The same name the loader would have reported the file under: relative to this one, unless the
       * import is absolute. Built by hand because both halves are bounded separately. */
      char shown[PATH_MAX];
      size_t used = 0;
      if (!(token.length > 0 && token.text[0] == '/')) {
        size_t directory_length = strlen(directory);
        if (directory_length + 2 < sizeof(shown)) {
          memcpy(shown, directory, directory_length);
          used = directory_length;
          shown[used++] = '/';
        }
      }
      if (used + token.length < sizeof(shown)) {
        memcpy(shown + used, token.text, token.length);
        used += token.length;
      }
      shown[used] = '\0';
      if (strcmp(shown, file) == 0) {
        found = previous.line;
        break;
      }
    }
    previous = token;
  }
  diag_free(&scratch);
  return found;
}

static void write_diagnostic(
  JsonWriter *writer,
  const Document *document,
  const LspText *text,
  const FramedslDiagnostic *diagnostic
) {
  json_object_begin(writer);
  json_key(writer, "range");

  char message[1024];
  if (diagnostic->file == NULL) {
    write_range(writer, text, diagnostic->line, diagnostic->column, diagnostic->end_line, diagnostic->end_column);
    snprintf(message, sizeof(message), "%s", diagnostic->message);
  } else {
    /* A problem in an imported file is shown on the import that brought it in: that file is not the
     * one on screen, and its own position means nothing here. */
    int line = import_line_of(document, diagnostic->file);
    write_range(writer, text, line > 0 ? line : 1, 1, line > 0 ? line : 1, line > 0 ? 7 : 1);
    snprintf(
      message,
      sizeof(message),
      "in %s:%d:%d: %s",
      diagnostic->file,
      diagnostic->line,
      diagnostic->column,
      diagnostic->message
    );
  }

  json_key(writer, "severity");
  json_write_integer(writer, diagnostic->severity == FRAMEDSL_SEVERITY_WARNING ? 2 : 1);
  if (diagnostic->check > 0) {
    json_key(writer, "code");
    json_write_integer(writer, diagnostic->check);
  }
  json_key(writer, "source");
  json_write_cstring(writer, "frame-dsl");
  json_key(writer, "message");
  json_write_cstring(writer, message);

  if (diagnostic->file != NULL) {
    /* Where it really is, so "go to the problem" lands in the imported file. */
    char *uri = uri_of_path(diagnostic->file);
    if (uri != NULL) {
      json_key(writer, "relatedInformation");
      json_array_begin(writer);
      json_object_begin(writer);
      json_key(writer, "location");
      json_object_begin(writer);
      json_key(writer, "uri");
      json_write_cstring(writer, uri);
      json_key(writer, "range");
      json_object_begin(writer);
      for (int end = 0; end < 2; ++end) {
        json_key(writer, end == 0 ? "start" : "end");
        json_object_begin(writer);
        json_key(writer, "line");
        json_write_integer(writer, diagnostic->line > 0 ? diagnostic->line - 1 : 0);
        json_key(writer, "character");
        json_write_integer(writer, diagnostic->column > 0 ? diagnostic->column - 1 : 0);
        json_object_end(writer);
      }
      json_object_end(writer);
      json_object_end(writer);
      json_key(writer, "message");
      json_write_cstring(writer, diagnostic->message);
      json_object_end(writer);
      json_array_end(writer);
      free(uri);
    }
  }
  json_object_end(writer);
}

static void publish_diagnostics(Server *server, const Document *document) {
  FramedslResult *result = document->path != NULL ? framedsl_load_source(document->text, document->path)
                                                  : framedsl_load_string(document->text, document->uri);
  LspText text;
  if (!lsp_text_init(&text, document->text, strlen(document->text), server->utf16)) {
    framedsl_result_free(result);
    return;
  }

  JsonWriter writer;
  begin_message(&writer);
  json_key(&writer, "method");
  json_write_cstring(&writer, "textDocument/publishDiagnostics");
  json_key(&writer, "params");
  json_object_begin(&writer);
  json_key(&writer, "uri");
  json_write_cstring(&writer, document->uri);
  json_key(&writer, "version");
  json_write_integer(&writer, document->version);
  json_key(&writer, "diagnostics");
  json_array_begin(&writer);
  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    write_diagnostic(&writer, document, &text, framedsl_diagnostic(result, i));
  }
  json_array_end(&writer);
  json_object_end(&writer);
  json_object_end(&writer);
  write_message(server, &writer);

  json_writer_free(&writer);
  lsp_text_free(&text);
  framedsl_result_free(result);
}

/* Every open document, because what one says can depend on another: saving a library changes what the
 * files importing it are told. */
static void publish_all(Server *server) {
  for (size_t i = 0; i < server->count; ++i) {
    publish_diagnostics(server, &server->documents[i]);
  }
}

/* ---- Requests ------------------------------------------------------------------------------------ */

static void handle_initialize(Server *server, const Json *id, const Json *params) {
  /* UTF-8 positions when the editor accepts them -- the compiler counts bytes, and then nothing has to
   * be converted. UTF-16 is the protocol's default and what an editor gets if it says nothing. */
  const Json *encodings = json_get(json_get(params, "general"), "positionEncodings");
  server->utf16 = true;
  if (encodings != NULL && encodings->kind == JSON_ARRAY) {
    for (size_t i = 0; i < encodings->count; ++i) {
      const char *name = json_string(encodings->items[i]);
      if (name != NULL && strcmp(name, "utf-8") == 0) {
        server->utf16 = false;
        break;
      }
    }
  }

  JsonWriter writer;
  begin_response(&writer, id);
  json_object_begin(&writer);
  json_key(&writer, "capabilities");
  json_object_begin(&writer);

  json_key(&writer, "positionEncoding");
  json_write_cstring(&writer, server->utf16 ? "utf-16" : "utf-8");

  json_key(&writer, "textDocumentSync");
  json_object_begin(&writer);
  json_key(&writer, "openClose");
  json_write_bool(&writer, true);
  /* Full text on every change: a definition file is small, and incremental sync is bookkeeping with
   * nothing to buy. */
  json_key(&writer, "change");
  json_write_integer(&writer, 1);
  json_key(&writer, "save");
  json_object_begin(&writer);
  json_key(&writer, "includeText");
  json_write_bool(&writer, false);
  json_object_end(&writer);
  json_object_end(&writer);

  json_key(&writer, "documentFormattingProvider");
  json_write_bool(&writer, true);

  json_key(&writer, "codeActionProvider");
  json_object_begin(&writer);
  json_key(&writer, "codeActionKinds");
  json_array_begin(&writer);
  json_write_cstring(&writer, "quickfix");
  json_array_end(&writer);
  json_object_end(&writer);

  json_key(&writer, "hoverProvider");
  json_write_bool(&writer, true);
  json_key(&writer, "definitionProvider");
  json_write_bool(&writer, true);
  json_key(&writer, "completionProvider");
  json_object_begin(&writer);
  /* Where a completion is worth offering before a letter is typed: an attribute's name, its value, a
   * valof() target, and what an `as` import brings. */
  json_key(&writer, "triggerCharacters");
  json_array_begin(&writer);
  json_write_cstring(&writer, "@");
  json_write_cstring(&writer, ":");
  json_write_cstring(&writer, "(");
  json_write_cstring(&writer, ".");
  json_array_end(&writer);
  json_object_end(&writer);

  json_key(&writer, "semanticTokensProvider");
  json_object_begin(&writer);
  json_key(&writer, "legend");
  json_object_begin(&writer);
  json_key(&writer, "tokenTypes");
  json_array_begin(&writer);
  for (int i = 0; i < SEM_TYPE_COUNT; ++i) {
    json_write_cstring(&writer, SEMANTIC_TYPE_NAMES[i]);
  }
  json_array_end(&writer);
  json_key(&writer, "tokenModifiers");
  json_array_begin(&writer);
  json_write_cstring(&writer, "declaration");
  json_array_end(&writer);
  json_object_end(&writer);
  json_key(&writer, "full");
  json_write_bool(&writer, true);
  json_object_end(&writer);

  json_object_end(&writer); /* capabilities */

  json_key(&writer, "serverInfo");
  json_object_begin(&writer);
  json_key(&writer, "name");
  json_write_cstring(&writer, "frame-dsl");
  json_object_end(&writer);

  json_object_end(&writer); /* result */
  json_object_end(&writer);
  write_message(server, &writer);
  json_writer_free(&writer);

  server->initialized = true;
}

static void handle_formatting(Server *server, const Json *id, const Json *params) {
  const char *uri = json_string(json_get(json_get(params, "textDocument"), "uri"));
  Document *document = uri != NULL ? find_document(server, uri) : NULL;

  JsonWriter writer;
  begin_response(&writer, id);

  DiagList diagnostics;
  diag_init(&diagnostics);
  char *formatted = document != NULL ? format_source(document->text, &diagnostics) : NULL;
  diag_free(&diagnostics);

  if (formatted == NULL || strcmp(formatted, document->text) == 0) {
    /* Nothing to do, and a file that does not parse is not guessed at: its errors are already on
     * screen as diagnostics. */
    json_array_begin(&writer);
    json_array_end(&writer);
  } else {
    LspText text;
    if (lsp_text_init(&text, document->text, strlen(document->text), server->utf16)) {
      json_array_begin(&writer);
      json_object_begin(&writer);
      json_key(&writer, "range");
      write_range(
        &writer, &text, 1, 1, (int)text.line_count, (int)(text.length - text.starts[text.line_count - 1]) + 1
      );
      json_key(&writer, "newText");
      json_write_cstring(&writer, formatted);
      json_object_end(&writer);
      json_array_end(&writer);
      lsp_text_free(&text);
    } else {
      json_array_begin(&writer);
      json_array_end(&writer);
    }
  }
  json_object_end(&writer);
  write_message(server, &writer);
  json_writer_free(&writer);
  free(formatted);
}

static void handle_semantic_tokens(Server *server, const Json *id, const Json *params) {
  const char *uri = json_string(json_get(json_get(params, "textDocument"), "uri"));
  Document *document = uri != NULL ? find_document(server, uri) : NULL;

  JsonWriter writer;
  begin_response(&writer, id);
  json_object_begin(&writer);
  json_key(&writer, "data");
  json_array_begin(&writer);

  LspText text;
  size_t count = 0;
  SemanticToken *tokens = document != NULL ? semantic_tokens(document->text, &count) : NULL;
  if (tokens != NULL && lsp_text_init(&text, document->text, strlen(document->text), server->utf16)) {
    /* The protocol takes each token relative to the one before it: line, then start, then length. */
    size_t previous_line = 0;
    size_t previous_start = 0;
    for (size_t i = 0; i < count; ++i) {
      size_t line;
      size_t start;
      lsp_text_position(&text, tokens[i].line, tokens[i].column, &line, &start);
      size_t length = lsp_text_units(&text, tokens[i].text, tokens[i].length);

      json_write_integer(&writer, (int64_t)(line - previous_line));
      json_write_integer(&writer, (int64_t)(line == previous_line ? start - previous_start : start));
      json_write_integer(&writer, (int64_t)length);
      json_write_integer(&writer, (int64_t)tokens[i].type);
      json_write_integer(&writer, tokens[i].declaration ? 1 : 0);
      previous_line = line;
      previous_start = start;
    }
    lsp_text_free(&text);
  }
  free(tokens);

  json_array_end(&writer);
  json_object_end(&writer);
  json_object_end(&writer);
  write_message(server, &writer);
  json_writer_free(&writer);
}

static char *copy_string(const Json *value) {
  const char *text = json_string(value);
  if (text == NULL) {
    return NULL;
  }
  char *copy = malloc(value->length + 1);
  if (copy != NULL) {
    memcpy(copy, text, value->length + 1);
  }
  return copy;
}

/* ---- Navigation ---------------------------------------------------------------------------------
 *
 * Each question opens the document afresh with nav_open(): a definition file is a few dozen lines, and
 * keeping a parsed copy in step with every edit would be bookkeeping with nothing to buy -- the same
 * reasoning as full-text sync. */

/* The document a request names, and the compiler's line and byte column for its position. */
static Document *position_of(Server *server, const Json *params, int *line, int *column) {
  const char *uri = json_string(json_get(json_get(params, "textDocument"), "uri"));
  Document *document = uri != NULL ? find_document(server, uri) : NULL;
  if (document == NULL) {
    return NULL;
  }
  int64_t protocol_line = 0;
  int64_t character = 0;
  const Json *position = json_get(params, "position");
  if (!json_integer(json_get(position, "line"), &protocol_line)
      || !json_integer(json_get(position, "character"), &character) || protocol_line < 0 || character < 0) {
    return NULL;
  }

  LspText text;
  if (!lsp_text_init(&text, document->text, strlen(document->text), server->utf16)) {
    return NULL;
  }
  size_t offset = lsp_text_offset(&text, (size_t)protocol_line, (size_t)character);
  size_t index = text.line_count - 1;
  while (index > 0 && text.starts[index] > offset) {
    index -= 1;
  }
  *line = (int)index + 1;
  *column = (int)(offset - text.starts[index]) + 1;
  lsp_text_free(&text);
  return document;
}

static void handle_hover(Server *server, const Json *id, const Json *params) {
  int line = 0;
  int column = 0;
  Document *document = position_of(server, params, &line, &column);
  Navigator *navigator = document != NULL ? nav_open(document->text, document->path) : NULL;
  NavLocation range;
  char *markdown = navigator != NULL ? nav_hover(navigator, line, column, &range) : NULL;

  JsonWriter writer;
  begin_response(&writer, id);
  LspText text;
  if (markdown != NULL && lsp_text_init(&text, document->text, strlen(document->text), server->utf16)) {
    json_object_begin(&writer);
    json_key(&writer, "contents");
    json_object_begin(&writer);
    json_key(&writer, "kind");
    json_write_cstring(&writer, "markdown");
    json_key(&writer, "value");
    json_write_cstring(&writer, markdown);
    json_object_end(&writer);
    json_key(&writer, "range");
    write_range(&writer, &text, range.line, range.column, range.end_line, range.end_column);
    json_object_end(&writer);
    lsp_text_free(&text);
  } else {
    json_write_null(&writer);
  }
  json_object_end(&writer);
  write_message(server, &writer);
  json_writer_free(&writer);
  free(markdown);
  nav_close(navigator);
}

/* A whole file, for positions in one the editor may not have open. NULL if it cannot be read. */
static char *slurp(const char *path) {
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    return NULL;
  }
  size_t capacity = 4096;
  size_t used = 0;
  char *data = malloc(capacity);
  while (data != NULL) {
    used += fread(data + used, 1, capacity - used - 1, handle);
    if (used + 1 < capacity) {
      break;
    }
    capacity *= 2;
    char *grown = realloc(data, capacity);
    if (grown == NULL) {
      free(data);
      data = NULL;
    }
    data = grown;
  }
  fclose(handle);
  if (data != NULL) {
    data[used] = '\0';
  }
  return data;
}

static void handle_definition(Server *server, const Json *id, const Json *params) {
  int line = 0;
  int column = 0;
  Document *document = position_of(server, params, &line, &column);
  Navigator *navigator = document != NULL ? nav_open(document->text, document->path) : NULL;
  NavLocation where = { 0 };
  bool found = navigator != NULL && nav_definition(navigator, line, column, &where);

  /* Positions are converted with the text they are in: an imported file's own, read from disk. */
  char *other = found && where.file != NULL ? slurp(where.file) : NULL;
  const char *in = where.file != NULL ? other : document != NULL ? document->text : NULL;
  char *uri = !found               ? NULL
              : where.file != NULL ? uri_of_path(where.file)
                                   : copy_string(json_get(json_get(params, "textDocument"), "uri"));

  JsonWriter writer;
  begin_response(&writer, id);
  LspText text;
  if (found && in != NULL && uri != NULL && lsp_text_init(&text, in, strlen(in), server->utf16)) {
    json_object_begin(&writer);
    json_key(&writer, "uri");
    json_write_cstring(&writer, uri);
    json_key(&writer, "range");
    write_range(&writer, &text, where.line, where.column, where.end_line, where.end_column);
    json_object_end(&writer);
    lsp_text_free(&text);
  } else {
    json_write_null(&writer);
  }
  json_object_end(&writer);
  write_message(server, &writer);
  json_writer_free(&writer);
  free(uri);
  free(other);
  free(where.file);
  nav_close(navigator);
}

static void handle_completion(Server *server, const Json *id, const Json *params) {
  int line = 0;
  int column = 0;
  Document *document = position_of(server, params, &line, &column);
  Navigator *navigator = document != NULL ? nav_open(document->text, document->path) : NULL;
  size_t count = 0;
  NavCompletion *items = navigator != NULL ? nav_complete(navigator, line, column, &count) : NULL;

  /* CompletionItemKind, as the protocol numbers them. */
  static const int KINDS[] = {
    [NAV_KEYWORD] = 14,   [NAV_BLOCK] = 22, [NAV_CAL] = 3,       [NAV_FIELD] = 5,
    [NAV_ATTRIBUTE] = 10, [NAV_VALUE] = 20, [NAV_NAMESPACE] = 9, [NAV_ENUM] = 13,
  };

  JsonWriter writer;
  begin_response(&writer, id);
  json_array_begin(&writer);
  for (size_t i = 0; i < count; ++i) {
    json_object_begin(&writer);
    json_key(&writer, "label");
    json_write_cstring(&writer, items[i].label);
    json_key(&writer, "kind");
    json_write_integer(&writer, KINDS[items[i].kind]);
    if (items[i].detail != NULL) {
      json_key(&writer, "detail");
      json_write_cstring(&writer, items[i].detail);
    }
    json_object_end(&writer);
  }
  json_array_end(&writer);
  json_object_end(&writer);
  write_message(server, &writer);
  json_writer_free(&writer);
  nav_completions_free(items, count);
  nav_close(navigator);
}

/* ---- Quick fixes -----------------------------------------------------------------------------------
 *
 * The loader makes every fix -- the check that finds a problem is what knows the likely repair -- and
 * attaches it to the diagnostic. All that is left here is to say which fixes the editor's range
 * touches, as edits. */

/* A byte offset for the compiler's 1-based line and column, clamped to the text. */
static size_t offset_of_position(const LspText *text, int line, int column) {
  if (line < 1) {
    return 0;
  }
  size_t index = (size_t)line - 1 < text->line_count ? (size_t)line - 1 : text->line_count - 1;
  size_t offset = text->starts[index] + (size_t)(column > 0 ? column - 1 : 0);
  return offset < text->length ? offset : text->length;
}

static void handle_code_action(Server *server, const Json *id, const Json *params) {
  const char *uri = json_string(json_get(json_get(params, "textDocument"), "uri"));
  Document *document = uri != NULL ? find_document(server, uri) : NULL;

  JsonWriter writer;
  begin_response(&writer, id);
  json_array_begin(&writer);

  LspText text;
  if (document != NULL && lsp_text_init(&text, document->text, strlen(document->text), server->utf16)) {
    const Json *range = json_get(params, "range");
    int64_t values[4] = { 0, 0, 0, 0 };
    json_integer(json_get(json_get(range, "start"), "line"), &values[0]);
    json_integer(json_get(json_get(range, "start"), "character"), &values[1]);
    json_integer(json_get(json_get(range, "end"), "line"), &values[2]);
    json_integer(json_get(json_get(range, "end"), "character"), &values[3]);
    for (int i = 0; i < 4; ++i) {
      values[i] = values[i] < 0 ? 0 : values[i];
    }
    size_t from = lsp_text_offset(&text, (size_t)values[0], (size_t)values[1]);
    size_t to = lsp_text_offset(&text, (size_t)values[2], (size_t)values[3]);

    FramedslResult *result = document->path != NULL ? framedsl_load_source(document->text, document->path)
                                                    : framedsl_load_string(document->text, document->uri);
    for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
      const FramedslDiagnostic *diagnostic = framedsl_diagnostic(result, i);
      if (diagnostic->fix_title == NULL || diagnostic->file != NULL) {
        continue;
      }
      size_t start = offset_of_position(&text, diagnostic->line, diagnostic->column);
      size_t end = offset_of_position(
        &text,
        diagnostic->end_line > 0 ? diagnostic->end_line : diagnostic->line,
        diagnostic->end_line > 0 ? diagnostic->end_column : diagnostic->column
      );
      if (start > to || from > end) {
        continue;
      }

      json_object_begin(&writer);
      json_key(&writer, "title");
      json_write_cstring(&writer, diagnostic->fix_title);
      json_key(&writer, "kind");
      json_write_cstring(&writer, "quickfix");
      json_key(&writer, "diagnostics");
      json_array_begin(&writer);
      write_diagnostic(&writer, document, &text, diagnostic);
      json_array_end(&writer);
      /* An error's fix is the one to reach for; a warning's is a tidy-up. */
      json_key(&writer, "isPreferred");
      json_write_bool(&writer, diagnostic->severity == FRAMEDSL_SEVERITY_ERROR);
      json_key(&writer, "edit");
      json_object_begin(&writer);
      json_key(&writer, "changes");
      json_object_begin(&writer);
      json_key(&writer, document->uri);
      json_array_begin(&writer);
      json_object_begin(&writer);
      json_key(&writer, "range");
      write_range(
        &writer,
        &text,
        diagnostic->fix_line,
        diagnostic->fix_column,
        diagnostic->fix_end_line,
        diagnostic->fix_end_column
      );
      json_key(&writer, "newText");
      json_write_cstring(&writer, diagnostic->fix_text);
      json_object_end(&writer);
      json_array_end(&writer);
      json_object_end(&writer); /* changes */
      json_object_end(&writer); /* edit */
      json_object_end(&writer);
    }
    framedsl_result_free(result);
    lsp_text_free(&text);
  }

  json_array_end(&writer);
  json_object_end(&writer);
  write_message(server, &writer);
  json_writer_free(&writer);
}

/* ---- Notifications --------------------------------------------------------------------------------- */


static void handle_did_open(Server *server, const Json *params) {
  const Json *item = json_get(params, "textDocument");
  const char *uri = json_string(json_get(item, "uri"));
  char *text = copy_string(json_get(item, "text"));
  int64_t version = 0;
  json_integer(json_get(item, "version"), &version);
  if (uri == NULL || text == NULL) {
    free(text);
    return;
  }
  Document *document = open_document(server, uri, text, version);
  if (document != NULL) {
    publish_diagnostics(server, document);
  }
}

static void handle_did_change(Server *server, const Json *params) {
  const Json *item = json_get(params, "textDocument");
  const char *uri = json_string(json_get(item, "uri"));
  const Json *changes = json_get(params, "contentChanges");
  if (uri == NULL || changes == NULL || changes->kind != JSON_ARRAY || changes->count == 0) {
    return;
  }
  Document *document = find_document(server, uri);
  if (document == NULL) {
    return;
  }
  int64_t version = document->version;
  json_integer(json_get(item, "version"), &version);

  for (size_t i = 0; i < changes->count; ++i) {
    const Json *change = changes->items[i];
    const Json *range = json_get(change, "range");
    char *text = copy_string(json_get(change, "text"));
    if (text == NULL) {
      return;
    }
    if (range == NULL) {
      free(document->text);
      document->text = text;
      continue;
    }

    /* Full sync is what this server asks for, so a range is an editor being generous rather than
     * following the capability; splicing it in is a few lines and beats ignoring the edit. */
    LspText old;
    if (!lsp_text_init(&old, document->text, strlen(document->text), server->utf16)) {
      free(text);
      return;
    }
    int64_t start_line = 0;
    int64_t start_character = 0;
    int64_t end_line = 0;
    int64_t end_character = 0;
    json_integer(json_get(json_get(range, "start"), "line"), &start_line);
    json_integer(json_get(json_get(range, "start"), "character"), &start_character);
    json_integer(json_get(json_get(range, "end"), "line"), &end_line);
    json_integer(json_get(json_get(range, "end"), "character"), &end_character);
    size_t from = lsp_text_offset(
      &old, (size_t)(start_line < 0 ? 0 : start_line), (size_t)(start_character < 0 ? 0 : start_character)
    );
    size_t to =
      lsp_text_offset(&old, (size_t)(end_line < 0 ? 0 : end_line), (size_t)(end_character < 0 ? 0 : end_character));
    if (to < from) {
      to = from;
    }

    size_t inserted = strlen(text);
    char *spliced = malloc(old.length - (to - from) + inserted + 1);
    if (spliced != NULL) {
      memcpy(spliced, document->text, from);
      memcpy(spliced + from, text, inserted);
      memcpy(spliced + from + inserted, document->text + to, old.length - to + 1);
      free(document->text);
      document->text = spliced;
    }
    lsp_text_free(&old);
    free(text);
  }

  document->version = version;
  publish_diagnostics(server, document);
}

static void handle_did_close(Server *server, const Json *params) {
  const char *uri = json_string(json_get(json_get(params, "textDocument"), "uri"));
  if (uri == NULL) {
    return;
  }
  /* An empty list, so what the editor is showing goes away with the buffer. */
  Document *document = find_document(server, uri);
  if (document != NULL) {
    free(document->text);
    document->text = malloc(1);
    if (document->text != NULL) {
      document->text[0] = '\0';
      publish_diagnostics(server, document);
    }
  }
  close_document(server, uri);
}

/* ---- The loop --------------------------------------------------------------------------------------- */

static bool handle(Server *server, const Json *message, int *exit_code) {
  const char *method = json_string(json_get(message, "method"));
  const Json *id = json_get(message, "id");
  const Json *params = json_get(message, "params");
  if (method == NULL) {
    return true; /* a response to something this server asked; it asks nothing */
  }

  if (strcmp(method, "exit") == 0) {
    *exit_code = server->shutdown_requested ? 0 : 1;
    return false;
  }
  if (strcmp(method, "initialize") == 0) {
    if (server->initialized) {
      send_error(server, id, -32600, "already initialized");
    } else {
      handle_initialize(server, id, params);
    }
    return true;
  }
  if (!server->initialized) {
    /* Nothing but initialize is answered before initialize, and a notification is dropped in silence,
     * both as the protocol says. */
    if (id != NULL) {
      send_error(server, id, -32002, "the server has not been initialized");
    }
    return true;
  }

  if (strcmp(method, "shutdown") == 0) {
    server->shutdown_requested = true;
    JsonWriter writer;
    begin_response(&writer, id);
    json_write_null(&writer);
    json_object_end(&writer);
    write_message(server, &writer);
    json_writer_free(&writer);
    return true;
  }
  if (strcmp(method, "textDocument/formatting") == 0) {
    handle_formatting(server, id, params);
    return true;
  }
  if (strcmp(method, "textDocument/codeAction") == 0) {
    handle_code_action(server, id, params);
    return true;
  }
  if (strcmp(method, "textDocument/hover") == 0) {
    handle_hover(server, id, params);
    return true;
  }
  if (strcmp(method, "textDocument/definition") == 0) {
    handle_definition(server, id, params);
    return true;
  }
  if (strcmp(method, "textDocument/completion") == 0) {
    handle_completion(server, id, params);
    return true;
  }
  if (strcmp(method, "textDocument/semanticTokens/full") == 0) {
    handle_semantic_tokens(server, id, params);
    return true;
  }
  if (strcmp(method, "textDocument/didOpen") == 0) {
    handle_did_open(server, params);
    return true;
  }
  if (strcmp(method, "textDocument/didChange") == 0) {
    handle_did_change(server, params);
    return true;
  }
  if (strcmp(method, "textDocument/didClose") == 0) {
    handle_did_close(server, params);
    return true;
  }
  if (strcmp(method, "textDocument/didSave") == 0) {
    publish_all(server);
    return true;
  }

  /* A request must be answered even when the answer is "no such method"; a notification must not be. */
  if (id != NULL) {
    send_error(server, id, -32601, "method not found");
  }
  return true;
}

int lsp_serve(FILE *in, FILE *out) {
  Server server = { .in = in, .out = out, .utf16 = true };
  int exit_code = 1;

  for (;;) {
    size_t length = 0;
    char *body = read_message(in, &length);
    if (body == NULL) {
      break; /* the editor is gone, or sent something that is not a message */
    }

    Region *region = region_new();
    Json *message = region != NULL ? json_parse(region, body, length) : NULL;
    if (message == NULL) {
      /* -32700, and no id to answer to: the protocol's own answer for text that is not JSON. */
      send_error(&server, NULL, -32700, "parse error");
    } else if (!handle(&server, message, &exit_code)) {
      region_free(region);
      free(body);
      break;
    }
    region_free(region);
    free(body);
  }

  for (size_t i = 0; i < server.count; ++i) {
    free(server.documents[i].uri);
    free(server.documents[i].path);
    free(server.documents[i].text);
  }
  free(server.documents);
  return exit_code;
}
