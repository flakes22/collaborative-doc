#include "search.h"
#include "logger.h"
#include <string.h>
#include <stdlib.h> // For malloc/free
#include <time.h>   // For strftime and localtime
#include "cache.h"
#include "storage_manager.h"
#include "protocol.h"
#include "socket_utils.h"
// --- Trie Implementation ---

static TrieNode* root;
static pthread_mutex_t trie_mutex;

// -------------------- Folder registry --------------------
#define MAX_FOLDERS 1024
typedef struct {
    char foldername[MAX_FILENAME];
    char owner_username[64];
} FolderRecord;

static FolderRecord folder_registry[MAX_FOLDERS];
static int folder_count = 0;

// --- New functions for VIEW command ---

// We need a helper struct to pass data through the recursion
typedef struct {
    const char* username;
    int flags;
    char* buffer;
    int buffer_size;
    int current_offset;
} ViewTraversalData;

// Small helper to collect filenames + ss_index for metadata refresh
typedef struct {
    char filename[MAX_FILENAME];
    int ss_index;
} FileEntry;

static void collect_files_recursive(TrieNode* node, FileEntry* entries, int* count, int max_count) {
    if (!node || *count >= max_count) return;
    if (node->file_info) {
        strncpy(entries[*count].filename, node->file_info->filename, MAX_FILENAME - 1);
        entries[*count].ss_index = node->file_info->ss_index;
        (*count)++;
    }
    for (int i = 0; i < TRIE_CHAR_SET_SIZE; i++) {
        if (node->children[i]) collect_files_recursive(node->children[i], entries, count, max_count);
    }
}

// Update a file's metadata in the trie safely (locks trie_mutex)
static void search_update_file_metadata(const char* filename, SSMetadataPayload* meta) {
    pthread_mutex_lock(&trie_mutex);
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        int index = (int)filename[i];
        if (index < 0 || index >= TRIE_CHAR_SET_SIZE) continue;
        if (current->children[index] == NULL) { current = NULL; break; }
        current = current->children[index];
    }
    if (current && current->file_info) {
        current->file_info->word_count = meta->word_count;
        current->file_info->char_count = meta->char_count;
        current->file_info->last_accessed = meta->last_accessed;
        current->file_info->modified = meta->last_modified;
        strncpy(current->file_info->last_accessed_by, meta->last_accessed_by, 64 - 1);
    }
    pthread_mutex_unlock(&trie_mutex);
}

/**
 * @brief The recursive part of the file list traversal.
 * Walks the Trie from 'node' downwards.
 */
static void traverse_for_view(TrieNode* node, ViewTraversalData* data) {
    if (node == NULL) {
        return;
    }

    // Check if the current node is a file
    FileRecord* file = node->file_info;
    if (file != NULL) {
        // This node represents a file. Check permissions.
        int has_permission = 0;
        
        if (data->flags & VIEW_FLAG_ALL) {
            has_permission = 1; // -a flag, list all files
        } else {
            // No -a flag, check permission
            // We can't call search_check_permission directly
            // because it locks the mutex we are already holding.
            
            // 1. Check owner
            if (strcmp(file->owner_username, data->username) == 0) {
                has_permission = 1;
            } else {
                // 2. Check ACL
                for (int i = 0; i < file->acl_count; i++) {
                    if (strcmp(file->acl[i].username, data->username) == 0 &&
                        file->acl[i].permission >= PERM_READ) {
                        has_permission = 1;
                        break;
                    }
                }
            }
        }
        
        // If we have permission, add it to the list
        if (has_permission) {
            int chars_written = 0;
            if (data->flags & VIEW_FLAG_LONG) {
                // -l flag: format as a table row
                // Format: | filename | word_count | char_count | last_access_time | owner |
                char time_str[30];
                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", 
                         localtime(&file->last_accessed));
                
                // Include TYPE column: 'F' for file
                chars_written = snprintf(data->buffer + data->current_offset,
                                         data->buffer_size - data->current_offset,
                                         "| F | %-10s | %5ld | %5ld | %16s | %-5s |\n",
                                         file->filename, file->word_count, file->char_count,
                                         time_str, file->owner_username);
            } else {
                // Regular format: just filename with arrow
                chars_written = snprintf(data->buffer + data->current_offset,
                                         data->buffer_size - data->current_offset,
                                         "--> %s\n", file->filename);
            }

            if (data->current_offset + chars_written >= data->buffer_size) {
                write_log("ERROR", "[SEARCH_VIEW] File list buffer too small!");
                // Stop recursion by not updating offset
                return;
            }
            data->current_offset += chars_written;
        }
    }

    // Continue recursion for all children
    for (int i = 0; i < TRIE_CHAR_SET_SIZE; i++) {
        if (data->current_offset >= data->buffer_size) {
            return; // Buffer is full
        }
        traverse_for_view(node->children[i], data);
    }
}

