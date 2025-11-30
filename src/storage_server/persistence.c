#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "../../include/persistence.h"

FileMeta file_table[MAX_FILES];
int file_count = 0;

// Internal helper to get file size
static long get_file_size(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fclose(f);
    return sz;
}

// Internal helper to count words in a file
static long count_words_in_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    
    long word_count = 0;
    int c;
    int in_word = 0;
    
    while ((c = fgetc(f)) != EOF) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_word) {
                word_count++;
                in_word = 0;
            }
        } else {
            in_word = 1;
        }
    }
    
    // Count last word if file doesn't end with whitespace
    if (in_word) {
        word_count++;
    }
    
    fclose(f);
    return word_count;
}

int load_metadata(const char *meta_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/metadata.txt", meta_dir);

    FILE *f = fopen(path, "r");
    if (!f) return 0; // no metadata yet

    file_count = 0;
    // New metadata format per-line:
    // filename,size,word_count,created,modified,last_accessed,last_accessed_by,owner,acl_count,acl_entries
    // where acl_entries is of the form: user1:perm;user2:perm;... (semicolon separated)
    char line[2048];
    while (fgets(line, sizeof(line), f) && file_count < MAX_FILES) {
        // Trim newline
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';

        // Use strtok to split by commas
        char *saveptr = NULL;
        char *tok = strtok_r(line, ",", &saveptr);
        if (!tok) continue;
        strncpy(file_table[file_count].filename, tok, 255);

        tok = strtok_r(NULL, ",", &saveptr);
        if (!tok) continue;
        file_table[file_count].size = atol(tok);

        tok = strtok_r(NULL, ",", &saveptr);
        if (!tok) continue;
        file_table[file_count].word_count = atol(tok);

        tok = strtok_r(NULL, ",", &saveptr);
        if (!tok) continue;
        file_table[file_count].created = (time_t)atol(tok);

        tok = strtok_r(NULL, ",", &saveptr);
        if (!tok) continue;
        file_table[file_count].modified = (time_t)atol(tok);

        tok = strtok_r(NULL, ",", &saveptr);
        if (!tok) continue;
        file_table[file_count].last_accessed = (time_t)atol(tok);

        // last_accessed_by
        tok = strtok_r(NULL, ",", &saveptr);
        if (tok && strcmp(tok, "-") != 0) {
            strncpy(file_table[file_count].last_accessed_by, tok, 64 - 1);
        } else {
            file_table[file_count].last_accessed_by[0] = '\0';
        }

        // Owner
        tok = strtok_r(NULL, ",", &saveptr);
        if (tok && strcmp(tok, "-") != 0) {
            strncpy(file_table[file_count].owner_username, tok, 64 - 1);
        } else {
            file_table[file_count].owner_username[0] = '\0';
        }

        // Folder (new field)
        tok = strtok_r(NULL, ",", &saveptr);
        if (tok && strcmp(tok, "-") != 0) {
            strncpy(file_table[file_count].folder, tok, 255);
        } else {
            file_table[file_count].folder[0] = '\0';
        }

        // ACL count
        tok = strtok_r(NULL, ",", &saveptr);
        int acl_count = 0;
        if (tok) acl_count = atoi(tok);
        file_table[file_count].acl_count = 0;

        // ACL entries (rest of the line)
        tok = strtok_r(NULL, "", &saveptr); // get remaining string (may be NULL)
        if (tok && acl_count > 0) {
            // tok contains something like: user1:1;user2:2;...
            char *acl_save = NULL;
            char *acl_tok = strtok_r(tok, ";", &acl_save);
            while (acl_tok && file_table[file_count].acl_count < MAX_ACL_ENTRIES) {
                char *sep = strchr(acl_tok, ':');
                if (sep) {
                    *sep = '\0';
                    const char *uname = acl_tok;
                    int perm = atoi(sep + 1);
                    strncpy(file_table[file_count].acl[file_table[file_count].acl_count].username, uname, 64 - 1);
                    file_table[file_count].acl[file_table[file_count].acl_count].permission = perm;
                    file_table[file_count].acl_count++;
                }
                acl_tok = strtok_r(NULL, ";", &acl_save);
            }
        }

        file_count++;
    }
    fclose(f);
    printf("[INFO] Loaded %d metadata entries from %s\n", file_count, path);
    return file_count;
}

