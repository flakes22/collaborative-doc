#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <time.h>
#include "protocol.h"

// Maximum number of files tracked
#define MAX_FILES 1024

typedef struct {
    char filename[256];
    long size;
    long word_count;
    time_t created;
    time_t modified;
    time_t last_accessed;
    char last_accessed_by[64];  // username of last accessor
    char owner_username[64];
    char folder[256];
    AclEntryPayload acl[MAX_ACL_ENTRIES];
    int acl_count;
} FileMeta;

// Global in-memory metadata table
extern FileMeta file_table[MAX_FILES];
extern int file_count;

// Load existing metadata.txt from disk into memory
int load_metadata(const char *meta_dir);

// Save current metadata table to metadata.txt
int save_metadata(const char *meta_dir);

// Add new entry
void add_metadata_entry(const char *meta_dir, const char *filename);

// Remove entry by filename
void remove_metadata_entry(const char *meta_dir, const char *filename);

// Update entry when file is written to
void update_metadata_entry(const char *meta_dir, const char *filename);

FileMeta* persist_find_file(const char *filename);
void persist_set_owner(const char *meta_dir, const char *filename, const char *owner);
void persist_set_acl(const char *meta_dir, const char *filename, const char *target_user, PermissionType permission);
void persist_remove_acl(const char *meta_dir, const char *filename, const char *target_user);
void persist_update_last_accessed(const char *meta_dir, const char *filename, const char *username);
void persist_set_folder(const char *meta_dir, const char *filename, const char *foldername);

#endif