/**
 * @brief Public API function to get the file list.
 */
int search_get_file_list(const char* username, int flags, char* out_buffer, int buffer_size) {
    memset(out_buffer, 0, buffer_size);

    ViewTraversalData data;
    data.username = username;
    data.flags = flags;
    data.buffer = out_buffer;
    data.buffer_size = buffer_size;
    data.current_offset = 0;

    // If -l flag requested, refresh metadata from Storage Servers first.
    if (flags & VIEW_FLAG_LONG) {
        // Collect file list (filenames + ss_index) while holding trie lock
        int max_files = MAX_STORAGE_SERVERS * MAX_FILES_PER_SERVER;
        FileEntry* entries = malloc(sizeof(FileEntry) * max_files);
        int entry_count = 0;
        if (entries) {
            pthread_mutex_lock(&trie_mutex);
            collect_files_recursive(root, entries, &entry_count, max_files);
            pthread_mutex_unlock(&trie_mutex);

            // For each file, query corresponding SS for metadata and update trie
            for (int i = 0; i < entry_count; i++) {
                FileEntry *e = &entries[i];
                StorageServerInfo* ss = get_ss_by_index(e->ss_index);
                if (ss == NULL || !ss->is_active) continue;

                // Build internal metadata request
                MessageHeader meta_req;
                memset(&meta_req, 0, sizeof(meta_req));
                meta_req.msg_type = MSG_INTERNAL_GET_METADATA;
                meta_req.source_component = COMPONENT_NAME_SERVER;
                strncpy(meta_req.filename, e->filename, MAX_FILENAME - 1);

                SSMetadataPayload meta_payload;
                memset(&meta_payload, 0, sizeof(meta_payload));

                // Contact SS to get metadata
                write_log("DEBUG", "[VIEW_REFRESH] Refreshing metadata for '%s' from SS %d", e->filename, e->ss_index);
                pthread_mutex_lock(&ss->socket_mutex);
                int ss_sock = ss->ss_socket_fd;
                if (send_header(ss_sock, &meta_req) == 0) {
                    MessageHeader resp;
                    if (recv_header(ss_sock, &resp) == 0 && resp.msg_type == MSG_INTERNAL_METADATA_RESP) {
                        if (recv_all(ss_sock, &meta_payload, sizeof(meta_payload)) == 0) {
                            // Update trie record with fresh metadata
                            search_update_file_metadata(e->filename, &meta_payload);
                            write_log("DEBUG", "[VIEW_REFRESH] Got metadata for '%s' (words=%ld, chars=%ld)",
                                      e->filename, meta_payload.word_count, meta_payload.char_count);
                        } else {
                            write_log("WARN", "[VIEW_REFRESH] Failed to recv metadata payload for '%s' from SS %d",
                                      e->filename, e->ss_index);
                        }
                    } else {
                        write_log("WARN", "[VIEW_REFRESH] Bad metadata response header for '%s' from SS %d",
                                  e->filename, e->ss_index);
                    }
                } else {
                    write_log("WARN", "[VIEW_REFRESH] Failed to send metadata request for '%s' to SS %d",
                              e->filename, e->ss_index);
                }
                pthread_mutex_unlock(&ss->socket_mutex);
            }
            free(entries);
        }
    }

    // Instead of traversing the entire trie, VIEW should list the immediate
    // top-level entries (folders and files in root). Build that list here.
    pthread_mutex_lock(&trie_mutex);

    // 1) Add top-level folders (those without a '/')
    for (int i = 0; i < folder_count; i++) {
        const char* fname = folder_registry[i].foldername;
        if (strchr(fname, '/') == NULL) {
            int chars_written = 0;
            if (data.flags & VIEW_FLAG_LONG) {
                // TYPE D for directory. table: | D | name | - | - | - | owner |
                chars_written = snprintf(data.buffer + data.current_offset,
                                         data.buffer_size - data.current_offset,
                                         "| D | %-10s | %5s | %5s | %16s | %-5s |\n",
                                         fname, "-", "-", "-", folder_registry[i].owner_username);
            } else {
                chars_written = snprintf(data.buffer + data.current_offset,
                                         data.buffer_size - data.current_offset,
                                         "[D] %s\n", fname);
            }
            if (data.current_offset + chars_written >= data.buffer_size) {
                write_log("ERROR", "[SEARCH_VIEW] File list buffer too small when adding folders!");
                pthread_mutex_unlock(&trie_mutex);
                return data.current_offset;
            }
            data.current_offset += chars_written;
        }
    }

    // 2) Add files that are in the root (file->folder is empty)
    {
        TrieNode* stack[4096]; int sp = 0; stack[sp++] = root;
        while (sp > 0) {
            TrieNode* node = stack[--sp];
            if (node->file_info) {
                FileRecord* file = node->file_info;
                const char* ffolder = file->folder;
                if (ffolder == NULL || ffolder[0] == '\0') {
                    int has_permission = 0;
                    if (data.flags & VIEW_FLAG_ALL) has_permission = 1;
                    else if (strcmp(file->owner_username, data.username) == 0) has_permission = 1;
                    else {
                        for (int a = 0; a < file->acl_count; a++) {
                            if (strcmp(file->acl[a].username, data.username) == 0 && file->acl[a].permission >= PERM_READ) { has_permission = 1; break; }
                        }
                    }
                    if (has_permission) {
                        int chars_written = 0;
                        if (data.flags & VIEW_FLAG_LONG) {
                            char time_str[30]; strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", localtime(&file->last_accessed));
                            chars_written = snprintf(data.buffer + data.current_offset, data.buffer_size - data.current_offset,
                                                     "| F | %-10s | %5ld | %5ld | %16s | %-5s |\n",
                                                     file->filename, file->word_count, file->char_count,
                                                     time_str, file->owner_username);
                        } else {
                            chars_written = snprintf(data.buffer + data.current_offset, data.buffer_size - data.current_offset, "--> %s\n", file->filename);
                        }
                        if (data.current_offset + chars_written >= data.buffer_size) {
                            write_log("ERROR", "[SEARCH_VIEW] File list buffer too small when adding files!");
                            pthread_mutex_unlock(&trie_mutex);
                            return data.current_offset;
                        }
                        data.current_offset += chars_written;
                    }
                }
            }
            for (int i = 0; i < TRIE_CHAR_SET_SIZE; i++) if (node->children[i]) stack[sp++] = node->children[i];
        }
    }

    pthread_mutex_unlock(&trie_mutex);

    return data.current_offset; // Total bytes written
}

