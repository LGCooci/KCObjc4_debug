#include <time.h>

#include <darwintest.h>
#include <darwintest_utils.h>

T_DECL(strptime_PR_27004626, "strptime() should fail when a %t doesn't match anything")
{
	struct tm tm;
	T_ASSERT_NULL(strptime("there'snotemplateforthis", "%t", &tm), NULL);
}

T_DECL(strptime_PR_29381762, "strptime() sets the tm_wday field incorrectly")
{
	time_t epoch = 0;
	struct tm t = *localtime(&epoch);

	T_LOG("2015-01-01 12:00:00 -> Thursday");
	(void)strptime("2015-01-01 12:00:00", "%F %T", &t);
	T_EXPECT_EQ(t.tm_wday, 4, NULL);

	T_LOG("2015-04-19 12:00:00 -> Sunday");
	(void)strptime("2015-04-19 12:00:00", "%F %T", &t);
	T_EXPECT_EQ(t.tm_wday, 0, NULL);

	T_LOG("2009-03-03 12:00:00 -> Tuesday");
	(void)strptime("2009-03-03 12:00:00", "%F %T", &t);
	T_EXPECT_EQ(t.tm_wday, 2, NULL);

	T_LOG("1990-02-15 12:00:00 -> Thursday");
	(void)strptime("1990-02-15 12:00:00", "%F %T", &t);
	T_EXPECT_EQ(t.tm_wday, 4, NULL);

	T_LOG("1993-03-02 12:00:00 -> Sunday");
	(void)strptime("1993-03-02 12:00:00", "%F %T", &t);
	T_EXPECT_EQ(t.tm_wday, 2, NULL);
}
