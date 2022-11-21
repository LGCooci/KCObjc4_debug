#include <stdlib.h>
#include <darwintest.h>
#include <malloc_private.h>

T_DECL(malloc_checkfix_zero_on_free, "Test malloc_zero_on_free_disable() SPI",
		T_META_ENVVAR("MallocZeroOnFree=1"))
{
	// Drive some activity up front
	void *p1 = malloc(16);
	T_ASSERT_NOTNULL(p1, "malloc 1");
	void *p2 = malloc(512);
	T_ASSERT_NOTNULL(p1, "malloc 2");

	free(p2);

	// Call the checkfix SPI
	malloc_zero_on_free_disable();

	// Drive some more activity
	void *p3 = calloc(1, 512);
	T_ASSERT_NOTNULL(p3, "calloc 1");

	free(p3);
	free(p1);

	T_PASS("Reached the end");
}