// -------------------- Folder helpers --------------------

static int starts_with(const char* s, const char* prefix) {
    if (!s || !prefix) return 0;
    size_t lp = strlen(prefix);
    if (lp == 0) return 1;
    return strncmp(s, prefix, lp) == 0;
}

int search_add_folder(const char* foldername, const char* owner_username) {
    if (!foldername || strlen(foldername) == 0) return -1;
    pthread_mutex_lock(&trie_mutex);
    for (int i = 0; i < folder_count; i++) {
        if (strcmp(folder_registry[i].foldername, foldername) == 0) {
            pthread_mutex_unlock(&trie_mutex);
            return -1; // already exists
        }
    }
    if (folder_count >= MAX_FOLDERS) {
        pthread_mutex_unlock(&trie_mutex);
        return -1;
    }
    strncpy(folder_registry[folder_count].foldername, foldername, MAX_FILENAME - 1);
    strncpy(folder_registry[folder_count].owner_username, owner_username, 64 - 1);
    folder_count++;
    pthread_mutex_unlock(&trie_mutex);
    write_log("SEARCH", "Added folder '%s' (owner=%s)", foldername, owner_username);
    return 0;
}

int search_find_folder(const char* foldername) {
    if (!foldername) return -1;
    int idx = -1;
    pthread_mutex_lock(&trie_mutex);
    for (int i = 0; i < folder_count; i++) {
        if (strcmp(folder_registry[i].foldername, foldername) == 0) { idx = i; break; }
    }
    pthread_mutex_unlock(&trie_mutex);
    return idx;
}

int search_set_file_folder(const char* filename, const char* foldername, const char* owner_username) {
    if (!filename) return -1;
    pthread_mutex_lock(&trie_mutex);
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        int index = (int)filename[i];
        if (index < 0 || index >= TRIE_CHAR_SET_SIZE || current->children[index] == NULL) {
            pthread_mutex_unlock(&trie_mutex);
            return -1; // Not found
        }
        current = current->children[index];
    }
    if (current->file_info == NULL) {
        pthread_mutex_unlock(&trie_mutex);
        return -1;
    }
    if (strcmp(current->file_info->owner_username, owner_username) != 0) {
        pthread_mutex_unlock(&trie_mutex);
        return -2; // Access denied
    }

    if (foldername && strlen(foldername) > 0)
        strncpy(current->file_info->folder, foldername, MAX_FILENAME - 1);
    else
        current->file_info->folder[0] = '\0';

    int ss_index = current->file_info->ss_index;
    pthread_mutex_unlock(&trie_mutex);
    write_log("SEARCH", "Moved file '%s' to folder '%s'", filename, foldername ? foldername : "");
    return ss_index;
}

