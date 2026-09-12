#pragma once

/* Counting what the library asks the allocator for.
 *
 * The interposition lives in main.c so that both stories using it see one counter. Defining malloc in
 * the executable overrides libc's for every object linked into it, the library included, which is
 * what makes "nothing allocates after the frame exists" a measurement rather than a claim.
 *
 * Prove the counter is live before trusting a zero from it: an interposition that silently did not
 * take makes a test pass while measuring nothing, which is worse than having no test. */

#include <stddef.h>

void framedsl_allocation_count_start(void);

/* Stops counting and returns how many allocations happened since the start. */
size_t framedsl_allocation_count_stop(void);
