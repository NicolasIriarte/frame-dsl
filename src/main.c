/* frame-dsl: validate a definition file and describe what is in it.
 *
 * Written against the public header alone, with nothing internal included. That is deliberate: if
 * the tool shipped with the project needs to reach inside, the interface is not finished. */
#include <stdio.h>

#include "framedsl.h"
#include "logger.h"

static void report(const FramedslResult *result) {
  const char *name = framedsl_source_name(result);

  for (size_t i = 0; i < framedsl_diagnostic_count(result); ++i) {
    const FramedslDiagnostic *diag = framedsl_diagnostic(result, i);

    /* The check number is printed because it is what makes a rejection checkable: "rejected" is a
     * weaker claim than "rejected for the reason the file says it should be". */
    char check[32] = "";
    if (diag->check > 0) {
      snprintf(check, sizeof(check), " [check %d]", diag->check);
    }

    if (diag->line > 0) {
      log_fail("%s:%d:%d:%s %s", name, diag->line, diag->column, check, diag->message);
    } else {
      log_fail("%s:%s %s", name, check, diag->message);
    }
  }
}

static void describe(const FramedslDefinition *definition) {
  for (size_t p = 0; p < framedsl_packet_count(definition); ++p) {
    size_t fields = framedsl_field_count(definition, p);

    log_info("pkt %s: %zu field%s, %llu bits reserved", framedsl_packet_name(definition, p), fields,
             fields == 1 ? "" : "s",
             (unsigned long long)framedsl_packet_reserved_bits(definition, p));

    for (size_t f = 0; f < fields; ++f) {
      FramedslField field;
      if (!framedsl_field(definition, p, f, &field)) {
        continue;
      }

      char offset[32] = "at ?";
      if (field.offset_known) {
        snprintf(offset, sizeof(offset), "at %llu", (unsigned long long)field.offset_bits);
      }

      if (field.variable) {
        log_info("    %-14s %s, variable, max %llu bits", field.name, offset,
                 (unsigned long long)field.max_bits);
      } else if (field.has_fixed) {
        log_info("    %-14s %s, %llu bits = %#llx", field.name, offset,
                 (unsigned long long)field.width_bits, (unsigned long long)field.fixed);
      } else {
        log_info("    %-14s %s, %llu bits", field.name, offset,
                 (unsigned long long)field.width_bits);
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2) {
    log_fail("usage: %s <definition.fdsl>", argv[0]);
    return 2;
  }

  FramedslResult *result = framedsl_load_file(argv[1]);
  if (result == NULL) {
    log_fail("out of memory");
    return 2;
  }

  int status;
  switch (framedsl_status(result)) {
  case FRAMEDSL_OK: {
    const FramedslDefinition *definition = framedsl_definition(result);
    describe(definition);
    log_info("%s: ok, %zu packet%s", framedsl_source_name(result),
             framedsl_packet_count(definition), framedsl_packet_count(definition) == 1 ? "" : "s");
    status = 0;
    break;
  }
  case FRAMEDSL_ERROR_INVALID:
    report(result);
    status = 1;
    break;
  /* A missing file is not a malformed file, and a script should not have to grep the message to
   * tell them apart. */
  case FRAMEDSL_ERROR_IO:
  case FRAMEDSL_ERROR_MEMORY:
  default:
    report(result);
    status = 2;
    break;
  }

  framedsl_result_free(result);
  return status;
}
