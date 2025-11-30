#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stddef.h> // For size_t
#include "common.h" // For MAX_FILENAME

// ------------------------------------------------------------
//  CONSTANTS
// ------------------------------------------------------------
// (We get MAX_FILENAME from common.h)
#define MAX_PAYLOAD_SIZE   512
#define MAX_SERVER_NAME    64
#define MAX_ACL_ENTRIES    10

// ------------------------------------------------------------
//  ENUMS
// ------------------------------------------------------------
typedef enum {
    PERM_NONE = 0,
    PERM_READ = 1,
    PERM_WRITE = 2
} PermissionType;

// ------------------------------------------------------------
//  BASIC PAYLOAD STRUCTS
// (Must be defined before complex structs that use them)
// ------------------------------------------------------------

// For ADDACCESS command
typedef struct {
    char target_username[64];
    PermissionType permission;
} AccessControlPayload;

// For READ/WRITE/STREAM redirect response
typedef struct {
    char ip_addr[64];
    int port;
} SSReadPayload;

// For VIEW command request
typedef struct {
    int flags;
} ViewPayload;

// For internal metadata request
typedef struct {
    long word_count;
    long char_count;
    time_t created;
    time_t last_modified;
    time_t last_accessed;
    char last_accessed_by[64];
} SSMetadataPayload;

// For ACL lists in network payloads
typedef struct {
    char username[64];
    PermissionType permission;
} AclEntryPayload;

// Add this typedef if it's missing:
typedef struct {
    char ss_ip[64];
    int ss_port;
} InfoPayload;

// ------------------------------------------------------------
//  MESSAGE TYPES
// ------------------------------------------------------------
#define COMPONENT_CLIENT         1
#define COMPONENT_NAME_SERVER    2
#define COMPONENT_STORAGE_SERVER 3

#define MSG_ACK        11    // Any -> Any
#define MSG_ERROR      18    // Any -> Any

// Client -> NS
#define MSG_CREATE          12
#define MSG_READ            14
#define MSG_DELETE          16
#define MSG_REGISTER_CLIENT 23
#define MSG_ADD_ACCESS      24
#define MSG_REM_ACCESS      25
#define MSG_EXEC            26
#define MSG_WRITE           27
#define MSG_STREAM          28
#define MSG_UNDO            29
#define MSG_INFO            30
#define MSG_LIST            32
#define MSG_VIEW            34
#define MSG_SS_DEAD_REPORT  38

// Folder-related client -> NS
#define MSG_CREATE_FOLDER   40
#define MSG_MOVE_FILE       41 // MOVE <filename> <folder>
#define MSG_MOVE_FOLDER     42 // MOVEFOLDER <src> <dst>
#define MSG_VIEWFOLDER      43 // VIEWFOLDER <folder>

// NS -> Client
#define MSG_READ_REDIRECT   21
#define MSG_INFO_RESPONSE   31
#define MSG_LIST_RESPONSE   33
#define MSG_VIEW_RESPONSE   35

// SS -> NS
#define MSG_REGISTER            10
#define MSG_REGISTER_FILE       36
#define MSG_REGISTER_COMPLETE   37

// NS <-> SS (Internal)
#define MSG_INTERNAL_READ           100
#define MSG_INTERNAL_DATA           101
#define MSG_INTERNAL_GET_METADATA   102
#define MSG_INTERNAL_METADATA_RESP  103
#define MSG_INTERNAL_ADD_ACCESS     104  // NS -> SS   <-- ADD THIS
#define MSG_INTERNAL_REM_ACCESS     105  // NS -> SS   <-- ADD THIS
#define MSG_INTERNAL_SET_OWNER       106
// Internal: set file's folder on SS (payload = foldername string)
#define MSG_INTERNAL_SET_FOLDER      107

// Checkpoint-related message types
#define MSG_CHECKPOINT         120
#define MSG_VIEWCHECKPOINT     121  
#define MSG_REVERT             122
#define MSG_LISTCHECKPOINTS     123

// Add this new message type (use an unused number):
#define MSG_LOCATE_FILE        130  // Request to locate a file for access request purposes
#define MSG_LOCATE_RESPONSE    131  // Response with storage server location
        
// ------------------------------------------------------------
//  COMPLEX PAYLOADS & MAIN HEADER
// (Must be defined *after* the basic structs they use)
// ------------------------------------------------------------

#define VIEW_FLAG_ALL  1 // Corresponds to -a
#define VIEW_FLAG_LONG 2 // Corresponds to -l
// Main message header
typedef struct {
    uint16_t msg_type;
    uint16_t source_component;
    uint16_t dest_component;
    uint32_t payload_length;
    char filename[MAX_FILENAME]; // Use MAX_FILENAME from common.h
} MessageHeader;

// Payload for SS file registration
typedef struct {
    char filename[MAX_FILENAME];
    char owner_username[64];
    AclEntryPayload acl[MAX_ACL_ENTRIES];
    int acl_count;
    long word_count;
    long char_count;
    time_t created;
    time_t modified;
    time_t last_accessed;
    char last_accessed_by[64];
    char folder[MAX_FILENAME];
} SSFileRecordPayload;

// Payload for VIEWFOLDER request: flags + folder name
typedef struct {
    int flags;
    char folder[MAX_FILENAME];
} ViewFolderPayload;

// Payload for INFO command response
typedef struct {
    char filename[MAX_FILENAME];
    char owner_username[64];
    char ss_ip[64];
    int ss_port;
    AclEntryPayload acl[MAX_ACL_ENTRIES];
    int acl_count;
    long word_count;
    long char_count;
    time_t created;
    time_t last_modified;
    time_t last_accessed;
    char last_accessed_by[64];
} FileInfoPayload;

// ------------------------------------------------------------
//  UTILITY FUNCTIONS (from protocol.c)
// ------------------------------------------------------------
int send_all(int socket_fd, const void *buf, size_t len);
int recv_all(int socket_fd, void *buf, size_t len);
int send_header(int socket_fd, MessageHeader *header);
int recv_header(int socket_fd, MessageHeader *header);

#endif // PROTOCOL_H