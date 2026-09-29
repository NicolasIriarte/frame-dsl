/* Story 13: the language server (#30), one scripted session per case.
 *
 * A session is written as the messages an editor would send, run through lsp_serve() over a pair of
 * in-memory streams, and read back as the messages it wrote. What is asserted is what an editor would
 * act on: the framing, the ids, the ranges, the severities and the edits -- not the bytes between
 * them, which the JSON writer's own story covers. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "lsp.h"
#include "lsp_semantic.h"
#include "testsuite.h"

/* ---- Running a session -------------------------------------------------------------------------- */

typedef struct {
  char *output;
  size_t length;
  int exit_code;
  Region *region;
  /* The messages the server wrote, parsed. */
  Json *messages[512];
  size_t count;
} Session;

static char *framed(const char *const *messages, size_t count, size_t *length) {
  size_t capacity = 4096;
  char *input = malloc(capacity);
  size_t used = 0;
  for (size_t i = 0; i < count && input != NULL; ++i) {
    size_t body = strlen(messages[i]);
    while (used + body + 64 > capacity) {
      capacity *= 2;
      char *grown = realloc(input, capacity);
      if (grown == NULL) {
        free(input);
        return NULL;
      }
      input = grown;
    }
    used += (size_t)snprintf(input + used, capacity - used, "Content-Length: %zu\r\n\r\n", body);
    memcpy(input + used, messages[i], body);
    used += body;
  }
  *length = used;
  return input;
}

/* Splits what the server wrote back into messages, checking the framing as it goes. */
static bool split(Session *session) {
  size_t at = 0;
  while (at < session->length) {
    const char *header = session->output + at;
    const char *blank = strstr(header, "\r\n\r\n");
    if (blank == NULL || strncmp(header, "Content-Length: ", 16) != 0) {
      return false;
    }
    size_t body = (size_t)atol(header + 16);
    size_t start = (size_t)(blank - session->output) + 4;
    if (start + body > session->length || session->count >= 512) {
      return false;
    }
    Json *message = json_parse(session->region, session->output + start, body);
    if (message == NULL) {
      return false; /* everything written must be JSON, whatever the input was */
    }
    session->messages[session->count++] = message;
    at = start + body;
  }
  return true;
}

static bool run(Session *session, const char *const *messages, size_t count) {
  memset(session, 0, sizeof(*session));
  session->region = region_new();

  size_t length = 0;
  char *input = framed(messages, count, &length);
  FILE *in = input != NULL ? fmemopen(input, length, "r") : NULL;
  FILE *out = open_memstream(&session->output, &session->length);
  if (in == NULL || out == NULL) {
    free(input);
    return false;
  }
  session->exit_code = lsp_serve(in, out);
  fclose(in);
  fclose(out);
  free(input);
  return split(session);
}

static void session_free(Session *session) {
  region_free(session->region);
  free(session->output);
}

/* The last publishDiagnostics for any document, which is what the editor would be showing. */
static const Json *last_diagnostics(const Session *session) {
  const Json *found = NULL;
  for (size_t i = 0; i < session->count; ++i) {
    const char *name = json_string(json_get(session->messages[i], "method"));
    if (name != NULL && strcmp(name, "textDocument/publishDiagnostics") == 0) {
      found = json_get(session->messages[i], "params");
    }
  }
  return found;
}

static const Json *response_to(const Session *session, int64_t id) {
  for (size_t i = 0; i < session->count; ++i) {
    int64_t found = 0;
    if (json_integer(json_get(session->messages[i], "id"), &found) && found == id) {
      return session->messages[i];
    }
  }
  return NULL;
}

static int64_t integer_at(const Json *value, const char *key) {
  int64_t out = -1;
  json_integer(json_get(value, key), &out);
  return out;
}

static const char *const INITIALIZE = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";
static const char *const INITIALIZE_UTF8 =
  "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"general\":{\"positionEncodings\":[\"utf-8\","
  "\"utf-16\"]}}}";
static const char *const EXIT = "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}";

/* didOpen for `text`, which must already be escaped for JSON. */
#define DID_OPEN(text)                                                                                                 \
  "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/"      \
  "t.fdsl\",\"languageId\":\"fdsl\",\"version\":1,\"text\":\"" text "\"}}}"

#define DID_CHANGE(text)                                                                                               \
  "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/"    \
  "t.fdsl\",\"version\":2},\"contentChanges\":[{\"text\":\"" text "\"}]}}"