int search_move_folder(const char* src, const char* dst, const char* owner_username, MoveFileUpdate* out_updates, int max_updates) {
    if (!src || !dst) return -1;
    pthread_mutex_lock(&trie_mutex);
    int src_idx = -1;
    for (int i = 0; i < folder_count; i++) if (strcmp(folder_registry[i].foldername, src) == 0) { src_idx = i; break; }
    if (src_idx == -1) { pthread_mutex_unlock(&trie_mutex); return -1; }
    if (strcmp(folder_registry[src_idx].owner_username, owner_username) != 0) { pthread_mutex_unlock(&trie_mutex); return -1; }

    // Ensure dst does not already exist
    for (int i = 0; i < folder_count; i++) if (strcmp(folder_registry[i].foldername, dst) == 0) { pthread_mutex_unlock(&trie_mutex); return -1; }

    // Rename folder entry (src -> dst)
    strncpy(folder_registry[src_idx].foldername, dst, MAX_FILENAME - 1);

    int updated = 0;
    int out_i = 0;
    // Walk trie and update
    TrieNode* stack[4096];
    int sp = 0;
    stack[sp++] = root;
    size_t src_len = strlen(src);

    while (sp > 0) {
        TrieNode* node = stack[--sp];
        if (node->file_info) {
            if (starts_with(node->file_info->folder, src)) {
                char new_folder[MAX_FILENAME] = {0};
                if (src_len == 0) {
                    strncpy(new_folder, dst, MAX_FILENAME - 1);
                } else {
                    const char* rest = node->file_info->folder + src_len;
                    if (rest[0] == '/') rest++;
                    if (strlen(rest) > 0)
                        snprintf(new_folder, MAX_FILENAME, "%s/%s", dst, rest);
                    else
                        snprintf(new_folder, MAX_FILENAME, "%s", dst);
                }
                strncpy(node->file_info->folder, new_folder, MAX_FILENAME - 1);
                if (out_updates && out_i < max_updates) {
                    strncpy(out_updates[out_i].filename, node->file_info->filename, MAX_FILENAME - 1);
                    strncpy(out_updates[out_i].folder, node->file_info->folder, MAX_FILENAME - 1);
                    out_updates[out_i].ss_index = node->file_info->ss_index;
                    out_i++;
                }
                updated++;
            }
        }
        for (int i = 0; i < TRIE_CHAR_SET_SIZE; i++) if (node->children[i]) stack[sp++] = node->children[i];
    }

    pthread_mutex_unlock(&trie_mutex);
    write_log("SEARCH", "Moved folder '%s' -> '%s' and updated %d files", src, dst, updated);
    return out_i; // number of updates written to out_updates
}

