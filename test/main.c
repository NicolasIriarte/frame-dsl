/* Test runner entrypoint. Registers every user story the generated registry lists, then runs them.
 *
 * Nothing is registered by hand here: `gen_test_registry` writes testsuite_registry.h from the
 * test/NN_*.c files that exist, so adding a story is adding a file. */
#include "testsuite.h"
#include "testsuite_registry.h"

int main(void) {
#define __REGISTER_ONE(id) REGISTER_USER_STORY(id);
  TESTSUITE_USER_STORIES(__REGISTER_ONE)
#undef __REGISTER_ONE

  return test_user_stories();
}
