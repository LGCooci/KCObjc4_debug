
// BUILD:  $CC linksWithCF.c  -o $BUILD_DIR/linksWithCF.exe -framework CoreFoundation
// BUILD:  $CC main.c         -o $BUILD_DIR/dyld_process_info.exe
// BUILD:  $TASK_FOR_PID_ENABLE  $BUILD_DIR/dyld_process_info.exe

// RUN:  $SUDO ./dyld_process_info.exe $RUN_DIR/linksWithCF.exe

#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <mach/mach.h>
#include <mach/machine.h>
#include <mach-o/dyld_process_info.h>
#include <Availability.h>


extern char** environ;

#if __x86_64__
    cpu_type_t otherArch[] = { CPU_TYPE_I386 };
#elif __i386__
    cpu_type_t otherArch[] = { CPU_TYPE_X86_64 };
#elif __arm64__
    cpu_type_t otherArch[] = { CPU_TYPE_ARM };
#elif __arm__
    cpu_type_t otherArch[] = { CPU_TYPE_ARM64 };
#endif

struct task_and_pid {
    pid_t pid;
    task_t task;
};

static struct task_and_pid launchTest(const char* testProgPath, bool launchOtherArch, bool launchSuspended)
{
    posix_spawnattr_t attr = 0;
    if ( posix_spawnattr_init(&attr) != 0 ) {
        printf("[FAIL] dyld_process_info posix_spawnattr_init()\n");
        exit(0);
    }
    if ( launchSuspended ) {
        if ( posix_spawnattr_setflags(&attr, POSIX_SPAWN_START_SUSPENDED) != 0 ) {
            printf("[FAIL] dyld_process_info POSIX_SPAWN_START_SUSPENDED\n");
            exit(0);
        }
    }
    if ( launchOtherArch ) {
        size_t copied;
        if ( posix_spawnattr_setbinpref_np(&attr, 1, otherArch, &copied) != 0 ) {
           printf("[FAIL] dyld_process_info posix_spawnattr_setbinpref_np()\n");
            exit(0);
        }
    }

    struct task_and_pid child = {0, 0};
    const char* argv[] = { testProgPath, NULL };
    int psResult = posix_spawn(&child.pid, testProgPath, NULL, &attr, (char**)argv, environ);
    if ( psResult != 0 ) {
        printf("[FAIL] dyld_process_info posix_spawn(%s) failed, err=%d\n", testProgPath, psResult);
        exit(0);
    }
    if (posix_spawnattr_destroy(&attr) != 0) {
        printf("[FAIL] dyld_process_info posix_spawnattr_destroy()\n");
        exit(0);
    }

    if ( task_for_pid(mach_task_self(), child.pid, &child.task) != KERN_SUCCESS ) {
        printf("[FAIL] dyld_process_info task_for_pid()\n");
        kill(child.pid, SIGKILL);
        exit(0);
    }

#if __x86_64__
    //printf("child pid=%d task=%d (%s, %s)\n", child.pid, child.task, launchOtherArch ? "i386" : "x86_64",  launchSuspended ? "suspended" : "active");
#endif

    // wait until process is up and has suspended itself
    struct task_basic_info info;
    do {
        unsigned count = TASK_BASIC_INFO_COUNT;
        kern_return_t kr = task_info(child.task, TASK_BASIC_INFO, (task_info_t)&info, &count);
        sleep(1);
    } while ( info.suspend_count == 0 );

    return child;
}

static void killTest(struct task_and_pid tp) {
    int r = kill(tp.pid, SIGKILL);
    waitpid(tp.pid, &r, 0);
}

