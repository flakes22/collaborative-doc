#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>

void init_logger(const char *ip, int port);
void set_logger_username(const char *username);
void write_log(const char *level, const char *format, ...);
void write_local_log(const char *level, const char *format, ...);
void close_logger();

#endif
