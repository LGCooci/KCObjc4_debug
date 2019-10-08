#include <darwintest.h>
#include <stdlib.h>
#include <os/stdlib.h>

T_DECL(os_simple_hash, "sanity check of os_simple_hash",
		T_META_ALL_VALID_ARCHS(true))
{
	const char * string =
			"We made the buttons on the screen look so good you'll want to lick them.";
	uint64_t hashval = os_simple_hash_string(string);
	T_EXPECT_NE(hashval, 0ULL, "usually should get a non-0 hash value");

	char buf[1024];
	arc4random_buf(buf, sizeof(buf));
	hashval = os_simple_hash(buf, sizeof(buf));
	T_EXPECT_NE(hashval, 0ULL, "usually should get a non-0 hash value");
}