static bool hasCF(task_t task, bool launchedSuspended)
{
    kern_return_t result;
    dyld_process_info info = _dyld_process_info_create(task, 0, &result);
    if ( info == NULL ) {
        printf("[FAIL] dyld_process_info _dyld_process_info_create(), kern_return_t=%d\n", result);
        return false;
    }

    dyld_process_state_info stateInfo;
    _dyld_process_info_get_state(info, &stateInfo);
    bool valueSaysLaunchedSuspended = (stateInfo.dyldState == dyld_process_state_not_started);
    if ( valueSaysLaunchedSuspended != launchedSuspended ) {
        printf("[FAIL] dyld_process_info suspend state mismatch\n");
        _dyld_process_info_release(info);
        return false;
    }

    __block bool foundDyld = false;
    _dyld_process_info_for_each_image(info, ^(uint64_t machHeaderAddress, const uuid_t uuid, const char* path) {
        //fprintf(stderr, "0x%llX %s\n", machHeaderAddress, path);
        if ( strstr(path, "/dyld") != NULL )
            foundDyld = true;
    });

    if ( launchedSuspended ) {
        // fprintf(stderr, "launched suspended image list:\n");
        __block bool foundMain = false;
        _dyld_process_info_for_each_image(info, ^(uint64_t machHeaderAddress, const uuid_t uuid, const char* path) {
            //fprintf(stderr, "0x%llX %s\n", machHeaderAddress, path);
            if ( strstr(path, "/linksWithCF.exe") != NULL )
                foundMain = true;
       });
        _dyld_process_info_release(info);
        return foundMain && foundDyld;
    }

    __block bool foundCF = false;
    _dyld_process_info_for_each_image(info, ^(uint64_t machHeaderAddress, const uuid_t uuid, const char* path) {
        //fprintf(stderr, "0x%llX %s\n", machHeaderAddress, path);
        if ( strstr(path, "/CoreFoundation.framework/") != NULL )
            foundCF = true;
    });

    _dyld_process_info_release(info);

    return foundCF && foundDyld;
}

static void checkForLeaks(const char *name) {
    printf("[BEGIN] %s checkForLeaks\n", name);
    pid_t child;
    int stat_loc;
    char buffer[PAGE_SIZE];
    (void)snprintf(&buffer[0], 128, "%d", getpid());

    const char* argv[] = { "/usr/bin/leaks", buffer, NULL };
    int psResult = posix_spawn(&child, "/usr/bin/leaks", NULL, NULL, (char**)argv, environ);
    if ( psResult != 0 ) {
        printf("[FAIL] %s checkForLeaks posix_spawn failed, err=%d\n", name, psResult);
        exit(0);
    }

    (void)wait4(child, &stat_loc, 0, NULL);
    ssize_t readBytes = 0;
    if (WIFEXITED(stat_loc) == 0) {
        printf("[FAIL] %s checkForLeaks leaks did not exit\n", name);
        exit(0);
    }
    if (WEXITSTATUS(stat_loc) == 1) {
        printf("[FAIL] %s checkForLeaks found leaks\n", name);
        exit(0);
    }
    if (WEXITSTATUS(stat_loc) != 0) {
        printf("[FAIL] %s checkForLeaks leaks errored out\n", name);
        exit(0);
    }
    printf("[PASS] %s checkForLeaks\n", name);
}


int main(int argc, const char* argv[])
{
    kern_return_t kr = KERN_SUCCESS;
    printf("[BEGIN] dyld_process_info\n");

    if ( argc < 2 ) {
        printf("[FAIL] dyld_process_info missing argument\n");
        exit(0);
    }

    const char* testProgPath = argv[1];
    struct task_and_pid child;

    // launch test program same arch as this program
    child = launchTest(testProgPath, false, false);
    if ( ! hasCF(child.task, false) ) {
        printf("[FAIL] dyld_process_info same arch does not link with CF and dyld\n");
        killTest(child);
        exit(0);
    }
    killTest(child);

    // launch test program suspended
    child = launchTest(testProgPath, false, true);
    if ( ! hasCF(child.task, true) ) {
        printf("[FAIL] dyld_process_info suspended does not link with CF and dyld\n");
        killTest(child);
        exit(0);
    }
    (void)kill(child.pid, SIGCONT);
    killTest(child);

#if __MAC_OS_X_VERSION_MIN_REQUIRED
    // only mac supports multiple architectures, run test program as other arch too
    child = launchTest(testProgPath, true, false);
    if ( ! hasCF(child.task, false) ) {
        printf("[FAIL] dyld_process_info other arch does not link with CF and dyld\n");
        killTest(child);
        exit(0);
    }
    killTest(child);

    // launch test program suspended
    child = launchTest(testProgPath, true, true);
    if ( ! hasCF(child.task, true) ) {
        printf("[FAIL] dyld_process_info suspended does not link with CF and dyld\n");
        killTest(child);
        exit(0);
    }
    (void)kill(child.pid, SIGCONT);
    killTest(child);
#endif

    // verify this program does not use CF
    if ( hasCF(mach_task_self(), false) ) {
        printf("[FAIL] dyld_process_info self links with CF and dyld\n");
        exit(0);
    }

    printf("[PASS] dyld_process_info\n");
    checkForLeaks("dyld_process_info");

	return 0;
}
