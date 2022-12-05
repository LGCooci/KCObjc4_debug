// TEST_CONFIG OS=!macosx

#include "test.h"
#include "testroot.i"
#include <simd/simd.h>

#if TARGET_OS_OSX
#include <Cambria/Traps.h>
#include <Cambria/Cambria.h>
#endif

#ifndef TEST_NAME
#define TEST_NAME __FILE__
#endif

#if defined(__arm__) 
// rdar://8331406
#   define ALIGN_() 
#else
#   define ALIGN_() asm(".align 4");
#endif


@interface Super : TestRoot @end

@implementation Super

-(void)voidret_nop
{
    return;
}

-(void)voidret_nop2
{
    return;
}

-(id)idret_nop
{
    return nil;
}

-(long long)llret_nop
{
    return 0;
}

-(struct stret)stret_nop
{
    return STRET_RESULT;
}

-(double)fpret_nop
{
    return 0;
}

-(long double)lfpret_nop
{
    return 0;
}

-(vector_ulong2)vecret_nop
{
    return (vector_ulong2){0x1234567890abcdefULL, 0xfedcba0987654321ULL};
}

@end


@interface Sub : Super @end

@implementation Sub @end


int main()
{

    // cached message performance
    // catches failure to cache or (abi=2) failure to fixup (#5584187)
    // fixme unless they all fail

    uint64_t minTime;
    uint64_t targetTime;

    Sub *sub = [Sub new];

    // fill cache first

    [sub voidret_nop];
    [sub voidret_nop2];
    [sub llret_nop];
    [sub stret_nop];
    [sub fpret_nop];
    [sub lfpret_nop];
    [sub vecret_nop];
    [sub voidret_nop];
    [sub voidret_nop2];
    [sub llret_nop];
    [sub stret_nop];
    [sub fpret_nop];
    [sub lfpret_nop];
    [sub vecret_nop];
    [sub voidret_nop];
    [sub voidret_nop2];
    [sub llret_nop];
    [sub stret_nop];
    [sub fpret_nop];
    [sub lfpret_nop];
    [sub vecret_nop];

    // Some of these times have high variance on some compilers. 
    // The errors we're trying to catch should be catastrophically slow, 
    // so the margins here are generous to avoid false failures.

    // Use voidret because id return is too slow for perf test with ARC.

    // Pick smallest of voidret_nop and voidret_nop2 time
    // in the hopes that one of them didn't collide in the method cache.

    // ALIGN_ matches loop alignment to make -O0 work

#define TRIALS 50
#define MESSAGES 1000000

    // Measure the time needed to send MESSAGES messages. To reduce the
    // influence of transient effects on the result, it will perform TRIALS
    // measurements, then take the minimum time from those trials. On
    // completion, the minimum measured time is stored in `minTime`.
    //
    // We take a minimum over many trials rather than a simple average for two
    // reasons:
    //
    // 1. If preemption or sudden system load makes a trial slow, it is not
    //    useful to incorporate that into the data. We want to reject those
    //    trials. There aren't transient events that will make a trial unusually
    //    *fast*, so the minimum is what we want to measure.
    // 2. Some hardware seems to take time to ramp up performance when suddenly
    //    placed under load. The first ~10 trials of a test run can be much
    //    slower than the rest, causing subsequent tests to be "too fast.'
#define MEASURE(message)                                           \
    do {                                                           \
        minTime = UINT64_MAX;                                      \
        for (int i = 0; i < TRIALS; i++) {                         \
            uint64_t startTime = mach_absolute_time();             \
            ALIGN_();                                              \
            for (int i = 0; i < MESSAGES; i++)                     \
                [sub message];                                     \
            uint64_t totalTime = mach_absolute_time() - startTime; \
            testprintf("trial: " #message "  %llu\n", totalTime);  \
            if (totalTime < minTime)                               \
                minTime = totalTime;                               \
        }                                                          \
    } while(0)
    
    MEASURE(voidret_nop);
    testprintf("BASELINE: voidret  %llu\n", minTime);
    targetTime = minTime;
    
    MEASURE(voidret_nop2);
    testprintf("BASELINE: voidret2  %llu\n", minTime);
    if (minTime < targetTime)
        targetTime = minTime;
    
#define CHECK(message)                                                         \
    do {                                                                       \
        MEASURE(message);                                                      \
        timecheck(#message " ", minTime, targetTime * 0.65, targetTime * 2.0); \
    } while(0)
    
    CHECK(llret_nop);
    CHECK(stret_nop);
    CHECK(fpret_nop);
    CHECK(vecret_nop);
#if TARGET_OS_OSX
    // lpfret is ~10x slower than other msgSends on Rosetta due to using the
    // x87 stack for returning the value, so don't test it there.
    if (!oah_is_current_process_translated())
#endif
        CHECK(lfpret_nop);

    succeed(TEST_NAME);
}
