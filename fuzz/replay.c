/* Replays inputs through a fuzz target without libFuzzer, so `make test` checks every saved input on
 * any compiler -- in CI too, which has no clang.
 *
 * Usage: replay DIRECTORY...  Each regular file in each directory is one input. Exits non-zero only
 * through the target itself: an abort(), or a sanitizer report, is the failure. */
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* Neither target has any reason to take longer than this on any input; a run that does is a hang, not
 * a slow machine -- see the Makefile's FUZZ_TIMEOUT. Without this, a hang committed as a regression
 * would block `make test`, and CI with it, forever rather than failing it. */
#define REPLAY_TIMEOUT_SECONDS 5

static const char *hung_path = NULL;

static void report_hang(int signum) {
  (void)signum;
  const char *message = hung_path != NULL ? hung_path : "(unknown input)";
  /* async-signal-safe, and the only thing here that can fail is the write itself -- nothing to do
   * about that with _exit() one line below regardless, so the result is deliberately unchecked. */
  ssize_t ignored;
  ignored = write(STDERR_FILENO, "replay: timed out on ", 21);
  ignored = write(STDERR_FILENO, message, strlen(message));
  ignored = write(STDERR_FILENO, "\n", 1);
  (void)ignored;
  _exit(1);
}

static int replay_file(const char *path) {
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    fprintf(stderr, "replay: cannot read %s\n", path);
    return 1;
  }
  uint8_t *data = NULL;
  size_t size = 0;
  size_t capacity = 0;
  for (;;) {
    if (size == capacity) {
      capacity = capacity == 0 ? 4096 : capacity * 2;
      uint8_t *grown = realloc(data, capacity);
      if (grown == NULL) {
        free(data);
        fclose(handle);
        return 1;
      }
      data = grown;
    }
    size_t got = fread(data + size, 1, capacity - size, handle);
    if (got == 0) {
      break;
    }
    size += got;
  }
  fclose(handle);

  hung_path = path;
  alarm(REPLAY_TIMEOUT_SECONDS);
  LLVMFuzzerTestOneInput(data, size);
  alarm(0);
  free(data);
  return 0;
}

int main(int argc, char **argv) {
  signal(SIGALRM, report_hang);

  size_t count = 0;
  for (int i = 1; i < argc; ++i) {
    DIR *directory = opendir(argv[i]);
    if (directory == NULL) {
      fprintf(stderr, "replay: cannot open %s\n", argv[i]);
      return 1;
    }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
      if (entry->d_name[0] == '.') {
        continue;
      }
      char path[4096];
      snprintf(path, sizeof(path), "%s/%s", argv[i], entry->d_name);
      struct stat info;
      if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        continue;
      }
      if (replay_file(path) != 0) {
        closedir(directory);
        return 1;
      }
      count += 1;
    }
    closedir(directory);
  }
  printf("replayed %zu input%s\n", count, count == 1 ? "" : "s");
  return 0;
}