int search_get_files_in_folder(const char* foldername, const char* username, int flags, char* out_buffer, int buffer_size) {
    memset(out_buffer, 0, buffer_size);
    ViewTraversalData data;
    data.username = username;
    data.flags = flags;
    data.buffer = out_buffer;
    data.buffer_size = buffer_size;
    data.current_offset = 0;

    // If -l flag requested, refresh metadata for files in this folder
    if (flags & VIEW_FLAG_LONG) {
        int max_files = MAX_STORAGE_SERVERS * MAX_FILES_PER_SERVER;
        FileEntry* entries = malloc(sizeof(FileEntry) * max_files);
        int entry_count = 0;
        if (entries) {
            pthread_mutex_lock(&trie_mutex);
            TrieNode* stack[4096]; int sp = 0; stack[sp++] = root;
            while (sp > 0 && entry_count < max_files) {
                TrieNode* node = stack[--sp];
                if (node->file_info) {
                    if (strcmp(node->file_info->folder, foldername ? foldername : "") == 0) {
                        strncpy(entries[entry_count].filename, node->file_info->filename, MAX_FILENAME - 1);
                        entries[entry_count].ss_index = node->file_info->ss_index;
                        entry_count++;
                    }
                }
                for (int i = 0; i < TRIE_CHAR_SET_SIZE; i++) if (node->children[i]) stack[sp++] = node->children[i];
            }
            pthread_mutex_unlock(&trie_mutex);

            for (int i = 0; i < entry_count; i++) {
                FileEntry *e = &entries[i];
                StorageServerInfo* ss = get_ss_by_index(e->ss_index);
                if (ss == NULL || !ss->is_active) continue;
                MessageHeader meta_req;
                memset(&meta_req, 0, sizeof(meta_req));
                meta_req.msg_type = MSG_INTERNAL_GET_METADATA;
                meta_req.source_component = COMPONENT_NAME_SERVER;
                strncpy(meta_req.filename, e->filename, MAX_FILENAME - 1);

                SSMetadataPayload meta_payload;
                memset(&meta_payload, 0, sizeof(meta_payload));

                pthread_mutex_lock(&ss->socket_mutex);
                int ss_sock = ss->ss_socket_fd;
                if (send_header(ss_sock, &meta_req) == 0) {
                    MessageHeader resp;
                    if (recv_header(ss_sock, &resp) == 0 && resp.msg_type == MSG_INTERNAL_METADATA_RESP) {
                        if (recv_all(ss_sock, &meta_payload, sizeof(meta_payload)) == 0) {
                            search_update_file_metadata(e->filename, &meta_payload);
                        }
                    }
                }
                pthread_mutex_unlock(&ss->socket_mutex);
            }
            free(entries);
        }
    }

    // Build listing: immediate subfolders then files
    pthread_mutex_lock(&trie_mutex);
    int base_len = foldername ? strlen(foldername) : 0;
    for (int i = 0; i < folder_count; i++) {
        const char* fname = folder_registry[i].foldername;
        if (base_len == 0) {
            if (strchr(fname, '/') == NULL) {
                int chars_written = 0;
                if (flags & VIEW_FLAG_LONG) {
                    chars_written = snprintf(out_buffer + data.current_offset, data.buffer_size - data.current_offset,
                                             "| D | %-10s | %5s | %5s | %16s | %-5s |\n",
                                             fname, "-", "-", "-", folder_registry[i].owner_username);
                } else {
                    chars_written = snprintf(out_buffer + data.current_offset, data.buffer_size - data.current_offset, "[D] %s\n", fname);
                }
                if (data.current_offset + chars_written < data.buffer_size) data.current_offset += chars_written;
            }
        } else {
            char prefix[MAX_FILENAME]; snprintf(prefix, sizeof(prefix), "%s/", foldername);
            if (starts_with(fname, prefix)) {
                const char* rest = fname + strlen(prefix);
                if (strchr(rest, '/') == NULL) {
                    int chars_written = 0;
                    if (flags & VIEW_FLAG_LONG) {
                        chars_written = snprintf(out_buffer + data.current_offset, data.buffer_size - data.current_offset,
                                                 "| D | %-10s | %5s | %5s | %16s | %-5s |\n",
                                                 rest, "-", "-", "-", folder_registry[i].owner_username);
                    } else {
                        chars_written = snprintf(out_buffer + data.current_offset, data.buffer_size - data.current_offset, "[D] %s\n", rest);
                    }
                    if (data.current_offset + chars_written < data.buffer_size) data.current_offset += chars_written;
                }
            }
        }
    }

    // Files in this folder
    TrieNode* stack2[4096]; int sp2 = 0; stack2[sp2++] = root;
    while (sp2 > 0) {
        TrieNode* node = stack2[--sp2];
        if (node->file_info) {
            const char* ffolder = node->file_info->folder;
            if (((foldername == NULL || strlen(foldername) == 0) && (ffolder == NULL || strlen(ffolder) == 0))
                || (ffolder && foldername && strcmp(ffolder, foldername) == 0)) {
                int has_permission = 0;
                if (flags & VIEW_FLAG_ALL) has_permission = 1;
                else if (strcmp(node->file_info->owner_username, username) == 0) has_permission = 1;
                else {
                    for (int a = 0; a < node->file_info->acl_count; a++) {
                        if (strcmp(node->file_info->acl[a].username, username) == 0 && node->file_info->acl[a].permission >= PERM_READ) { has_permission = 1; break; }
                    }
                }
                if (has_permission) {
                    int chars_written = 0;
                    if (flags & VIEW_FLAG_LONG) {
                        char time_str[30]; strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M", localtime(&node->file_info->last_accessed));
                        chars_written = snprintf(out_buffer + data.current_offset, data.buffer_size - data.current_offset,
                                                 "| F | %-10s | %5ld | %5ld | %16s | %-5s |\n",
                                                 node->file_info->filename, node->file_info->word_count, node->file_info->char_count,
                                                 time_str, node->file_info->owner_username);
                    } else {
                        chars_written = snprintf(out_buffer + data.current_offset, data.buffer_size - data.current_offset, "--> %s\n", node->file_info->filename);
                    }
                    if (data.current_offset + chars_written < data.buffer_size) data.current_offset += chars_written;
                }
            }
        }
        for (int i = 0; i < TRIE_CHAR_SET_SIZE; i++) if (node->children[i]) stack2[sp2++] = node->children[i];
    }

    pthread_mutex_unlock(&trie_mutex);
    return data.current_offset;
}

/**
 * @brief Creates and initializes a new TrieNode.
 */