#define GOOD "pkt TM {\\n  id: 8 = 1;\\n};\\n"
#define BROKEN "pkt TM {\\n  id: 8\\n};\\n"

/* ---- The cases ------------------------------------------------------------------------------------ */

static int us13_tc01_initialize_says_what_it_can_do(const TestArguments *args) {
  (void)args;
  const char *const messages[] = { INITIALIZE, EXIT };
  Session session;
  if (!run(&session, messages, 2)) {
    return __LINE__;
  }
  const Json *capabilities = json_get(json_get(response_to(&session, 1), "result"), "capabilities");
  const Json *legend = json_get(json_get(capabilities, "semanticTokensProvider"), "legend");
  const Json *types = json_get(legend, "tokenTypes");

  int failure = 0;
  const char *encoding = json_string(json_get(capabilities, "positionEncoding"));
  if (encoding == NULL || strcmp(encoding, "utf-16") != 0) {
    failure = __LINE__; /* the protocol's default, when the editor offers nothing else */
  } else if (json_get(capabilities, "documentFormattingProvider") == NULL
             || json_get(capabilities, "textDocumentSync") == NULL) {
    failure = __LINE__;
  } else if (types == NULL || types->kind != JSON_ARRAY || types->count != SEM_TYPE_COUNT) {
    failure = __LINE__;
  } else if (strcmp(json_string(types->items[0]), SEMANTIC_TYPE_NAMES[0]) != 0) {
    failure = __LINE__;
  }
  session_free(&session);
  return failure;
}

static int us13_tc02_utf8_positions_when_the_editor_takes_them(const TestArguments *args) {
  (void)args;
  const char *const messages[] = { INITIALIZE_UTF8, EXIT };
  Session session;
  if (!run(&session, messages, 2)) {
    return __LINE__;
  }
  const Json *capabilities = json_get(json_get(response_to(&session, 1), "result"), "capabilities");
  const char *encoding = json_string(json_get(capabilities, "positionEncoding"));
  int failure = encoding != NULL && strcmp(encoding, "utf-8") == 0 ? 0 : __LINE__;
  session_free(&session);
  return failure;
}

static int us13_tc03_what_it_says_no_to(const TestArguments *args) {
  (void)args;
  const char *const messages[] = {
    /* Before initialize: a request is refused and a notification is dropped in silence. */
    "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"textDocument/formatting\",\"params\":{}}",
    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\",\"params\":{}}",
    INITIALIZE,
    "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"textDocument/nonsense\",\"params\":{}}",
    "{\"jsonrpc\":\"2.0\",\"method\":\"$/somethingElse\",\"params\":{}}",
    "not json at all",
    EXIT,
  };
  Session session;
  if (!run(&session, messages, 7)) {
    return __LINE__;
  }
  int failure = 0;
  if (integer_at(json_get(response_to(&session, 7), "error"), "code") != -32002) {
    failure = __LINE__;
  } else if (integer_at(json_get(response_to(&session, 8), "error"), "code") != -32601) {
    failure = __LINE__;
  } else {
    /* The parse error is answered with no id, and nothing answers the two notifications. */
    bool parse_error = false;
    size_t answers = 0;
    for (size_t i = 0; i < session.count; ++i) {
      const Json *error = json_get(session.messages[i], "error");
      if (error != NULL) {
        answers += 1;
        parse_error |= integer_at(error, "code") == -32700;
      }
    }
    if (!parse_error || answers != 3) {
      failure = __LINE__;
    }
  }
  session_free(&session);
  return failure;
}

