/* Story 28: where the tool's output goes (#142). Results on stdout and problems on stderr, so that either
 * can be piped and an error is never mistaken for a result; nothing of this project's source in them;
 * and colour only on a terminal, only on what names a problem as one. */
#define _XOPEN_SOURCE 700

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.h"

/* Records the first failing line and carries on, so a case reads as a list of expectations. */
#define EXPECT(condition)                                                                                              \
  do {                                                                                                                 \
    if (failure == 0 && !(condition)) {                                                                                \
      failure = __LINE__;                                                                                              \
    }                                                                                                                  \
  } while (0)

#define UART EXAMPLES_DIR "/uart_device.fdsl"
#define INVALID EXAMPLES_DIR "/invalid/unknown_block.fdsl"
#define NO_MATCH "frame-dsl: at byte 0: no packet in this definition matches these bytes\n"

/* Loads, with one warning. */
#define UNUSED_BLOCK                                                                                                   \
  "block Spare {\n"                                                                                                    \
  "  a: 8;\n"                                                                                                          \
  "};\n"                                                                                                               \
  "pkt P {\n"                                                                                                          \
  "  id: 8 = 0x01;\n"                                                                                                  \
  "};\n"

typedef struct {
  int status;
  char text[8192];
} Stream;

/* One stream of a run, by what `redirect` does with the shell's descriptors. */
static Stream capture(const char *arguments, const char *redirect) {
  Stream stream = { .status = -1, .text = { 0 } };

  char command[1024];
  snprintf(command, sizeof(command), "%s %s %s", FRAMEDSL_BIN, arguments, redirect);

  FILE *pipe = popen(command, "r");
  if (pipe == NULL) {
    return stream;
  }
  size_t used = fread(stream.text, 1, sizeof(stream.text) - 1, pipe);
  stream.text[used] = '\0';

  int closed = pclose(pipe);
  stream.status = WIFEXITED(closed) ? WEXITSTATUS(closed) : -1;
  return stream;
}

static Stream out_of(const char *arguments) {
  return capture(arguments, "2>/dev/null");
}

static Stream err_of(const char *arguments) {
  return capture(arguments, "2>&1 >/dev/null");
}

/* Runs the tool with stderr and stdout on a terminal, and returns what it wrote there, or false where
 * this machine has no terminals to give out. A terminal turns each newline into two characters, so
 * this is compared by what it holds rather than as a whole. */
static bool on_terminal(const char *arguments, const char *environment, char *text, size_t size) {
  int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
    return false;
  }
  const char *name = ptsname(master);
  if (name == NULL) {
    return false;
  }

  char command[1024];
  snprintf(command, sizeof(command), "%s %s %s", environment, FRAMEDSL_BIN, arguments);

  pid_t child = fork();
  if (child < 0) {
    return false;
  }
  if (child == 0) {
    int slave = open(name, O_RDWR);
    if (slave < 0) {
      _exit(127);
    }
    dup2(slave, STDOUT_FILENO);
    dup2(slave, STDERR_FILENO);
    close(master);
    execl("/bin/sh", "sh", "-c", command, (char *)NULL);
    _exit(127);
  }

  size_t used = 0;
  ssize_t got;
  /* It ends in an error rather than a zero when the other side of a terminal closes. */
  while (used + 1 < size && (got = read(master, text + used, size - used - 1)) > 0) {
    used += (size_t)got;
  }
  text[used] = '\0';
  close(master);
  waitpid(child, NULL, 0);
  return true;
}

static bool plain(const char *text) {
  return strchr(text, '\033') == NULL && strstr(text, "src/") == NULL && strstr(text, "[FAIL]") == NULL
         && strstr(text, "[INFO]") == NULL;
}

static int us28_tc01_a_failed_decode_writes_to_stderr_alone(const TestArguments *args) {
  (void)args;
  int failure = 0;

  Stream out = out_of(UART " -d FF");
  Stream err = err_of(UART " -d FF");
  EXPECT(out.status == 1 && err.status == 1);
  EXPECT(out.text[0] == '\0');
  EXPECT(strcmp(err.text, NO_MATCH) == 0);
  return failure;
}

