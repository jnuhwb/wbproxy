//
// Created by Wellbin on 2016/12/22.
//

#ifndef WBPROXY_WBLOG_H
#define WBPROXY_WBLOG_H

typedef enum LogLevel {
    LogLevelInfo,
    LogLevelError
} LogLevel;

void wblogInitContext();
void wblogDestroyContext();

void wblog(char *s);
void wblogf(char *format, ...);
void wbloglf(LogLevel level, char *format, ...);

#endif //WBPROXY_WBLOG_H