static TrieNode* create_node() {
    TrieNode* node = (TrieNode*)malloc(sizeof(TrieNode));
    if (node) {
        node->file_info = NULL;
        for (int i = 0; i < TRIE_CHAR_SET_SIZE; i++) {
            node->children[i] = NULL;
        }
    }
    return node;
}

/**
 * @brief Internal helper to find a file record.
 * Returns a pointer to the record or NULL if not found.
 * NOTE: This function assumes the trie_mutex is already locked.
 */
static FileRecord* find_file_record(const char* filename) {
    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        int index = (int)filename[i];
        if (index < 0 || index >= TRIE_CHAR_SET_SIZE) continue; // Skip invalid chars

        if (current->children[index] == NULL) {
            return NULL; // Path does not exist
        }
        current = current->children[index];
    }
    
    // At the end of the string, check if it's a file
    return current->file_info; // Will be NULL if not a file
}

// =========================================================================
//  PUBLIC API FUNCTIONS
// =========================================================================

void init_search_trie() {
    root = create_node();
    pthread_mutex_init(&trie_mutex, NULL);
    write_log("INIT", "File Search (Trie) initialized.");
}

/**
 * @brief Adds a file to the Trie.
 */
void search_add_file(const char* filename, int ss_index, const char* owner) {
    pthread_mutex_lock(&trie_mutex);

    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        int index = (int)filename[i];
        if (index < 0 || index >= TRIE_CHAR_SET_SIZE) continue;

        if (current->children[index] == NULL) {
            current->children[index] = create_node();
        }
        current = current->children[index];
    }

    // At the end of the path
    if (current->file_info != NULL) {
        write_log("WARN", "[SEARCH] File '%s' already exists. (Not adding)", filename);
    } else {
        // Create new FileRecord
        FileRecord* new_record = (FileRecord*)malloc(sizeof(FileRecord));
        strncpy(new_record->filename, filename, MAX_FILENAME- 1);
        strncpy(new_record->owner_username, owner, 64 - 1);
        new_record->ss_index = ss_index;
    new_record->folder[0] = '\0';
    new_record->word_count = 0;
    new_record->char_count = 0;
    new_record->created = 0;
    new_record->modified = 0;
    new_record->last_accessed = 0;
    new_record->last_accessed_by[0] = '\0';
        new_record->acl_count = 0;
        
        current->file_info = new_record; // Link it to the trie

        write_log("SEARCH", "Added file '%s' to records (on SS index %d, Owner: %s)", 
                  filename, ss_index, owner);
    }

    pthread_mutex_unlock(&trie_mutex);
}

/**
 * @brief Finds a file and returns its SS index.
 */
int search_find_file(const char* filename) {
   // --- 1. CHECK CACHE FIRST ---
    int cached_index = cache_lookup(filename);
    if (cached_index != -1) {
        return cached_index; // Cache Hit!
    }

    // --- 2. CACHE MISS: Search the Trie ---
    pthread_mutex_lock(&trie_mutex);

    FileRecord* record = find_file_record(filename);
    int ss_index = -1;

    if (record != NULL) {
        ss_index = record->ss_index;
    }

    pthread_mutex_unlock(&trie_mutex);

    // --- 3. ADD TO CACHE (if found) ---
    if (ss_index != -1) {
        write_log("SEARCH", "Search for '%s'... found on SS index %d (Trie)", filename, ss_index);
        cache_add(filename, ss_index);
    } else {
        write_log("SEARCH", "Search for '%s'... NOT FOUND (Trie)", filename);
    }

    return ss_index;
}

/**
 * @brief Checks if a user has a specific permission for a file.
 */
int search_check_permission(const char* filename, const char* username, PermissionType permission) {
    pthread_mutex_lock(&trie_mutex);
    
    FileRecord* record = find_file_record(filename);
    if (record == NULL) {
        pthread_mutex_unlock(&trie_mutex);
        return 0; // File doesn't exist, so no permission
    }

    // 1. Check if user is the owner (owner has all permissions)
    if (strcmp(record->owner_username, username) == 0) {
        pthread_mutex_unlock(&trie_mutex);
        return 1; // Owner can do anything
    }

    // 2. Check the ACL
    for (int i = 0; i < record->acl_count; i++) {
        if (strcmp(record->acl[i].username, username) == 0) {
            if (record->acl[i].permission >= permission) {
                pthread_mutex_unlock(&trie_mutex);
                return 1; // Access granted
            }
        }
    }

    // 3. No match
    pthread_mutex_unlock(&trie_mutex);
    return 0; // Access denied
}

/**
 * @brief Grants a permission to a user for a specific file.
 */
