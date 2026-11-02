/* Story 16: `frame-dsl --lsp` as an editor meets it (#30).
 *
 * The server's own behaviour is covered case by case in test/unit/13_lsp.c, which drives it in
 * process. What is left -- and what this asserts -- is the part only the real binary can show: that
 * the flag reaches the server, that it talks over its own stdin and stdout with nothing else mixed
 * in, and that it exits when told to. */
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.h"

typedef struct {
  pid_t pid;
  FILE *in;
  FILE *out;
} Server;

static bool server_start(Server *server) {
  int to_server[2];
  int from_server[2];
  if (pipe(to_server) != 0 || pipe(from_server) != 0) {
    return false;
  }
  server->pid = fork();
  if (server->pid < 0) {
    return false;
  }
  if (server->pid == 0) {
    dup2(to_server[0], STDIN_FILENO);
    dup2(from_server[1], STDOUT_FILENO);
    close(to_server[1]);
    close(from_server[0]);
    execl(FRAMEDSL_BIN, FRAMEDSL_BIN, "--lsp", (char *)NULL);
    _exit(127);
  }
  close(to_server[0]);
  close(from_server[1]);
  server->in = fdopen(to_server[1], "w");
  server->out = fdopen(from_server[0], "r");
  return server->in != NULL && server->out != NULL;
}

static void send(Server *server, const char *message) {
  fprintf(server->in, "Content-Length: %zu\r\n\r\n%s", strlen(message), message);
  fflush(server->in);
}

/* One message back, body only. NULL if the server said nothing more. */
static char *receive(Server *server) {
  size_t length = 0;
  char line[256];
  while (fgets(line, sizeof(line), server->out) != NULL) {
    if (strcmp(line, "\r\n") == 0) {
      break;
    }
    if (strncmp(line, "Content-Length:", 15) == 0) {
      length = (size_t)atol(line + 15);
    }
  }
  if (length == 0) {
    return NULL;
  }
  char *body = malloc(length + 1);
  if (body == NULL || fread(body, 1, length, server->out) != length) {
    free(body);
    return NULL;
  }
  body[length] = '\0';
  return body;
}

static int server_stop(Server *server) {
  fclose(server->in);
  fclose(server->out);
  int status = 0;
  waitpid(server->pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int us16_tc01_the_binary_serves_an_editor(const TestArguments *args) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/t.fdsl", args->tmp_dir);
  char uri[1100];
  snprintf(uri, sizeof(uri), "file://%s", path);

  Server server;
  if (!server_start(&server)) {
    return __LINE__;
  }

  send(&server, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
  char *reply = receive(&server);
  int failure = 0;
  if (reply == NULL || strstr(reply, "\"serverInfo\"") == NULL || strstr(reply, "frame-dsl") == NULL) {
    failure = __LINE__;
  }
  free(reply);

  /* A file with one mistake in it, opened as the editor would open it. */
  char message[2048];
  snprintf(
    message,
    sizeof(message),
    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"%s\","
    "\"languageId\":\"fdsl\",\"version\":1,\"text\":\"pkt TM {\\n  id: 8\\n};\\n\"}}}",
    uri
  );
  send(&server, message);
  reply = receive(&server);
  if (
    failure == 0
    && (reply == NULL || strstr(reply, "publishDiagnostics") == NULL || strstr(reply, "expected ';'") == NULL || strstr(reply, uri) == NULL)) {
    failure = __LINE__;
  }
  free(reply);

  send(&server, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"shutdown\"}");
  reply = receive(&server);
  if (failure == 0 && (reply == NULL || strstr(reply, "\"result\":null") == NULL)) {
    failure = __LINE__;
  }
  free(reply);

  send(&server, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
  int status = server_stop(&server);
  if (failure == 0 && status != 0) {
    failure = __LINE__;
  }
  return failure;
}

/* Nothing but protocol on stdout: a stray printf anywhere in the library would put an editor out of
 * step with the stream for good, and it is the kind of thing that gets added by accident. */
static int us16_tc02_nothing_but_protocol_on_stdout(const TestArguments *args) {
  (void)args;
  Server server;
  if (!server_start(&server)) {
    return __LINE__;
  }
  send(&server, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
  char *reply = receive(&server);
  free(reply);

  /* An imported file that is not there, and a warning: two things the tool has messages of its own
   * about. */
  send(
    &server,
    "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file:///tmp/"
    "missing_import.fdsl\",\"languageId\":\"fdsl\",\"version\":1,\"text\":\"import \\\"nowhere.fdsl\\\";\\nblock S "
    "{\\n  s: 8;\\n};\\npkt TM {\\n  id: 8 := 1;\\n};\\n\"}}}"
  );
  reply = receive(&server);
  int failure = reply != NULL && strstr(reply, "publishDiagnostics") != NULL ? 0 : __LINE__;
  free(reply);

  send(&server, "{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
  /* Exit without shutdown is a 1, which is the protocol's way of saying the editor did not ask. */
  int status = server_stop(&server);
  if (failure == 0 && status != 1) {
    failure = __LINE__;
  }
  return failure;
}

void register_user_story16(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us16_tc01_the_binary_serves_an_editor, "the binary serves an editor over stdin and stdout", false);
  ADD_TEST_CASE(us16_tc02_nothing_but_protocol_on_stdout, "nothing but protocol is written to stdout", false);
}
