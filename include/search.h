#ifndef SEARCH_H
#define SEARCH_H

#include "protocol.h" // For PermissionType
#include <pthread.h>  // For pthread_mutex_t
#include <time.h>     // For time_t
#include "common.h"

#define MAX_ACL_ENTRIES 10 // Max 10 users per file's ACL
#define TRIE_CHAR_SET_SIZE 128 // For all ASCII characters

// --- Data Structures ---

// Represents one user's permission on a file
typedef struct {
    char username[64];
    PermissionType permission;
} AclEntry;

// This is the main data structure for a file.
// A pointer to this is stored in the Trie.
typedef struct {
    char filename[MAX_FILENAME];
    char owner_username[64];
    int ss_index; // Which storage server has this file
    char folder[MAX_FILENAME];
    
    long word_count;
    long char_count;
    time_t created;
    time_t modified;
    time_t last_accessed;
    char last_accessed_by[64];
    
    AclEntry acl[MAX_ACL_ENTRIES];
    int acl_count;
} FileRecord;

// The Trie Node structure
typedef struct TrieNode {
    struct TrieNode* children[TRIE_CHAR_SET_SIZE];
    FileRecord* file_info; // NULL if not the end of a file
} TrieNode;


// --- Functions ---

void init_search_trie();

void search_add_file(const char* filename, int ss_index, const char* owner);

int search_find_file(const char* filename);

// We'll add this later for the DELETE command
// void search_delete_file(const char* filename, const char* username);

int search_check_permission(const char* filename, const char* username, PermissionType permission);

int search_grant_permission(const char* filename, const char* owner_username, const char* target_username, PermissionType permission);

int search_remove_permission(const char* filename, const char* owner_username, const char* target_username);

/**
 * @brief Deletes a file from the search records.
 * Only the owner can perform this.
 * @param filename The file to delete.
 * @param username The user making the request.
 * @return The ss_index of the file on success, -1 on "Not Found", -2 on "Access Denied".
 */
int search_delete_file(const char* filename, const char* username);

/**
 * @brief Gets a copy of a file's details from the Trie.
 * This is thread-safe.
 * @param filename The file to look up.
 * @param record_copy A pointer to a FileRecord struct to copy data into.
 * @return 0 on success, -1 if file not found.
 */
int search_get_file_details(const char* filename, FileRecord* record_copy);

/**
 * @brief Traverses the entire Trie and builds a formatted string of files.
 * This is a complex, recursive function.
 * @param username The user asking for the list.
 * @param flags A bitmask of VIEW_FLAG_ALL and VIEW_FLAG_LONG.
 * @param out_buffer The buffer to write the list into.
 * @param buffer_size The size of the output buffer.
 * @return The number of bytes written to the buffer.
 */
int search_get_file_list(const char* username, int flags, char* out_buffer, int buffer_size);
/**
 * @brief Traverses the entire Trie and deletes all file records
 * associated with a specific, dead storage server.
 * @param ss_index The index of the SS to purge.
 */
void search_purge_by_ss(int ss_index);

// ... (after search_get_file_details)

/**
 * @brief Rebuilds a file record in the Trie from an SS.
 * This is used on SS registration to populate the NS.
 * @param ss_index The index of the SS that owns this file.
 * @param file_payload The full file record from the SS.
 */
void search_rebuild_add_file(int ss_index, SSFileRecordPayload* file_payload);

// Folder APIs
// Represents a folder move update for notifying SSes
typedef struct {
    char filename[MAX_FILENAME];
    char folder[MAX_FILENAME];
    int ss_index;
} MoveFileUpdate;

int search_add_folder(const char* foldername, const char* owner_username);
int search_find_folder(const char* foldername);
// Move/rename a folder and update contained files. Returns number of updated files or -1 on error.
int search_move_folder(const char* src, const char* dst, const char* owner_username, MoveFileUpdate* out_updates, int max_updates);

// Set a single file's folder. Returns ss_index on success, -1 not found, -2 access denied.
int search_set_file_folder(const char* filename, const char* foldername, const char* owner_username);

// List immediate contents of a folder (files + immediate subfolders). Returns bytes written.
int search_get_files_in_folder(const char* foldername, const char* username, int flags, char* out_buffer, int buffer_size);

#endif // SEARCH_H