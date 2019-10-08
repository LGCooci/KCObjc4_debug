#include <stdlib.h>

#include <malloc_private.h>

#include <darwintest.h>

T_DECL(reallorarray, "reallocarray(3)",
	   T_META_CHECK_LEAKS(NO)){
	void *ptr;
	T_WITH_ERRNO; T_EXPECT_NOTNULL((ptr = reallocarray(NULL, 8, 8)), NULL);
	T_WITH_ERRNO; T_EXPECT_NOTNULL(reallocarray(ptr, 8, 8), NULL);
	T_EXPECT_NULL(reallocarray(NULL, SIZE_MAX >> 3, 1 << 5), NULL);
	T_EXPECT_EQ(errno, ENOMEM, NULL);
	T_EXPECT_NULL(reallocarray(ptr, SIZE_MAX >> 3, 1 << 5), NULL);
	T_EXPECT_EQ(errno, ENOMEM, NULL);
}

T_DECL(reallorarrayf, "reallocarrayf(3)",
	   T_META_CHECK_LEAKS(NO)){
	void *ptr;
	T_WITH_ERRNO; T_EXPECT_NOTNULL((ptr = reallocarrayf(NULL, 8, 8)), NULL);
	T_WITH_ERRNO; T_EXPECT_NOTNULL(reallocarrayf(ptr, 8, 8), NULL);
	T_EXPECT_NULL(reallocarrayf(NULL, SIZE_MAX >> 3, 1 << 5), NULL);
	T_EXPECT_EQ(errno, ENOMEM, NULL);
	T_EXPECT_NULL(reallocarrayf(ptr, SIZE_MAX >> 3, 1 << 5), NULL);
	T_EXPECT_EQ(errno, ENOMEM, NULL);
}
