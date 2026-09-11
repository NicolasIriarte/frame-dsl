/* DELETE ME.
 *
 * This story tests nothing. It exists only so that `make test` has something to run before any real
 * test does, proving the runner builds, registers a story and reports a result. The moment a test of
 * actual behaviour lands, delete this file -- a placeholder that keeps passing next to real tests is
 * worse than no placeholder, because it makes the suite look like it covers one thing more than it
 * does. Removing it needs no other change: the registry is generated from the files present. */
#include "testsuite.h"

static int us1_tc01_runner_works(const TestArguments *args) {
  (void)args;

  if (1 + 1 != 2) {
    FORCE_FAILURE();
  }
  return 0;
}

void register_user_story1(void) {
  START_REGISTERING();
  ADD_TEST_CASE(us1_tc01_runner_works, "the test runner runs a case and reports it", true);
}
