#include <stdio.h>
#include <stdlib.h>

#include "diag.h"
#include "logger.h"
#include "parser.h"

/* Reads the whole file. Definition files are small by nature -- they describe a handful of packet
 * layouts -- so there is no reason to stream one. */
static char *read_file(const char *path, DiagList *diags) {
  FILE *handle = fopen(path, "rb");
  if (handle == NULL) {
    diag_add(diags, 0, 0, 0, "cannot open '%s'", path);
    return NULL;
  }

  if (fseek(handle, 0, SEEK_END) != 0) {
    diag_add(diags, 0, 0, 0, "cannot read '%s'", path);
    fclose(handle);
    return NULL;
  }
  long size = ftell(handle);
  if (size < 0) {
    diag_add(diags, 0, 0, 0, "cannot read '%s'", path);
    fclose(handle);
    return NULL;
  }
  rewind(handle);

  char *source = malloc((size_t)size + 1);
  if (source == NULL) {
    diag_add(diags, 0, 0, 0, "out of memory reading '%s'", path);
    fclose(handle);
    return NULL;
  }

  size_t read = fread(source, 1, (size_t)size, handle);
  fclose(handle);
  source[read] = '\0';
  return source;
}

static void report(const char *path, const DiagList *diags) {
  for (size_t i = 0; i < diags->count; ++i) {
    const Diag *diag = &diags->items[i];
    if (diag->line > 0) {
      log_fail("%s:%d:%d: %s", path, diag->line, diag->column, diag->message);
    } else {
      log_fail("%s: %s", path, diag->message);
    }
  }
  if (diags->overflowed) {
    log_fail("%s: more problems follow, not reported", path);
  }
}

static void describe(const AstFile *file) {
  for (size_t p = 0; p < file->packet_count; ++p) {
    const AstPacket *packet = &file->packets[p];
    log_info("pkt %s: %zu field%s", packet->name, packet->field_count,
             packet->field_count == 1 ? "" : "s");
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    log_fail("usage: %s <definition.fdsl>", argv[0]);
    return 2;
  }

  DiagList diags;
  diag_init(&diags);

  char *source = read_file(argv[1], &diags);
  if (source == NULL) {
    report(argv[1], &diags);
    diag_free(&diags);
    return 2;
  }

  AstFile *file = parse(source, &diags);

  int status = 0;
  if (diags.count > 0) {
    report(argv[1], &diags);
    status = 1;
  } else if (file != NULL) {
    describe(file);
    log_info("%s: parsed, %zu packet%s", argv[1], file->packet_count,
             file->packet_count == 1 ? "" : "s");
  }

  ast_free(file);
  free(source);
  diag_free(&diags);
  return status;
}