int search_grant_permission(const char* filename, const char* owner_username, 
                            const char* target_username, PermissionType permission) {
    
    pthread_mutex_lock(&trie_mutex);
    
    FileRecord* record = find_file_record(filename);
    if (record == NULL) {
        pthread_mutex_unlock(&trie_mutex);
        return -1; // File not found
    }

    // 1. Check if the user making the request is the owner
    if (strcmp(record->owner_username, owner_username) != 0) {
        pthread_mutex_unlock(&trie_mutex);
        return -1; // Not the owner, access denied
    }

    // 2. Find the target user in the ACL to update, or add new
    int found_index = -1;
    for (int i = 0; i < record->acl_count; i++) {
        if (strcmp(record->acl[i].username, target_username) == 0) {
            found_index = i;
            break;
        }
    }

    if (found_index != -1) {
        record->acl[found_index].permission = permission;
    } else {
        if (record->acl_count >= MAX_ACL_ENTRIES) {
            pthread_mutex_unlock(&trie_mutex);
            return -1; // ACL is full
        }
        int new_index = record->acl_count;
        strncpy(record->acl[new_index].username, target_username, 64 - 1);
        record->acl[new_index].permission = permission;
        record->acl_count++;
    }

    pthread_mutex_unlock(&trie_mutex);
    write_log("SEARCH", "User '%s' granted permission %d for file '%s' to user '%s'",
              owner_username, permission, filename, target_username);
    return 0; // Success
}

/**
 * @brief Removes all permissions for a user from a specific file.
 */
int search_remove_permission(const char* filename, const char* owner_username, 
                             const char* target_username) {

    pthread_mutex_lock(&trie_mutex);
    
    FileRecord* record = find_file_record(filename);
    if (record == NULL) {
        pthread_mutex_unlock(&trie_mutex);
        return -1; // File not found
    }

    if (strcmp(record->owner_username, owner_username) != 0) {
        pthread_mutex_unlock(&trie_mutex);
        return -1; // Not the owner
    }

    int found_index = -1;
    for (int i = 0; i < record->acl_count; i++) {
        if (strcmp(record->acl[i].username, target_username) == 0) {
            found_index = i;
            break;
        }
    }

    if (found_index != -1) {
        int last_index = record->acl_count - 1;
        record->acl[found_index] = record->acl[last_index]; // Swap with last
        record->acl_count--;
    }

    pthread_mutex_unlock(&trie_mutex);
    write_log("SEARCH", "User '%s' removed access for file '%s' from user '%s'",
              owner_username, filename, target_username);
    return 0; // Success
}

/**
 * @brief Deletes a file record from the Trie.
 * For simplicity, we find the node, free the FileRecord,
 * and set the pointer to NULL. (A full implementation
 * would also prune parent nodes if they become empty).
 */
int search_delete_file(const char* filename, const char* username) {
    pthread_mutex_lock(&trie_mutex);

    TrieNode* current = root;
    for (int i = 0; filename[i] != '\0'; i++) {
        int index = (int)filename[i];
        if (index < 0 || index >= TRIE_CHAR_SET_SIZE || current->children[index] == NULL) {
            pthread_mutex_unlock(&trie_mutex);
            write_log("SEARCH", "User '%s' failed to delete '%s': File Not Found.", username, filename);
            return -1; // Not Found
        }
        current = current->children[index];
    }

    // We found the node. Now check file_info and ownership.
    if (current->file_info == NULL) {
        pthread_mutex_unlock(&trie_mutex);
        write_log("SEARCH", "User '%s' failed to delete '%s': File Not Found.", username, filename);
        return -1; // Not Found
    }

    if (strcmp(current->file_info->owner_username, username) != 0) {
        pthread_mutex_unlock(&trie_mutex);
        write_log("SEARCH", "User '%s' failed to delete '%s': Access Denied (Not Owner).", username, filename);
        return -2; // Access Denied
    }

    // --- Access Granted ---
    int ss_index = current->file_info->ss_index;
    
    // Free the record and unlink it from the trie
    free(current->file_info);
    current->file_info = NULL;

    pthread_mutex_unlock(&trie_mutex);
    
    write_log("SEARCH", "User '%s' successfully deleted file '%s' (from SS %d).", 
              username, filename, ss_index);
              
    return ss_index; // Success, return the SS index
}

/**
 * @brief Gets a copy of a file's details.
 */
int search_get_file_details(const char* filename, FileRecord* record_copy) {
    pthread_mutex_lock(&trie_mutex);
    
    FileRecord* record_in_trie = find_file_record(filename);
    
    if (record_in_trie == NULL) {
        pthread_mutex_unlock(&trie_mutex);
        return -1; // Not Found
    }
    
    // Copy the data out so the caller has it.
    // This is thread-safe, as the caller isn't holding a
    // pointer to the live trie data.
    memcpy(record_copy, record_in_trie, sizeof(FileRecord));
    
    pthread_mutex_unlock(&trie_mutex);
    return 0; // Success
}

