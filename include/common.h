#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>

#define MAX_BUFFER 1024
#define MAX_FILENAME 256

// Default ports (temporary placeholders)
#define NAME_SERVER_PORT 5000
#define STORAGE_SERVER_PORT 6000

// Status / Error Codes
#define OK_200 "OK_200"
#define OK_201 "OK_201"
#define ERR_400 "ERR_400"  // Bad request
#define ERR_401 "ERR_401"  // Unauthorized
#define ERR_404 "ERR_404"  // Not found
#define ERR_500 "ERR_500"  // Internal error

// Component identifiers for logging
typedef enum {
    COMPONENT_CLIENT,
    COMPONENT_STORAGE_SERVER,
    COMPONENT_NAME_SERVER,
    COMPONENT_COMMON
} ComponentType;

// Forward declarations for logging and error helpers
//void log_event(ComponentType type, const char *message);
void handle_error(const char *msg);

// STORAGE
#define STORAGE_BASE "data/storage_servers/ss1"
#define STORAGE_DIR  STORAGE_BASE "/files"
#define META_DIR     STORAGE_BASE "/metadata"
#define LOG_FILE     STORAGE_BASE "/logs/storage_log.txt"

#endif