/* A capture that goes wrong part-way: what was read is a result, and the failure is not. */
static int us28_tc02_frames_before_a_failure_stay_on_stdout(const TestArguments *args) {
  (void)args;
  int failure = 0;

  Stream out = out_of(UART " -d '01 00 02 FF'");
  Stream err = err_of(UART " -d '01 00 02 FF'");
  EXPECT(out.status == 1);
  EXPECT(strstr(out.text, "pkt TC_HK_REQ {") != NULL && strstr(out.text, "frame-dsl:") == NULL);
  EXPECT(strcmp(err.text, "frame-dsl: at byte 3: no packet in this definition matches these bytes\n") == 0);

  /* And in that order when both go to one place. */
  Stream both = capture(UART " -d '01 00 02 FF'", "2>&1");
  const char *frame = strstr(both.text, "pkt TC_HK_REQ {");
  const char *problem = strstr(both.text, "frame-dsl:");
  EXPECT(frame != NULL && problem != NULL && frame < problem);
  return failure;
}

static int us28_tc03_a_description_is_plain_text_on_stdout(const TestArguments *args) {
  (void)args;
  int failure = 0;

  Stream out = out_of(UART);
  Stream err = err_of(UART);
  EXPECT(out.status == 0);
  EXPECT(strncmp(out.text, "pkt TC_HK_REQ: ", 15) == 0);
  EXPECT(strstr(out.text, ": ok, ") != NULL && plain(out.text));
  EXPECT(err.text[0] == '\0');
  return failure;
}

/* Bad hex is the command line's fault, so its status is 2, and it is said the same way. */
static int us28_tc04_bad_hex_is_a_problem_like_any_other(const TestArguments *args) {
  (void)args;
  int failure = 0;

  Stream out = out_of(UART " -d 'A'");
  Stream err = err_of(UART " -d 'A'");
  EXPECT(out.status == 2 && out.text[0] == '\0');
  EXPECT(strcmp(err.text, "frame-dsl: hex: an odd number of digits; the last byte is incomplete\n") == 0);
  return failure;
}

/* A definition's problems are the one shape in every mode, the one --check prints and editors read. */
static int us28_tc05_a_bad_definition_reads_the_same_in_every_mode(const TestArguments *args) {
  (void)args;
  int failure = 0;

  Stream checked = out_of(INVALID " --check");
  EXPECT(checked.status == 1 && strstr(checked.text, ": error: ") != NULL && plain(checked.text));

  Stream described = err_of(INVALID);
  Stream decoded = err_of(INVALID " -d FF");
  EXPECT(described.status == 1 && decoded.status == 1);
  EXPECT(strcmp(described.text, checked.text) == 0);
  EXPECT(strcmp(decoded.text, checked.text) == 0);

  EXPECT(out_of(INVALID).text[0] == '\0');
  return failure;
}

static int us28_tc06_on_a_terminal_only_the_name_of_a_problem_is_coloured(const TestArguments *args) {
  (void)args;
  int failure = 0;
  char text[8192];

  if (!on_terminal(UART " -d '01 00 02 FF'", "", text, sizeof(text))) {
    return 0; /* no terminal to be had here: the rest of the story still holds */
  }
  const char *colour = strstr(text, "\033[31mframe-dsl:\033[0m at byte 3: ");
  EXPECT(colour != NULL);
  /* That one name, and nothing else: every other escape would show up before or after it. */
  EXPECT(colour != NULL && strchr(text, '\033') == colour);
  EXPECT(colour != NULL && strchr(colour + strlen("\033[31mframe-dsl:\033[0"), '\033') == NULL);

  EXPECT(on_terminal(UART " -d FF", "NO_COLOR=1", text, sizeof(text)));
  EXPECT(strchr(text, '\033') == NULL && strstr(text, "frame-dsl: at byte 0: ") != NULL);

  /* An empty NO_COLOR says nothing, as https://no-color.org has it. */
  EXPECT(on_terminal(UART " -d FF", "NO_COLOR=", text, sizeof(text)));
  EXPECT(strstr(text, "\033[31mframe-dsl:\033[0m ") != NULL);
  return failure;
}

