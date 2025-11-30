#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>

static FILE *global_log = NULL;
static FILE *local_log = NULL;
static char logger_ip[64] = "0.0.0.0";
static int logger_port = 0;
static char logger_username[64] = "N/A";

// Helper to ensure directories exist
static void ensure_directory_exists(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0777) == -1 && errno != EEXIST) {
            perror("Error creating directory");
            exit(EXIT_FAILURE);
        }
    }
}

// Initialize both global and local loggers
void init_logger(const char *ip, int port) {
    strncpy(logger_ip, ip, sizeof(logger_ip) - 1);
    logger_port = port;

    // --- Global logs in ./logs/ ---
    ensure_directory_exists("logs");
    global_log = fopen("logs/server_activity.log", "a");
    if (!global_log) {
        perror("Error opening global log file");
        exit(EXIT_FAILURE);
    }

    // --- Local logs in ./data/storage_servers/ss_<port>/logs/ ---
    ensure_directory_exists("data");
    ensure_directory_exists("data/storage_servers");

    char ss_dir[128];
    snprintf(ss_dir, sizeof(ss_dir), "data/storage_servers/ss_%d", port);
    ensure_directory_exists(ss_dir);

    char logs_dir[256];
    snprintf(logs_dir, sizeof(logs_dir), "%s/logs", ss_dir);
    ensure_directory_exists(logs_dir);

    char local_path[512];
    snprintf(local_path, sizeof(local_path), "%s/server_log.txt", logs_dir);

    local_log = fopen(local_path, "a");
    if (!local_log) {
        perror("Error opening local log file");
        exit(EXIT_FAILURE);
    }

    time_t now = time(NULL);
    // fprintf(global_log, "\n===== Storage Server %d started at %s=====\n", port, ctime(&now));
    // fprintf(local_log, "\n===== Storage Server %d started at %s=====\n", port, ctime(&now));
    fflush(global_log);
    fflush(local_log);
}

// Allow dynamic username update when a client connects
void set_logger_username(const char *username) {
    if (username)
        strncpy(logger_username, username, sizeof(logger_username) - 1);
    else
        strncpy(logger_username, "N/A", sizeof(logger_username) - 1);
}

// Writes a log entry to both global and local log files
void write_log(const char *level, const char *format, ...) {
    time_t now = time(NULL);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    va_list args;

    if (global_log) {
        fprintf(global_log, "[%s] [%s:%d] [USER=%s] [%s] ",
                time_str, logger_ip, logger_port, logger_username, level);
        va_start(args, format);
        vfprintf(global_log, format, args);
        va_end(args);
        fprintf(global_log, "\n");
        fflush(global_log);
    }

    if (local_log) {
        fprintf(local_log, "[%s] [%s:%d] [USER=%s] [%s] ",
                time_str, logger_ip, logger_port, logger_username, level);
        va_start(args, format);
        vfprintf(local_log, format, args);
        va_end(args);
        fprintf(local_log, "\n");
        fflush(local_log);
    }
}

// Local-only logs
void write_local_log(const char *level, const char *format, ...) {
    if (!local_log) return;

    time_t now = time(NULL);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    fprintf(local_log, "[%s] [%s:%d] [USER=%s] [%s] ",
            time_str, logger_ip, logger_port, logger_username, level);

    va_list args;
    va_start(args, format);
    vfprintf(local_log, format, args);
    va_end(args);

    fprintf(local_log, "\n");
    fflush(local_log);
}

// Close logs
void close_logger() {
    time_t now = time(NULL);
    if (global_log) {
       // fprintf(global_log, "===== Logger closed at %s=====\n", ctime(&now));
        fclose(global_log);
        global_log = NULL;
    }
    if (local_log) {
       // fprintf(local_log, "===== Logger closed at %s=====\n", ctime(&now));
        fclose(local_log);
        local_log = NULL;
    }
}