static int us13_tc04_an_open_document_is_diagnosed(const TestArguments *args) {
  (void)args;
  const char *const messages[] = { INITIALIZE, DID_OPEN(BROKEN), DID_CHANGE(GOOD), EXIT };
  Session session;
  if (!run(&session, messages, 4)) {
    return __LINE__;
  }

  int failure = 0;
  size_t published = 0;
  const Json *first = NULL;
  for (size_t i = 0; i < session.count; ++i) {
    const char *method = json_string(json_get(session.messages[i], "method"));
    if (method != NULL && strcmp(method, "textDocument/publishDiagnostics") == 0) {
      published += 1;
      if (first == NULL) {
        first = json_get(session.messages[i], "params");
      }
    }
  }

  const Json *diagnostics = json_get(first, "diagnostics");
  if (published != 2 || diagnostics == NULL || diagnostics->count != 1) {
    failure = __LINE__;
  } else {
    const Json *one = diagnostics->items[0];
    const Json *start = json_get(json_get(one, "range"), "start");
    const char *message = json_string(json_get(one, "message"));
    if (integer_at(start, "line") != 2 || integer_at(start, "character") != 0) {
      failure = __LINE__; /* `};` on the third line, counted from zero */
    } else if (integer_at(one, "severity") != 1 || message == NULL || strstr(message, "expected ';'") == NULL) {
      failure = __LINE__;
    } else if (strcmp(json_string(json_get(one, "source")), "frame-dsl") != 0) {
      failure = __LINE__;
    }
  }

  /* Fixed, and the editor is told to clear what it was showing. */
  const Json *last = json_get(last_diagnostics(&session), "diagnostics");
  if (failure == 0 && (last == NULL || last->count != 0)) {
    failure = __LINE__;
  }
  session_free(&session);
  return failure;
}

static int us13_tc05_a_warning_is_a_warning(const TestArguments *args) {
  (void)args;
  const char *const messages[] = { INITIALIZE,
                                   DID_OPEN("block Spare {\\n  s: 8;\\n};\\npkt TM {\\n  id: 8 = 1;\\n};\\n"),
                                   EXIT };
  Session session;
  if (!run(&session, messages, 3)) {
    return __LINE__;
  }
  const Json *diagnostics = json_get(last_diagnostics(&session), "diagnostics");
  int failure = 0;
  if (diagnostics == NULL || diagnostics->count != 1) {
    failure = __LINE__;
  } else if (integer_at(diagnostics->items[0], "severity") != 2 || integer_at(diagnostics->items[0], "code") != 30) {
    failure = __LINE__;
  }
  session_free(&session);
  return failure;
}

static int us13_tc06_formatting_is_one_edit(const TestArguments *args) {
  (void)args;
  const char *const messages[] = {
    INITIALIZE,
    DID_OPEN("pkt TM {\\n\\tid:8=1;\\n};\\n"),
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/formatting\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"},\"options\":{\"tabSize\":2,\"insertSpaces\":true}}}",
    DID_CHANGE(GOOD),
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/formatting\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"}}}",
    DID_CHANGE(BROKEN),
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/formatting\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"}}}",
    EXIT,
  };
  Session session;
  if (!run(&session, messages, 8)) {
    return __LINE__;
  }

  int failure = 0;
  const Json *edits = json_get(response_to(&session, 2), "result");
  if (edits == NULL || edits->kind != JSON_ARRAY || edits->count != 1) {
    failure = __LINE__;
  } else {
    const Json *edit = edits->items[0];
    const char *text = json_string(json_get(edit, "newText"));
    const Json *start = json_get(json_get(edit, "range"), "start");
    if (text == NULL || strcmp(text, "pkt TM {\n  id: 8 = 1;\n};\n") != 0) {
      failure = __LINE__;
    } else if (integer_at(start, "line") != 0 || integer_at(start, "character") != 0) {
      failure = __LINE__;
    } else if (integer_at(json_get(json_get(edit, "range"), "end"), "line") != 3) {
      failure = __LINE__; /* to the end of the document, which is the start of the line past the last */
    }
  }

  /* Already formatted, and one that does not parse: nothing to do in either case. */
  const Json *none = json_get(response_to(&session, 3), "result");
  const Json *broken = json_get(response_to(&session, 4), "result");
  if (failure == 0 && (none == NULL || none->count != 0 || broken == NULL || broken->count != 0)) {
    failure = __LINE__;
  }
  session_free(&session);
  return failure;
}

