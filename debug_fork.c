//
// Created by Wellbin on 2016/12/20.
//

#include <unistd.h>
#include <signal.h>

//#define DEBUG_FORK

pid_t debug_fork() {
#ifdef DEBUG_FORK
    pid_t pid = fork();
    if (0 == pid) {
        kill(0, SIGSTOP);
    }
    return pid;
#else
    return fork();
#endif
}