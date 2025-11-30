#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#include "../../include/common.h"
#include "../../include/storage_server.h"
#include "../../include/persistence.h"
#include "../../include/logger.h"

// Create directory recursively if it doesn't exist
void create_dir_if_not_exists(const char *path) {
    struct stat st = {0};
    if (stat(path, &st) == -1) {
        if (mkdir(path, 0777) == -1) {
            perror("mkdir failed");
            exit(EXIT_FAILURE);
        }
    }
}

// Initialize the storage server directory structure and load metadata
int init_storage_server(int port) {
    char base_path[512];        // Increased from 256
    snprintf(base_path, sizeof(base_path), "data/ss_%d", port);
    
    char files_path[512];       // Increased from 256
    char meta_path[512];        // Increased from 256
    char undo_dir[512];         // Increased from 256
    char versions_dir[512];     // Increased from 256
    char access_requests_dir[512];  // Increased from 256
    char checkpoints_dir[512];      // Increased from 256
    
    snprintf(files_path, sizeof(files_path), "%s/files", base_path);
    snprintf(meta_path, sizeof(meta_path), "%s/metadata", base_path);
    snprintf(undo_dir, sizeof(undo_dir), "%s/undo", base_path);
    snprintf(versions_dir, sizeof(versions_dir), "%s/versions", base_path);
    snprintf(access_requests_dir, sizeof(access_requests_dir), "%s/access_requests", base_path);
    snprintf(checkpoints_dir, sizeof(checkpoints_dir), "%s/checkpoints", base_path);
    
    // Create directories (including new ones for access requests and checkpoints)
    mkdir(base_path, 0755);
    mkdir(files_path, 0755);
    mkdir(meta_path, 0755);
    mkdir(undo_dir, 0755);
    mkdir(versions_dir, 0755);
    mkdir(access_requests_dir, 0755);  // Add this
    mkdir(checkpoints_dir, 0755);      // Add this

    // ---- Logging ----
    printf("[INFO] Storage server directory initialized at: %s\n", base_path);
    printf("[INFO] Undo system initialized with versions directory: %s\n", versions_dir);
    printf("[INFO] Access requests directory initialized: %s\n", access_requests_dir);  // Add this
    printf("[INFO] Checkpoints directory initialized: %s\n", checkpoints_dir);          // Add this

    FILE *log = fopen("server.log", "a");
    if (log) {
        fprintf(log, "[INFO] Storage server directory initialized at: %s\n", base_path);
        fprintf(log, "[INFO] Undo system initialized with versions directory: %s\n", versions_dir);
        fprintf(log, "[INFO] Access requests directory initialized: %s\n", access_requests_dir);  // Add this
        fprintf(log, "[INFO] Checkpoints directory initialized: %s\n", checkpoints_dir);          // Add this
        fclose(log);
    } else {
        perror("server.log open failed");
    }

    // Create checkpoint metadata directory as well
    char checkpoint_meta_dir[512];  // Increased from 256
    snprintf(checkpoint_meta_dir, sizeof(checkpoint_meta_dir), "%s/checkpoint_meta", base_path);
    mkdir(checkpoint_meta_dir, 0755);

    // ---- Metadata Persistence ----
    int loaded = load_metadata(meta_path);
    if (loaded > 0) {
        printf("[INFO] Loaded %d metadata entries from %s/metadata.txt\n", loaded, meta_path);

        FILE *log2 = fopen("server.log", "a");
        if (log2) {
            fprintf(log2, "[INFO] Loaded %d metadata entries from %s/metadata.txt\n", loaded, meta_path);
            fclose(log2);
        }
    } else {
        printf("[INFO] No previous metadata found in %s/metadata.txt — starting fresh.\n", meta_path);

        FILE *log3 = fopen("server.log", "a");
        if (log3) {
            fprintf(log3, "[INFO] No previous metadata found in %s/metadata.txt — starting fresh.\n", meta_path);
            fclose(log3);
        }
    }

    return 0;
}

void close_storage_server(void) {
    // Cleanup function - can be empty for now
    return;
}