static int us13_tc07_semantic_tokens_say_what_each_name_is(const TestArguments *args) {
  (void)args;
  const char *const messages[] = {
    INITIALIZE,
    DID_OPEN("pkt TM {\\n  id: 8 = 1;\\n};\\n"),
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/semanticTokens/full\",\"params\":{\"textDocument\":"
    "{\"uri\":\"file:///tmp/t.fdsl\"}}}",
    EXIT,
  };
  Session session;
  if (!run(&session, messages, 4)) {
    return __LINE__;
  }
  const Json *data = json_get(json_get(response_to(&session, 2), "result"), "data");
  /* `pkt` `TM` `id` `8` `=` `1` -- six tokens of five numbers each. */
  const int64_t expected[] = {
    0, 0, 3, SEM_KEYWORD,  0, /* pkt */
    0, 4, 2, SEM_TYPE,     1, /* TM, declared here */
    1, 2, 2, SEM_PROPERTY, 1, /* id, declared here */
    0, 4, 1, SEM_NUMBER,   0, /* 8 */
    0, 2, 1, SEM_OPERATOR, 0, /* = */
    0, 2, 1, SEM_NUMBER,   0, /* 1 */
  };
  int failure = 0;
  if (data == NULL || data->count != sizeof(expected) / sizeof(*expected)) {
    failure = __LINE__;
  } else {
    for (size_t i = 0; i < data->count && failure == 0; ++i) {
      int64_t value = -1;
      if (!json_integer(data->items[i], &value) || value != expected[i]) {
        failure = __LINE__ * 100 + (int)i;
      }
    }
  }
  session_free(&session);
  return failure;
}

static int us13_tc08_a_document_closed_is_forgotten(const TestArguments *args) {
  (void)args;
  const char *const messages[] = {
    INITIALIZE,
    DID_OPEN(BROKEN),
    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/"
    "t.fdsl\"}}}",
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/semanticTokens/full\",\"params\":{\"textDocument\":"
    "{\"uri\":\"file:///tmp/t.fdsl\"}}}",
    EXIT,
  };
  Session session;
  if (!run(&session, messages, 5)) {
    return __LINE__;
  }
  int failure = 0;
  const Json *cleared = json_get(last_diagnostics(&session), "diagnostics");
  const Json *data = json_get(json_get(response_to(&session, 2), "result"), "data");
  if (cleared == NULL || cleared->count != 0) {
    failure = __LINE__; /* what the editor was showing must go away with the buffer */
  } else if (data == NULL || data->count != 0) {
    failure = __LINE__; /* and a question about it is answered with nothing, not a crash */
  }
  session_free(&session);
  return failure;
}

static int us13_tc09_a_range_change_is_spliced_in(const TestArguments *args) {
  (void)args;
  /* This server asks for whole documents, but an editor sending a range is taken at its word. */
  const char *const messages[] = {
    INITIALIZE,
    DID_OPEN(GOOD),
    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/"
    "t.fdsl\",\"version\":2},\"contentChanges\":[{\"range\":{\"start\":{\"line\":1,\"character\":11},\"end\":{\"line\":"
    "1,"
    "\"character\":12}},\"text\":\"\"}]}}",
    EXIT,
  };
  Session session;
  if (!run(&session, messages, 4)) {
    return __LINE__;
  }
  /* The `;` of `id: 8 = 1;` removed, so the next line is where the error is. */
  const Json *diagnostics = json_get(last_diagnostics(&session), "diagnostics");
  int failure = 0;
  if (diagnostics == NULL || diagnostics->count != 1) {
    failure = __LINE__;
  } else if (integer_at(json_get(json_get(diagnostics->items[0], "range"), "start"), "line") != 2) {
    failure = __LINE__;
  }
  session_free(&session);
  return failure;
}

static int us13_tc10_shutdown_then_exit_is_a_clean_exit(const TestArguments *args) {
  (void)args;
  const char *const clean[] = { INITIALIZE, "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\"}", EXIT };
  const char *const abrupt[] = { INITIALIZE, EXIT };
  Session session;
  if (!run(&session, clean, 3)) {
    return __LINE__;
  }
  int failure = 0;
  const Json *response = response_to(&session, 9);
  if (session.exit_code != 0) {
    failure = __LINE__;
  } else if (response == NULL || json_get(response, "result") == NULL
             || json_get(response, "result")->kind != JSON_NULL) {
    failure = __LINE__;
  }
  session_free(&session);
  if (failure != 0) {
    return failure;
  }

  if (!run(&session, abrupt, 2)) {
    return __LINE__;
  }
  failure = session.exit_code == 1 ? 0 : __LINE__;
  session_free(&session);
  return failure;
}

/* Typing a file out, one character at a time, asking for tokens at every step: the states a server
 * actually meets are the half-written ones. Nothing may crash, and everything written must be JSON --
 * which split() checks for every message of every session in this story. */
