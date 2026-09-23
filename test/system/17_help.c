/* Story 17: somebody who has never used frame-dsl asks it how.
 *
 * `--help` is the first thing typed at a new tool, so it has to work from anywhere on the command
 * line and read cleanly on a terminal. Its option list is also what the Bash completion reads, so
 * the list is held to the parser here: every flag it lists is one the parser takes, and every flag
 * this story knows the tool has is listed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "testsuite.h"

typedef struct {
  int status;
  char out[16384];
  char err[4096];
} Run;

static void slurp(const char *path, char *buffer, size_t capacity) {
  buffer[0] = '\0';
  FILE *handle = fopen(path, "r");
  if (handle == NULL) {
    return;
  }
  size_t got = fread(buffer, 1, capacity - 1, handle);
  buffer[got] = '\0';
  fclose(handle);
}

/* Runs the binary with `arguments`, keeping stdout and stderr apart: which one the help goes to is
 * part of what is being tested. */
static Run run_with(const char *arguments) {
  Run run = { .status = -1 };

  char out_path[] = "/tmp/frame-dsl-help-out-XXXXXX";
  char err_path[] = "/tmp/frame-dsl-help-err-XXXXXX";
  int out_fd = mkstemp(out_path);
  int err_fd = mkstemp(err_path);
  if (out_fd >= 0) {
    close(out_fd);
  }
  if (err_fd >= 0) {
    close(err_fd);
  }
  if (out_fd < 0 || err_fd < 0) {
    return run;
  }

  char command[2048];
  snprintf(command, sizeof(command), "%s %s >%s 2>%s </dev/null", FRAMEDSL_BIN, arguments, out_path, err_path);
  int status = system(command);
  run.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

  slurp(out_path, run.out, sizeof(run.out));
  slurp(err_path, run.err, sizeof(run.err));
  remove(out_path);
  remove(err_path);
  return run;
}

static bool is_help(const Run *run) {
  return run->status == 0 && run->err[0] == '\0' && strstr(run->out, "usage:") != NULL
         && strstr(run->out, "options:") != NULL && strstr(run->out, "exit status:") != NULL;
}

static int us17_tc01_help_goes_to_stdout_and_succeeds(const TestArguments *args) {
  (void)args;

  const char *spellings[] = { "--help", "-h" };
  for (size_t i = 0; i < sizeof(spellings) / sizeof(*spellings); ++i) {
    Run run = run_with(spellings[i]);
    if (!is_help(&run)) {
      return __LINE__;
    }
    /* Plain text: no logger prefix, no colour. */
    if (strchr(run.out, '\033') != NULL || strstr(run.out, "[FAIL]") != NULL || strstr(run.out, "[INFO]") != NULL) {
      return __LINE__;
    }
  }
  return 0;
}

static int us17_tc02_help_wins_wherever_it_appears(const TestArguments *args) {
  (void)args;

  char arguments[512];
  snprintf(arguments, sizeof(arguments), "%s/uart_device.fdsl -e TC_WRITE size=2 data=AABB --help", EXAMPLES_DIR);
  Run run = run_with(arguments);
  /* The help, and no frame: TC_WRITE would have started "03 02". */
  if (!is_help(&run) || strstr(run.out, "03 02") != NULL) {
    return __LINE__;
  }

  /* Even where the rest of the command is wrong, which is exactly when somebody reaches for it. */
  run = run_with("--no-such-flag -h");
  if (!is_help(&run)) {
    return __LINE__;
  }
  return 0;
}

static int us17_tc03_version_names_the_build(const TestArguments *args) {
  (void)args;

  Run run = run_with("--version");
  if (run.status != 0 || run.err[0] != '\0' || strncmp(run.out, "frame-dsl ", 10) != 0) {
    return __LINE__;
  }
  /* One line, with something after the name -- a commit, or "unknown" outside a checkout. */
  const char *newline = strchr(run.out, '\n');
  if (newline == NULL || newline[1] != '\0' || newline - run.out <= 10) {
    return __LINE__;
  }
  return 0;
}

/* The flags the tool has. Adding one to the tool without adding it here fails tc05 below, because
 * --help then lists a flag this story has not heard of. */
static const char *const KNOWN_FLAGS[] = {
  "-d",      "--decode", "-e",       "--encode",     "-r",    "--report", "-q",     "--quiet",   "--emit-c",
  "--check", "--werror", "--format", "--stdin-name", "--lsp", "-h",       "--help", "--version",
};

#define KNOWN_FLAG_COUNT (sizeof(KNOWN_FLAGS) / sizeof(*KNOWN_FLAGS))

