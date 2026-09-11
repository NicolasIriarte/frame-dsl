/* Unit test runner: registers every story the generated registry lists. */
#include "testsuite.h"
#include "testsuite_registry.h"

int main(void) {
#define __REGISTER_ONE(id) REGISTER_USER_STORY(id);
  TESTSUITE_USER_STORIES(__REGISTER_ONE)
#undef __REGISTER_ONE

  return test_user_stories();
}