static int us13_tc11_every_state_of_typing_is_survivable(const TestArguments *args) {
  (void)args;
  const char *typed =
    "import \\\"x.fdsl\\\" as n;\\n@byte_order: little_endian;\\ncal f(x) = x * 2;\\nblock H {\\n  "
    "id: 8 = 1;\\n};\\npkt TM {\\n  h: H { id = 2; };\\n  [ @cal: f; ]\\n  v: 16;\\n};\\n";
  size_t length = strlen(typed);

  const char *messages[512];
  char *buffers[512];
  size_t count = 0;
  messages[count++] = INITIALIZE;
  messages[count++] = DID_OPEN("");

  size_t used = 0;
  for (size_t prefix = 1; prefix <= length && count < 500; ++prefix) {
    /* Never cut an escape in half: the prefix has to stay a JSON string. */
    if (typed[prefix - 1] == '\\' || (prefix >= 2 && typed[prefix - 2] == '\\')) {
      continue;
    }
    char *change = malloc(prefix + 256);
    char *tokens = malloc(160);
    if (change == NULL || tokens == NULL) {
      free(change);
      free(tokens);
      break;
    }
    snprintf(
      change,
      prefix + 256,
      "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/"
      "t.fdsl\",\"version\":2},\"contentChanges\":[{\"text\":\"%.*s\"}]}}",
      (int)prefix,
      typed
    );
    snprintf(
      tokens,
      160,
      "{\"jsonrpc\":\"2.0\",\"id\":%zu,\"method\":\"textDocument/semanticTokens/full\",\"params\":{\"textDocument\":"
      "{\"uri\":\"file:///tmp/t.fdsl\"}}}",
      prefix + 100
    );
    buffers[used++] = change;
    buffers[used++] = tokens;
    messages[count++] = change;
    messages[count++] = tokens;
  }
  messages[count++] = EXIT;

  Session session;
  bool ok = run(&session, messages, count);
  int failure = ok ? 0 : __LINE__;
  if (ok && session.count < count / 2) {
    failure = __LINE__; /* every request answered */
  }
  if (ok) {
    session_free(&session);
  }
  for (size_t i = 0; i < used; ++i) {
    free(buffers[i]);
  }
  return failure;
}

static int us13_tc12_a_non_ascii_line_is_measured_the_editors_way(const TestArguments *args) {
  (void)args;
  /* The comment holds "é", two bytes and one UTF-16 unit, so the two encodings disagree about where
   * the error after it is -- which is the whole reason the encoding is negotiated. */
  const char *const document = DID_OPEN("pkt TM {  # caf\\u00e9\\n  id: 8\\n};\\n");
  const char *const utf16[] = { INITIALIZE, document, EXIT };
  const char *const utf8[] = { INITIALIZE_UTF8, document, EXIT };

  Session session;
  if (!run(&session, utf16, 3)) {
    return __LINE__;
  }
  const Json *diagnostics = json_get(last_diagnostics(&session), "diagnostics");
  int failure = 0;
  if (diagnostics == NULL || diagnostics->count != 1) {
    failure = __LINE__;
  }
  session_free(&session);
  if (failure != 0) {
    return failure;
  }

  if (!run(&session, utf8, 3)) {
    return __LINE__;
  }
  diagnostics = json_get(last_diagnostics(&session), "diagnostics");
  if (diagnostics == NULL || diagnostics->count != 1) {
    failure = __LINE__;
  }
  session_free(&session);
  return failure;
}

/* #72 through the protocol: announced, and each request answered in the shape an editor reads. The
 * answers themselves are story 15's. */
