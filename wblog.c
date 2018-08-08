//
// Created by Wellbin on 2016/12/22.
//

#include "wblog.h"
#include <sys/types.h>
#include <time.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <pthread.h>

#ifndef WIN32
#include <sys/errno.h>
#endif

#define LOG_BUF_SIZE (10240)
#define LOG_MAX_PATH_SIZE (128)

#define LOG_LEVEL_INFO
#define LOG_LEVEL_ERROR

static pthread_mutex_t lock;

void wblogInitContext() {
    pthread_mutex_init(&lock, NULL);
}

void wblogDestroyContext() {
    pthread_mutex_destroy(&lock);
}

void wblog(char *s) {
    char day[128];
    char daytime[128];
    time_t now;
    struct tm *ti;

    time(&now);
    ti = localtime(&now);
    strftime(day, 128, "%Y-%m-%d", ti);
    strftime(daytime, 128, "%Y-%m-%d %H:%M:%S", ti);

    printf("[pid:%d] %s %s\n", getpid(), daytime, s);

#ifdef WIN32
    mkdir("log");
#else
    if (access("log", F_OK) == -1) {//directory's not exist
        if (mkdir("log", S_IRWXU) != 0) {
            printf("make log directory error! errno=%d\n", errno);
        }
    }
#endif

    char logPath[LOG_MAX_PATH_SIZE];
    memset(logPath, 0, LOG_MAX_PATH_SIZE);
    strcat(logPath, "./log/");
    strcat(logPath, day);
    strcat(logPath, ".log");

    pthread_mutex_lock(&lock);
    FILE *f = fopen(logPath, "ab+");
    if (!f) {
        printf("open log file error\n");
        return;
    }

    fprintf(f, "[pid:%d] %s %s\n", getpid(), daytime, s);
    fflush(f);
    fclose(f);
    pthread_mutex_unlock(&lock);
}


void wblogf(char *format, ...) {
    char buf[LOG_BUF_SIZE];
    memset(buf, 0, LOG_BUF_SIZE);

    va_list args;
    va_start(args, format);
    vsnprintf(buf, LOG_BUF_SIZE, format, args);
    va_end(args);

    wblog(buf);
}

void wbloglf(LogLevel level, char *format, ...) {
    char *levelStr = NULL;
    switch (level) {
        case LogLevelInfo:
            levelStr = "[info]";
            break;
        case LogLevelError:
            levelStr = "[error]";
            break;

        default:
            break;
    }

    char buf[LOG_BUF_SIZE];
    memset(buf, 0, LOG_BUF_SIZE);

    va_list args;
    va_start(args, format);
    vsnprintf(buf, LOG_BUF_SIZE, format, args);
    va_end(args);

    char logStr[strlen(levelStr) + LOG_BUF_SIZE];
    strcpy(logStr, levelStr);
    strcat(logStr, buf);
    wblog(logStr);
}