// --- Internal recursive helper for purging ---
static void recursive_purge_by_ss(TrieNode* node, int dead_ss_index) {
    if (node == NULL) {
        return;
    }

    // Check if the current node is a file
    FileRecord* file = node->file_info;
    if (file != NULL) {
        // This node is a file. Does it belong to the dead server?
        if (file->ss_index == dead_ss_index) {
            write_log("SEARCH", "Purging file '%s' (was on dead SS %d)", 
                      file->filename, dead_ss_index);
            
            // Invalidate from cache
            cache_invalidate(file->filename);
            
            // Free the record and unlink it from the trie
            free(node->file_info);
            node->file_info = NULL;
        }
    }

    // Continue recursion for all children
    for (int i = 0; i < TRIE_CHAR_SET_SIZE; i++) {
        // CORRECTED RECURSIVE CALL:
        recursive_purge_by_ss(node->children[i], dead_ss_index);
    }
}


/**
 * @brief Public API to purge all files from a dead SS.
 */
void search_purge_by_ss(int ss_index) {
    if (ss_index < 0 || ss_index >= MAX_STORAGE_SERVERS) {
        return;
    }
    
    write_log("SEARCH", "Purging all files for dead SS index %d...", ss_index);
    
    // Lock the trie for the entire traversal
    pthread_mutex_lock(&trie_mutex);
    recursive_purge_by_ss(root, ss_index);
    pthread_mutex_unlock(&trie_mutex);
    
    write_log("SEARCH", "Purge complete for SS index %d.", ss_index);
}

// ... (at the bottom)

void search_rebuild_add_file(int ss_index, SSFileRecordPayload* file_payload) {
    pthread_mutex_lock(&trie_mutex);

    TrieNode* current = root;
    const char* filename = file_payload->filename;

    for (int i = 0; filename[i] != '\0'; i++) {
        int index = (int)filename[i];
        if (index < 0 || index >= TRIE_CHAR_SET_SIZE) continue;

        if (current->children[index] == NULL) {
            current->children[index] = create_node();
        }
        current = current->children[index];
    }

    // --- NEW FIX: Check for conflicts before adding ---
    if (current->file_info != NULL) {
        
        if (current->file_info->ss_index == ss_index) {
            // This is fine, the SS is just reconnecting with its own file.
            // We'll "refresh" the record.
            write_log("SEARCH", "[REBUILD] File '%s' from SS %d already in Trie. (Refreshing)", 
                      filename, ss_index);
            free(current->file_info);
            
        } else {
            // This is a conflict. The file already exists on a DIFFERENT SS.
            write_log("WARN", "[REBUILD] CONFLICT: File '%s' from SS %d rejected. "
                              "It already exists on SS %d.",
                      filename, ss_index, current->file_info->ss_index);
            
            // Reject the file by simply returning.
            pthread_mutex_unlock(&trie_mutex);
            return; 
        }

    } else {
        // No conflict, this is a new file.
        write_log("SEARCH", "[REBUILD] Added file '%s' to records (on SS %d, Owner: %s)", 
                  filename, ss_index, file_payload->owner_username);
    }
    // --- END FIX ---


    // Create new FileRecord and copy ALL data from the payload
    FileRecord* new_record = (FileRecord*)malloc(sizeof(FileRecord));
    
    // Copy file info
    strncpy(new_record->filename, file_payload->filename, MAX_FILENAME - 1);
    strncpy(new_record->owner_username, file_payload->owner_username, 64 - 1);
    new_record->ss_index = ss_index;
    
    // Copy timestamps and counts
    new_record->word_count = file_payload->word_count;
    new_record->char_count = file_payload->char_count;
    new_record->created = file_payload->created;
    new_record->modified = file_payload->modified;
    new_record->last_accessed = file_payload->last_accessed;
    strncpy(new_record->last_accessed_by, file_payload->last_accessed_by, 64 - 1);
    
    // Copy ACL
    new_record->acl_count = file_payload->acl_count;
    for (int i = 0; i < new_record->acl_count; i++) {
        strncpy(new_record->acl[i].username, file_payload->acl[i].username, 64 - 1);
        new_record->acl[i].permission = (PermissionType)file_payload->acl[i].permission;
    }
    // Copy folder if present
    if (file_payload->folder[0] != '\0') {
        strncpy(new_record->folder, file_payload->folder, MAX_FILENAME - 1);
    } else {
        new_record->folder[0] = '\0';
    }
    
    current->file_info = new_record; // Link it to the trie

    pthread_mutex_unlock(&trie_mutex);
}