static int us13_tc13_hover_definition_and_completion_are_answered(const TestArguments *args) {
  (void)args;
  const char *const messages[] = {
    INITIALIZE,
    DID_OPEN("block H {\\n  id: 8;\\n};\\npkt TM {\\n  h:   H { id = 1; };\\n  len: 8;\\n  [ @\\n"),
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"},\"position\":{\"line\":4,\"character\":2}}}",
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"},\"position\":{\"line\":4,\"character\":7}}}",
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"},\"position\":{\"line\":6,\"character\":5}}}",
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"},\"position\":{\"line\":0,\"character\":8}}}",
    EXIT,
  };
  Session session;
  if (!run(&session, messages, 7)) {
    return __LINE__;
  }

  int failure = 0;
  const Json *capabilities = json_get(json_get(response_to(&session, 1), "result"), "capabilities");
  const Json *hover = json_get(response_to(&session, 2), "result");
  const Json *definition = json_get(response_to(&session, 3), "result");
  const Json *completion = json_get(response_to(&session, 4), "result");
  const Json *nothing = response_to(&session, 5);
  const char *markdown = json_string(json_get(json_get(hover, "contents"), "value"));
  const char *uri = json_string(json_get(definition, "uri"));

  if (json_get(capabilities, "hoverProvider") == NULL || json_get(capabilities, "definitionProvider") == NULL
      || json_get(json_get(capabilities, "completionProvider"), "triggerCharacters") == NULL) {
    failure = __LINE__;
  } else if (markdown == NULL || strstr(markdown, "instance of block `H`") == NULL
             || strcmp(json_string(json_get(json_get(hover, "contents"), "kind")), "markdown") != 0) {
    failure = __LINE__;
  } else if (uri == NULL || strcmp(uri, "file:///tmp/t.fdsl") != 0
             || integer_at(json_get(json_get(definition, "range"), "start"), "line") != 0
             || integer_at(json_get(json_get(definition, "range"), "start"), "character") != 6) {
    failure = __LINE__; /* `H` in the use, onto `block H` */
  } else if (completion == NULL || completion->kind != JSON_ARRAY || completion->count != 3
             || integer_at(completion->items[0], "kind") != 10) {
    failure = __LINE__; /* the three attribute names, as properties */
  } else if (nothing == NULL || json_get(nothing, "result") == NULL || json_get(nothing, "result")->kind != JSON_NULL) {
    failure = __LINE__; /* a question about `{` is answered with null, not an error */
  }
  session_free(&session);
  return failure;
}

/* #73 through the protocol: a fix the loader attached comes back as a quick fix whose edit an editor
 * applies as it is. Which fixes there are is system story 18's. */
static int us13_tc14_a_fix_is_offered_as_a_code_action(const TestArguments *args) {
  (void)args;
  const char *const messages[] = {
    INITIALIZE,
    DID_OPEN("@byte_ordre: big_endian;\\npkt TM {\\n  id: 8 = 1;\\n};\\n"),
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/codeAction\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"},\"range\":{\"start\":{\"line\":0,\"character\":3},\"end\":{\"line\":0,\"character\":3}},"
    "\"context\":{\"diagnostics\":[]}}}",
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/codeAction\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"},\"range\":{\"start\":{\"line\":2,\"character\":0},\"end\":{\"line\":2,\"character\":0}},"
    "\"context\":{\"diagnostics\":[]}}}",
    EXIT,
  };
  Session session;
  if (!run(&session, messages, 5)) {
    return __LINE__;
  }

  int failure = 0;
  const Json *capabilities = json_get(json_get(response_to(&session, 1), "result"), "capabilities");
  const Json *actions = json_get(response_to(&session, 2), "result");
  const Json *elsewhere = json_get(response_to(&session, 3), "result");
  const Json *action = actions != NULL && actions->kind == JSON_ARRAY && actions->count == 1 ? actions->items[0] : NULL;
  const Json *edits = json_get(json_get(json_get(action, "edit"), "changes"), "file:///tmp/t.fdsl");
  const Json *edit = edits != NULL && edits->kind == JSON_ARRAY && edits->count == 1 ? edits->items[0] : NULL;
  const Json *range = json_get(edit, "range");

  if (json_get(capabilities, "codeActionProvider") == NULL) {
    failure = __LINE__;
  } else if (action == NULL || strcmp(json_string(json_get(action, "title")), "did you mean 'byte_order'?") != 0
             || strcmp(json_string(json_get(action, "kind")), "quickfix") != 0) {
    failure = __LINE__;
  } else if (edit == NULL || strcmp(json_string(json_get(edit, "newText")), "byte_order") != 0
             || integer_at(json_get(range, "start"), "line") != 0
             || integer_at(json_get(range, "start"), "character") != 1
             || integer_at(json_get(range, "end"), "character") != 11) {
    failure = __LINE__;
  } else if (elsewhere == NULL || elsewhere->kind != JSON_ARRAY || elsewhere->count != 0) {
    failure = __LINE__; /* a range no diagnostic touches is offered nothing */
  }
  session_free(&session);
  return failure;
}

/* #119 through the protocol: check 34 is published as a warning, and its fix -- removing the earlier of
 * the two -- is offered at the later one, where the warning is. */