/* Printed for a person in every mode but --check, so the severity is what catches the eye. */
static int us28_tc08_on_a_terminal_a_problem_with_a_definition_shows_its_severity(const TestArguments *args) {
  int failure = 0;
  char text[8192];

  if (!on_terminal(INVALID, "", text, sizeof(text))) {
    return 0;
  }
  EXPECT(strstr(text, ": \033[31merror:\033[0m ") != NULL);

  char path[1024];
  snprintf(path, sizeof(path), "%s/unused_block.fdsl", args->tmp_dir);
  FILE *handle = fopen(path, "w");
  EXPECT(handle != NULL);
  if (handle != NULL) {
    fputs(UNUSED_BLOCK, handle);
    fclose(handle);
  }

  EXPECT(on_terminal(path, "", text, sizeof(text)));
  const char *colour = strstr(text, ": \033[33mwarning:\033[0m ");
  EXPECT(colour != NULL);
  /* That word, and nothing else: the description that follows is a result. */
  EXPECT(colour != NULL && strchr(text, '\033') == colour + strlen(": "));
  EXPECT(colour != NULL && strchr(colour + strlen(": \033[33mwarning:\033[0"), '\033') == NULL);
  EXPECT(strstr(text, "pkt P: 1 field") != NULL);

  EXPECT(on_terminal(path, "NO_COLOR=1", text, sizeof(text)));
  EXPECT(strstr(text, ": warning: ") != NULL && plain(text));

  char checked[1100];
  snprintf(checked, sizeof(checked), "%s --check", path);
  EXPECT(on_terminal(checked, "", text, sizeof(text)));
  EXPECT(strstr(text, ": warning: ") != NULL && plain(text));
  return failure;
}

/* What editors and scripts parse stays plain, on a terminal or not. */
static int us28_tc07_check_and_format_stay_plain_on_a_terminal(const TestArguments *args) {
  (void)args;
  int failure = 0;
  char text[8192];

  if (!on_terminal(INVALID " --check", "", text, sizeof(text))) {
    return 0;
  }
  EXPECT(strstr(text, ": error: ") != NULL && plain(text));
  EXPECT(on_terminal(UART " --format --check", "", text, sizeof(text)));
  EXPECT(plain(text));
  return failure;
}

void register_user_story28(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us28_tc01_a_failed_decode_writes_to_stderr_alone, "a failed decode writes to stderr alone", false);
  ADD_TEST_CASE(us28_tc02_frames_before_a_failure_stay_on_stdout, "frames before a failure stay on stdout", false);
  ADD_TEST_CASE(us28_tc03_a_description_is_plain_text_on_stdout, "a description is plain text on stdout", false);
  ADD_TEST_CASE(us28_tc04_bad_hex_is_a_problem_like_any_other, "bad hex is a problem like any other", false);
  ADD_TEST_CASE(
    us28_tc05_a_bad_definition_reads_the_same_in_every_mode, "a bad definition reads the same in every mode", false
  );
  ADD_TEST_CASE(
    us28_tc06_on_a_terminal_only_the_name_of_a_problem_is_coloured,
    "on a terminal only the name of a problem is coloured",
    false
  );
  ADD_TEST_CASE(
    us28_tc07_check_and_format_stay_plain_on_a_terminal, "--check and --format stay plain on a terminal", false
  );
  ADD_TEST_CASE(
    us28_tc08_on_a_terminal_a_problem_with_a_definition_shows_its_severity,
    "on a terminal a problem with a definition shows its severity",
    false
  );
}
