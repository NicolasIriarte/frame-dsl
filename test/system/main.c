/* System test runner: registers every story the generated registry lists.
 *
 * It also interposes the allocator for the whole binary -- see allocations.h. This belongs here
 * rather than in a story because two stories count allocations and there must be one counter. */
#include <stdbool.h>
#include <stdlib.h>

#include "allocations.h"
#include "testsuite.h"
#include "testsuite_registry.h"

static bool counting = false;
static size_t allocations = 0;

extern void *__libc_malloc(size_t size);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *pointer, size_t size);
extern void __libc_free(void *pointer);

void *malloc(size_t size) {
  if (counting) {
    allocations += 1;
  }
  return __libc_malloc(size);
}

void *calloc(size_t count, size_t size) {
  if (counting) {
    allocations += 1;
  }
  return __libc_calloc(count, size);
}

void *realloc(void *pointer, size_t size) {
  if (counting) {
    allocations += 1;
  }
  return __libc_realloc(pointer, size);
}

void free(void *pointer) {
  __libc_free(pointer);
}

void framedsl_allocation_count_start(void) {
  allocations = 0;
  counting = true;
}

size_t framedsl_allocation_count_stop(void) {
  counting = false;
  return allocations;
}

int main(void) {
#define __REGISTER_ONE(id) REGISTER_USER_STORY(id);
  TESTSUITE_USER_STORIES(__REGISTER_ONE)
#undef __REGISTER_ONE

  return test_user_stories();
}