static int us13_tc15_an_attribute_written_twice_is_a_warning_with_a_fix(const TestArguments *args) {
  (void)args;
  const char *const messages[] = {
    INITIALIZE,
    DID_OPEN("pkt TM {\\n  id: 8 = 1;\\n  [ @bit_order: lsb_first; @bit_order: msb_first; ]\\n  x: 8;\\n};\\n"),
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/codeAction\",\"params\":{\"textDocument\":{\"uri\":"
    "\"file:///tmp/t.fdsl\"},\"range\":{\"start\":{\"line\":2,\"character\":30},\"end\":{\"line\":2,\"character\":30}},"
    "\"context\":{\"diagnostics\":[]}}}",
    EXIT,
  };
  Session session;
  if (!run(&session, messages, 4)) {
    return __LINE__;
  }

  int failure = 0;
  const Json *published = json_get(last_diagnostics(&session), "diagnostics");
  const Json *warning =
    published != NULL && published->kind == JSON_ARRAY && published->count == 1 ? published->items[0] : NULL;
  const Json *actions = json_get(response_to(&session, 2), "result");
  const Json *action = actions != NULL && actions->kind == JSON_ARRAY && actions->count == 1 ? actions->items[0] : NULL;
  const Json *edits = json_get(json_get(json_get(action, "edit"), "changes"), "file:///tmp/t.fdsl");
  const Json *edit = edits != NULL && edits->kind == JSON_ARRAY && edits->count == 1 ? edits->items[0] : NULL;
  const Json *range = json_get(edit, "range");

  if (warning == NULL || integer_at(warning, "severity") != 2 || integer_at(warning, "code") != 34
      || integer_at(json_get(json_get(warning, "range"), "start"), "character") != 28) {
    failure = __LINE__;
  } else if (action == NULL || strcmp(json_string(json_get(action, "title")), "remove the earlier '@bit_order'") != 0) {
    failure = __LINE__;
  } else if (edit == NULL || strcmp(json_string(json_get(edit, "newText")), "") != 0
             || integer_at(json_get(range, "start"), "line") != 2
             || integer_at(json_get(range, "start"), "character") != 4
             || integer_at(json_get(range, "end"), "line") != 2
             || integer_at(json_get(range, "end"), "character") != 27) {
    failure = __LINE__;
  }
  session_free(&session);
  return failure;
}

void register_user_story13(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us13_tc01_initialize_says_what_it_can_do, "initialize announces what the server does", false);
  ADD_TEST_CASE(us13_tc02_utf8_positions_when_the_editor_takes_them, "UTF-8 positions when they are offered", false);
  ADD_TEST_CASE(us13_tc03_what_it_says_no_to, "too early, unknown, and not JSON are each refused", false);
  ADD_TEST_CASE(us13_tc04_an_open_document_is_diagnosed, "a document is diagnosed on open and on change", false);
  ADD_TEST_CASE(us13_tc05_a_warning_is_a_warning, "a warning arrives as one, with its check number", false);
  ADD_TEST_CASE(us13_tc06_formatting_is_one_edit, "formatting is one edit, or none at all", false);
  ADD_TEST_CASE(us13_tc07_semantic_tokens_say_what_each_name_is, "semantic tokens name every token", false);
  ADD_TEST_CASE(us13_tc08_a_document_closed_is_forgotten, "closing clears the diagnostics and the document", false);
  ADD_TEST_CASE(us13_tc09_a_range_change_is_spliced_in, "a change with a range is spliced in", false);
  ADD_TEST_CASE(us13_tc10_shutdown_then_exit_is_a_clean_exit, "shutdown then exit is 0; exit alone is 1", false);
  ADD_TEST_CASE(us13_tc11_every_state_of_typing_is_survivable, "every state of typing a file is survivable", false);
  ADD_TEST_CASE(
    us13_tc12_a_non_ascii_line_is_measured_the_editors_way, "a non-ASCII line is measured both ways", false
  );
  ADD_TEST_CASE(
    us13_tc13_hover_definition_and_completion_are_answered, "hover, definition and completion are answered", false
  );
  ADD_TEST_CASE(us13_tc14_a_fix_is_offered_as_a_code_action, "a fix is offered as a quick-fix code action", false);
  ADD_TEST_CASE(
    us13_tc15_an_attribute_written_twice_is_a_warning_with_a_fix, "a repeat in one group is a warning with a fix", false
  );
}
