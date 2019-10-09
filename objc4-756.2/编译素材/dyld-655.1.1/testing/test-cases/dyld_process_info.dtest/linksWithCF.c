#include <signal.h>
#include <unistd.h>
#include <mach/mach.h>

int main()
{
    (void)kill(getpid(), SIGSTOP);
    return 0;
}