int save_metadata(const char *meta_dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/metadata.txt", meta_dir);

    FILE *f = fopen(path, "w");
    if (!f) {
        perror("save_metadata fopen");
        return -1;
    }
    for (int i = 0; i < file_count; i++) {
        // Write: filename,size,word_count,created,modified,last_accessed,last_accessed_by,owner,acl_count,acl_entries
        fprintf(f, "%s,%ld,%ld,%ld,%ld,%ld,",
                file_table[i].filename,
                file_table[i].size,
                file_table[i].word_count,
                (long)file_table[i].created,
                (long)file_table[i].modified,
                (long)file_table[i].last_accessed);
        
        if (file_table[i].last_accessed_by[0] != '\0') {
            fprintf(f, "%s,", file_table[i].last_accessed_by);
        } else {
            fprintf(f, "-,");
        }

        if (file_table[i].owner_username[0] != '\0') {
            fprintf(f, "%s,", file_table[i].owner_username);
        } else {
            fprintf(f, "-,");
        }
        // Folder
        if (file_table[i].folder[0] != '\0') {
            fprintf(f, "%s,", file_table[i].folder);
        } else {
            fprintf(f, "-,");
        }
        
        fprintf(f, "%d,", file_table[i].acl_count);

        // ACL entries as user:perm;user:perm;...
        for (int j = 0; j < file_table[i].acl_count; j++) {
            fprintf(f, "%s:%d;", file_table[i].acl[j].username, file_table[i].acl[j].permission);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    return 0;
}

void add_metadata_entry(const char *meta_dir, const char *filename) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(file_table[i].filename, filename) == 0) return; // already exists
    }
    if (file_count >= MAX_FILES) return;

    strncpy(file_table[file_count].filename, filename, 255);
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/../files/%s", meta_dir, filename);
    file_table[file_count].size = get_file_size(filepath);
    file_table[file_count].word_count = count_words_in_file(filepath);
    time_t now = time(NULL);
    file_table[file_count].created = now;
    file_table[file_count].modified = now;
    file_table[file_count].last_accessed = now;
    file_table[file_count].last_accessed_by[0] = '\0';
    file_table[file_count].owner_username[0] = '\0';
    file_table[file_count].folder[0] = '\0';
    file_table[file_count].acl_count = 0;
    file_count++;
    save_metadata(meta_dir);
}

void remove_metadata_entry(const char *meta_dir, const char *filename) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(file_table[i].filename, filename) == 0) {
            for (int j = i; j < file_count - 1; j++)
                file_table[j] = file_table[j + 1];
            file_count--;
            save_metadata(meta_dir);
            return;
        }
    }
}

void update_metadata_entry(const char *meta_dir, const char *filename) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(file_table[i].filename, filename) == 0) {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/../files/%s", meta_dir, filename);
            file_table[i].size = get_file_size(filepath);
            file_table[i].word_count = count_words_in_file(filepath);
            file_table[i].modified = time(NULL);
            save_metadata(meta_dir);
            return;
        }
    }
}

/**
 * @brief Update last accessed time and user for a file.
 */
void persist_update_last_accessed(const char *meta_dir, const char *filename, const char *username) {
    FileMeta* file = persist_find_file(filename);
    if (file) {
        file->last_accessed = time(NULL);
        if (username) {
            strncpy(file->last_accessed_by, username, 64 - 1);
        }
        save_metadata(meta_dir);
    }
}


/**
 * @brief Finds a pointer to a FileMeta struct in the global table.
 * @return Pointer to the struct, or NULL if not found.
 */
FileMeta* persist_find_file(const char *filename) {
    for (int i = 0; i < file_count; i++) {
        if (strcmp(file_table[i].filename, filename) == 0) {
            return &file_table[i];
        }
    }
    return NULL;
}

/**
 * @brief Sets the owner of a file and saves.
 */
void persist_set_owner(const char *meta_dir, const char *filename, const char *owner) {
    FileMeta* file = persist_find_file(filename);
    if (file) {
        strncpy(file->owner_username, owner, 64 - 1);
        save_metadata(meta_dir);
    }
}

void persist_set_folder(const char *meta_dir, const char *filename, const char *foldername) {
    FileMeta* file = persist_find_file(filename);
    if (file) {
        if (foldername && strlen(foldername) > 0)
            strncpy(file->folder, foldername, 255);
        else
            file->folder[0] = '\0';
        save_metadata(meta_dir);
    }
}

/**
 * @brief Adds or updates an ACL entry for a file and saves.
 */
void persist_set_acl(const char *meta_dir, const char *filename, const char *target_user, PermissionType permission) {
    FileMeta* file = persist_find_file(filename);
    if (!file) return;

    // Check if user is already in ACL
    for (int i = 0; i < file->acl_count; i++) {
        if (strcmp(file->acl[i].username, target_user) == 0) {
            file->acl[i].permission = permission; // Update existing
            save_metadata(meta_dir);
            return;
        }
    }

    // Add as new entry (if space)
    if (file->acl_count < MAX_ACL_ENTRIES) {
        int i = file->acl_count;
        strncpy(file->acl[i].username, target_user, 64 - 1);
        file->acl[i].permission = permission;
        file->acl_count++;
        save_metadata(meta_dir);
    }
}

/**
 * @brief Removes a user from a file's ACL and saves.
 */
void persist_remove_acl(const char *meta_dir, const char *filename, const char *target_user) {
    FileMeta* file = persist_find_file(filename);
    if (!file) return;

    int found_index = -1;
    for (int i = 0; i < file->acl_count; i++) {
        if (strcmp(file->acl[i].username, target_user) == 0) {
            found_index = i;
            break;
        }
    }

    if (found_index != -1) {
        // Swap with the last ACL entry
        file->acl[found_index] = file->acl[file->acl_count - 1];
        file->acl_count--;
        save_metadata(meta_dir);
    }
}