/* A flag as a whole word of the help text: preceded by a space and followed by a space, a comma or
 * the end of the line, so "-d" does not match inside "--decode". */
static bool help_lists(const char *help, const char *flag) {
  size_t length = strlen(flag);
  for (const char *at = strstr(help, flag); at != NULL; at = strstr(at + 1, flag)) {
    char after = at[length];
    if (at > help && at[-1] == ' ' && (after == ' ' || after == ',' || after == '\n')) {
      return true;
    }
  }
  return false;
}

static int us17_tc04_every_known_flag_is_listed(const TestArguments *args) {
  (void)args;

  Run run = run_with("--help");
  const char *options = strstr(run.out, "\noptions:\n");
  if (options == NULL) {
    return __LINE__;
  }
  for (size_t i = 0; i < KNOWN_FLAG_COUNT; ++i) {
    if (!help_lists(options, KNOWN_FLAGS[i])) {
      return __LINE__;
    }
  }
  return 0;
}

static int us17_tc05_every_listed_flag_is_accepted(const TestArguments *args) {
  (void)args;

  Run help = run_with("--help");
  const char *options = strstr(help.out, "\noptions:\n");
  const char *end = options == NULL ? NULL : strstr(options + 1, "\n\n");
  if (options == NULL || end == NULL) {
    return __LINE__;
  }

  size_t listed = 0;
  for (const char *line = options + 10; line < end;) {
    const char *newline = strchr(line, '\n');
    /* An option line's first word is a flag; a continued description's is not. */
    const bool option_line = line[strspn(line, " ")] == '-';
    for (const char *word = line; word < newline && option_line;) {
      while (word < newline && *word == ' ') {
        ++word;
      }
      if (*word != '-') {
        break;
      }
      char flag[64];
      size_t length = strcspn(word, " ,\n");
      if (length == 0 || length >= sizeof(flag)) {
        return __LINE__;
      }
      memcpy(flag, word, length);
      flag[length] = '\0';

      bool known = false;
      for (size_t i = 0; i < KNOWN_FLAG_COUNT; ++i) {
        known |= strcmp(flag, KNOWN_FLAGS[i]) == 0;
      }
      if (!known) {
        return __LINE__;
      }

      /* Whatever else it complains about, the parser must know the flag. A value is given so that a
       * flag which takes one is not refused for lacking it; --lsp is left out since it would wait
       * for an editor on stdin -- which is /dev/null here, so it would end, but it proves nothing. */
      if (strcmp(flag, "--lsp") != 0) {
        char arguments[128];
        snprintf(arguments, sizeof(arguments), "%s x", flag);
        Run run = run_with(arguments);
        if (strstr(run.err, "unexpected argument") != NULL) {
          return __LINE__;
        }
      }
      ++listed;

      word += length;
      if (*word == ',') {
        ++word;
      } else {
        break;
      }
    }
    line = newline + 1;
  }

  return listed == KNOWN_FLAG_COUNT ? 0 : __LINE__;
}

static int us17_tc06_a_mistake_gets_one_line_and_a_pointer(const TestArguments *args) {
  (void)args;

  Run run = run_with("--no-such-flag");
  if (run.status != 2 || run.out[0] != '\0') {
    return __LINE__;
  }
  if (strstr(run.err, "--no-such-flag") == NULL || strstr(run.err, "--help") == NULL) {
    return __LINE__;
  }
  /* The reason and the pointer, and not the help text all over again. */
  size_t lines = 0;
  for (const char *c = run.err; *c != '\0'; ++c) {
    lines += *c == '\n';
  }
  if (lines != 2 || strstr(run.err, "options:") != NULL) {
    return __LINE__;
  }
  return 0;
}

void register_user_story17(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us17_tc01_help_goes_to_stdout_and_succeeds, "--help and -h print plain help to stdout", false);
  ADD_TEST_CASE(
    us17_tc02_help_wins_wherever_it_appears, "--help anywhere prints the help and does nothing else", false
  );
  ADD_TEST_CASE(us17_tc03_version_names_the_build, "--version prints one line naming the build", false);
  ADD_TEST_CASE(us17_tc04_every_known_flag_is_listed, "every flag the tool has is in --help", false);
  ADD_TEST_CASE(us17_tc05_every_listed_flag_is_accepted, "every flag --help lists is one the parser takes", false);
  ADD_TEST_CASE(
    us17_tc06_a_mistake_gets_one_line_and_a_pointer, "a usage mistake is one line and a --help hint", false
  );
}
