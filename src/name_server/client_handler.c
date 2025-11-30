#include "client_handler.h"
#include "logger.h"
#include "protocol.h"
#include "storage_manager.h"
#include "search.h"
#include "executor.h"
#include "cache.h"
#include "user_manager.h"
#include <unistd.h> // for close()
#include <string.h>
#include <stdlib.h> // For malloc/free

// =========================================================================
//  HELPER FUNCTIONS
// =========================================================================

static void send_error_to_client(int sock_fd, const char* error_message) {
    write_log("ERROR", "Socket %d: %s", sock_fd, error_message);
    
    MessageHeader err_header;
    memset(&err_header, 0, sizeof(err_header));
    
    err_header.msg_type = MSG_ERROR;
    err_header.source_component = COMPONENT_NAME_SERVER;
    err_header.dest_component = COMPONENT_CLIENT;
    strncpy(err_header.filename, error_message, MAX_FILENAME - 1);
    err_header.payload_length = 0;

    send_header(sock_fd, &err_header);
}

static void send_ack_to_client(int sock_fd) {
    MessageHeader ack_header;
    memset(&ack_header, 0, sizeof(ack_header));
    ack_header.msg_type = MSG_ACK;
    ack_header.source_component = COMPONENT_NAME_SERVER;
    ack_header.dest_component = COMPONENT_CLIENT;
    
    if (send_header(sock_fd, &ack_header) == -1) {
        write_log("WARN", "Socket %d: Failed to send ACK to client", sock_fd);
    }
}

void handle_ss_dead_report(int sock_fd, MessageHeader* header) {
    if (header->payload_length != sizeof(SSReadPayload)) {
        send_error_to_client(sock_fd, "Bad payload for SS_DEAD_REPORT.");
        return;
    }

    SSReadPayload payload;
    if (recv_all(sock_fd, &payload, sizeof(payload)) == -1) {
        return; // Client disconnected
    }

    write_log("CLIENT_CMD", "Socket %d: Reported dead SS at %s:%d",
              sock_fd, payload.ip_addr, payload.port);

    // Find the server's internal socket FD by its public address
    int ss_sock_to_purge = get_ss_sock_by_address(payload.ip_addr, payload.port);

    if (ss_sock_to_purge != -1) {
        write_log("CLIENT_CMD", "Found matching active SS (socket %d). Purging it.",
                  ss_sock_to_purge);
        
        // This is the trigger: remove the server and purge its files
        remove_storage_server(ss_sock_to_purge);
    } else {
        write_log("CLIENT_CMD", "Dead SS report for %s:%d does not match any active server. Ignoring.",
                  payload.ip_addr, payload.port);
    }
    
    // Acknowledge the report
    send_ack_to_client(sock_fd);
}

// =========================================================================
//  COMMAND HANDLERS (PROXY COMMANDS)
// =========================================================================

void handle_create_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_CREATE for file '%s'",
              client_username, sock_fd, header->filename);

    // --- FIX 1: Check if file exists BEFORE contacting the SS ---
    int existing_ss_index = search_find_file(header->filename);
    if (existing_ss_index != -1) {
        send_error_to_client(sock_fd, "File already exists.");
        write_log("CLIENT_CMD", "User '%s' create failed: '%s' already exists.",
                  client_username, header->filename);
        return;
    }
    // --- END FIX 1 ---

    // File does not exist, proceed with creation
    StorageServerInfo* ss = get_ss_for_new_file();
    if (ss == NULL) {
        send_error_to_client(sock_fd, "No active storage servers available.");
        return;
    }
    write_log("CLIENT_CMD", "Socket %d: Assigning file '%s' to SS on port %d (socket %d)",
              sock_fd, header->filename, ss->client_facing_port, ss->ss_socket_fd);

    // --- LOCK SS SOCKET ---
    pthread_mutex_lock(&ss->socket_mutex);

    header->dest_component = COMPONENT_STORAGE_SERVER;
    if (send_header(ss->ss_socket_fd, header) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(sock_fd, "Failed to communicate with storage server.");
        remove_storage_server(ss->ss_socket_fd);
        return;
    }

    MessageHeader ss_response;
    if (recv_header(ss->ss_socket_fd, &ss_response) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(sock_fd, "Storage server disconnected or failed to respond.");
        remove_storage_server(ss->ss_socket_fd);
        return;
    }
    
    pthread_mutex_unlock(&ss->socket_mutex);
    // --- UNLOCK SS SOCKET ---

    if (ss_response.msg_type != MSG_ACK) {
        send_error_to_client(sock_fd, "Storage server failed to create the file.");
        return;
    }
    write_log("CLIENT_CMD", "Socket %d: SS %d ACK'd file creation.", 
              sock_fd, ss->ss_socket_fd);

    // Now we can safely add it to our records
    int ss_index = ss - ss_registry;
    search_add_file(header->filename, ss_index, client_username); // Set owner in NS memory

    // --- FIX 2 (Persistence): Send MSG_INTERNAL_SET_OWNER to the SS ---
    // This tells the SS to save the owner to its metadata.bin
    MessageHeader owner_header;
    memset(&owner_header, 0, sizeof(owner_header));
    owner_header.msg_type = MSG_INTERNAL_SET_OWNER; // This is 106
    owner_header.source_component = COMPONENT_NAME_SERVER;
    // We'll send the target filename in the header and the owner's username as payload
    strncpy(owner_header.filename, header->filename, MAX_FILENAME - 1); // target file
    owner_header.payload_length = strlen(client_username) + 1; // include null terminator
    
    // We send this to the SS, but we don't wait for an ACK.
    pthread_mutex_lock(&ss->socket_mutex);
    send_header(ss->ss_socket_fd, &owner_header);
    send_all(ss->ss_socket_fd, client_username, owner_header.payload_length);
    pthread_mutex_unlock(&ss->socket_mutex);
    // --- END FIX 2 ---

    send_ack_to_client(sock_fd);
}

void handle_delete_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_DELETE for file '%s'",
              client_username, sock_fd, header->filename);

    int ss_index = search_delete_file(header->filename, client_username);

    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }
    if (ss_index == -2) {
        send_error_to_client(sock_fd, "Access Denied (Only owner can delete).");
        return;
    }

    cache_invalidate(header->filename);

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        write_log("WARN", "File '%s' deleted from records, but SS %d is inactive.", 
                  header->filename, ss_index);
        send_ack_to_client(sock_fd);
        return;
    }

    // --- LOCK SS SOCKET ---
    pthread_mutex_lock(&ss->socket_mutex);

    header->dest_component = COMPONENT_STORAGE_SERVER;
    if (send_header(ss->ss_socket_fd, header) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        write_log("ERROR", "Failed to send MSG_DELETE to SS %d.", ss_index);
        remove_storage_server(ss->ss_socket_fd);
        send_ack_to_client(sock_fd);
        return;
    }

    MessageHeader ss_response;
    if (recv_header(ss->ss_socket_fd, &ss_response) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        write_log("ERROR", "SS %d disconnected after DELETE request.", ss_index);
        remove_storage_server(ss->ss_socket_fd);
        send_ack_to_client(sock_fd);
        return;
    }
    
    pthread_mutex_unlock(&ss->socket_mutex);
    // --- UNLOCK SS SOCKET ---

    if (ss_response.msg_type != MSG_ACK) {
        write_log("ERROR", "SS %d failed to ACK delete, but file is gone from NS records.", ss_index);
    }

    send_ack_to_client(sock_fd);
}

void handle_undo_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_UNDO for file '%s'",
              client_username, sock_fd, header->filename);
    
    if (!search_check_permission(header->filename, client_username, PERM_WRITE)) {
        send_error_to_client(sock_fd, "Access Denied (Write Permission Required).");
        return;
    }

    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    // --- LOCK SS SOCKET ---
    pthread_mutex_lock(&ss->socket_mutex);

    int ss_sock = ss->ss_socket_fd;
    header->dest_component = COMPONENT_STORAGE_SERVER;
    if (send_header(ss_sock, header) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(sock_fd, "Failed to communicate with storage server.");
        remove_storage_server(ss_sock);
        return;
    }

    MessageHeader ss_response;
    if (recv_header(ss_sock, &ss_response) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(sock_fd, "Storage server disconnected or failed to respond.");
        remove_storage_server(ss_sock);
        return;
    }
    
    pthread_mutex_unlock(&ss->socket_mutex);
    // --- UNLOCK SS SOCKET ---

    if (ss_response.msg_type != MSG_ACK) {
        send_error_to_client(sock_fd, "Storage server failed to perform undo.");
        return;
    }

    write_log("CLIENT_CMD", "Socket %d: SS %d ACK'd file undo.", sock_fd, ss_sock);
    send_ack_to_client(sock_fd);
}

void handle_info_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_INFO for file '%s'",
              client_username, sock_fd, header->filename);
    
    if (!search_check_permission(header->filename, client_username, PERM_READ)) {
        send_error_to_client(sock_fd, "Access Denied (Read Permission Required).");
        return;
    }

    FileRecord file_data;
    if (search_get_file_details(header->filename, &file_data) == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(file_data.ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }
    
    SSMetadataPayload metadata;
    memset(&metadata, 0, sizeof(metadata));
    int ss_sock = ss->ss_socket_fd;

    // --- LOCK SS SOCKET ---
    pthread_mutex_lock(&ss->socket_mutex);

    MessageHeader meta_req_header;
    memset(&meta_req_header, 0, sizeof(meta_req_header));
    meta_req_header.msg_type = MSG_INTERNAL_GET_METADATA;
    meta_req_header.source_component = COMPONENT_NAME_SERVER;
    strncpy(meta_req_header.filename, header->filename, MAX_FILENAME - 1);
    
    if (send_header(ss_sock, &meta_req_header) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(sock_fd, "Failed to communicate with storage server.");
        remove_storage_server(ss_sock);
        return;
    }
    
    MessageHeader meta_resp_header;
    if (recv_header(ss_sock, &meta_resp_header) == -1 || meta_resp_header.msg_type != MSG_INTERNAL_METADATA_RESP) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(sock_fd, "Storage server failed to send metadata.");
        remove_storage_server(ss_sock);
        return;
    }

    if (recv_all(ss_sock, &metadata, sizeof(SSMetadataPayload)) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(sock_fd, "Failed to receive metadata payload.");
        remove_storage_server(ss_sock);
        return;
    }
    
    pthread_mutex_unlock(&ss->socket_mutex);
    // --- UNLOCK SS SOCKET ---
    
    write_log("CLIENT_CMD", "Socket %d: Got metadata from SS %d", sock_fd, ss_sock);

    FileInfoPayload payload;
    memset(&payload, 0, sizeof(payload));

    strncpy(payload.filename, file_data.filename, MAX_FILENAME - 1);
    strncpy(payload.owner_username, file_data.owner_username, 64 - 1);
    strncpy(payload.ss_ip, ss->ip_addr, 64 - 1);
    payload.ss_port = ss->client_facing_port;
    payload.acl_count = file_data.acl_count;
    for (int i = 0; i < file_data.acl_count; i++) {
        strncpy(payload.acl[i].username, file_data.acl[i].username, 64 - 1);
        payload.acl[i].permission = file_data.acl[i].permission;
    }
    payload.word_count = metadata.word_count;
    payload.char_count = metadata.char_count;
    payload.created = metadata.created;
    payload.last_modified = metadata.last_modified;
    payload.last_accessed = metadata.last_accessed;
    strncpy(payload.last_accessed_by, metadata.last_accessed_by, 64 - 1);

    MessageHeader resp_header;
    memset(&resp_header, 0, sizeof(resp_header));
    resp_header.msg_type = MSG_INFO_RESPONSE;
    resp_header.source_component = COMPONENT_NAME_SERVER;
    resp_header.dest_component = COMPONENT_CLIENT;
    resp_header.payload_length = sizeof(FileInfoPayload);

    if (send_header(sock_fd, &resp_header) == -1) { return; }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { return; }

    write_log("CLIENT_CMD", "Socket %d: Sent full INFO response for '%s'",
              sock_fd, header->filename);
}  

void handle_add_access(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_ADD_ACCESS for file '%s'",
              client_username, sock_fd, header->filename);

    if (header->payload_length != sizeof(AccessControlPayload)) {
        send_error_to_client(sock_fd, "Bad payload for ADD_ACCESS.");
        return;
    }
    AccessControlPayload payload;
    if (recv_all(sock_fd, &payload, sizeof(payload)) == -1) {
        return; // Client disconnected
    }

    int result = search_grant_permission(header->filename, client_username, 
                                         payload.target_username, payload.permission);
    
    if (result == -1) {
        send_error_to_client(sock_fd, "Access Denied (Not Owner or File Not Found).");
        return;
    }
    
    // --- FILL IN THE TODO ---
    int ss_index = search_find_file(header->filename);
    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    MessageHeader ss_header;
    memset(&ss_header, 0, sizeof(ss_header));
    ss_header.msg_type = MSG_INTERNAL_ADD_ACCESS;
    ss_header.source_component = COMPONENT_NAME_SERVER;
    strncpy(ss_header.filename, header->filename, MAX_FILENAME - 1);
    ss_header.payload_length = sizeof(AccessControlPayload);

    pthread_mutex_lock(&ss->socket_mutex);
    send_header(ss->ss_socket_fd, &ss_header);
    send_all(ss->ss_socket_fd, &payload, sizeof(payload));
    
    // Wait for ACK from SS
    MessageHeader ss_response;
    recv_header(ss->ss_socket_fd, &ss_response);
    pthread_mutex_unlock(&ss->socket_mutex);

    if (ss_response.msg_type == MSG_ACK) {
        send_ack_to_client(sock_fd);
    } else {
        send_error_to_client(sock_fd, "Storage server failed to update ACL.");
    }
    // --- END TODO ---
}

void handle_rem_access(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_REM_ACCESS for file '%s'",
              client_username, sock_fd, header->filename);

    if (header->payload_length == 0 || header->payload_length > 64) {
        send_error_to_client(sock_fd, "Bad payload for REM_ACCESS.");
        return;
    }
    char target_username[64];
    memset(target_username, 0, sizeof(target_username));
    if (recv_all(sock_fd, target_username, header->payload_length) == -1) {
        return;
    }

    int result = search_remove_permission(header->filename, client_username, target_username);

    if (result == -1) {
        send_error_to_client(sock_fd, "Access Denied (Not Owner or File Not Found).");
        return;
    }

    // --- FILL IN THE TODO ---
    int ss_index = search_find_file(header->filename);
    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    MessageHeader ss_header;
    memset(&ss_header, 0, sizeof(ss_header));
    ss_header.msg_type = MSG_INTERNAL_REM_ACCESS;
    ss_header.source_component = COMPONENT_NAME_SERVER;
    // Put the target filename in the header and send the username as payload
    strncpy(ss_header.filename, header->filename, MAX_FILENAME - 1);
    ss_header.payload_length = strlen(target_username) + 1;


    pthread_mutex_lock(&ss->socket_mutex);
    send_header(ss->ss_socket_fd, &ss_header);
    send_all(ss->ss_socket_fd, target_username, ss_header.payload_length);
    
    // Wait for ACK from SS
    MessageHeader ss_response;
    recv_header(ss->ss_socket_fd, &ss_response);
    pthread_mutex_unlock(&ss->socket_mutex);

    if (ss_response.msg_type == MSG_ACK) {
        send_ack_to_client(sock_fd);
    } else {
        send_error_to_client(sock_fd, "Storage server failed to update ACL.");
    }
    // --- END TODO ---
}

void handle_locate_file_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_LOCATE_FILE for file '%s'",
              client_username, sock_fd, header->filename);
    
    // Find the file regardless of access permissions (this is the key difference from other handlers)
    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found in any storage server");
        write_log("WARN", "LOCATE_FILE: File %s not found in any storage server", header->filename);
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server");
        write_log("WARN", "LOCATE_FILE: File %s is on inactive storage server %d", header->filename, ss_index);
        return;
    }

    // Send storage server location information
    SSReadPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.ip_addr, ss->ip_addr, 64);
    payload.port = ss->client_facing_port;

    MessageHeader redirect_header;
    memset(&redirect_header, 0, sizeof(redirect_header));
    redirect_header.msg_type = MSG_LOCATE_RESPONSE;
    redirect_header.source_component = COMPONENT_NAME_SERVER;
    redirect_header.dest_component = COMPONENT_CLIENT;
    redirect_header.payload_length = sizeof(SSReadPayload);

    if (send_header(sock_fd, &redirect_header) == -1) { 
        write_log("ERROR", "Failed to send LOCATE_RESPONSE header to socket %d", sock_fd);
        return; 
    }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { 
        write_log("ERROR", "Failed to send LOCATE_RESPONSE payload to socket %d", sock_fd);
        return; 
    }
    
    write_log("CLIENT_CMD", "Socket %d: Sent location info for '%s' - SS at %s:%d",
              sock_fd, header->filename, payload.ip_addr, payload.port);
}

// =========================================================================
//  COMMAND HANDLERS (REDIRECT COMMANDS)
// =========================================================================

void handle_read_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_READ for file '%s'",
              client_username, sock_fd, header->filename);
    
    if (!search_check_permission(header->filename, client_username, PERM_READ)) {
        send_error_to_client(sock_fd, "Access Denied.");
        return;
    }

    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    SSReadPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.ip_addr, ss->ip_addr, 64);
    payload.port = ss->client_facing_port;

    MessageHeader redirect_header;
    memset(&redirect_header, 0, sizeof(redirect_header));
    redirect_header.msg_type = MSG_READ_REDIRECT;
    redirect_header.source_component = COMPONENT_NAME_SERVER;
    redirect_header.dest_component = COMPONENT_CLIENT;
    redirect_header.payload_length = sizeof(SSReadPayload);

    if (send_header(sock_fd, &redirect_header) == -1) { return; }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { return; }
    
    write_log("CLIENT_CMD", "Socket %d: Sent redirect for '%s' to SS at %s:%d",
              sock_fd, header->filename, payload.ip_addr, payload.port);
}

void handle_write_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_WRITE for file '%s'",
              client_username, sock_fd, header->filename);

    if (!search_check_permission(header->filename, client_username, PERM_WRITE)) {
        send_error_to_client(sock_fd, "Access Denied (Write Permission Required).");
        return;
    }

    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    SSReadPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.ip_addr, ss->ip_addr, 64);
    payload.port = ss->client_facing_port;

    MessageHeader redirect_header;
    memset(&redirect_header, 0, sizeof(redirect_header));
    redirect_header.msg_type = MSG_READ_REDIRECT; 
    redirect_header.source_component = COMPONENT_NAME_SERVER;
    redirect_header.dest_component = COMPONENT_CLIENT;
    redirect_header.payload_length = sizeof(SSReadPayload);

    if (send_header(sock_fd, &redirect_header) == -1) { return; }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { return; }

    write_log("CLIENT_CMD", "Socket %d: Sent WRITE redirect for '%s' to SS at %s:%d",
              sock_fd, header->filename, payload.ip_addr, payload.port);
}

void handle_stream_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_STREAM for file '%s'",
              client_username, sock_fd, header->filename);

    if (!search_check_permission(header->filename, client_username, PERM_READ)) {
        send_error_to_client(sock_fd, "Access Denied (Read Permission Required).");
        return;
    }

    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    SSReadPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.ip_addr, ss->ip_addr, 64);
    payload.port = ss->client_facing_port;

    MessageHeader redirect_header;
    memset(&redirect_header, 0, sizeof(redirect_header));
    redirect_header.msg_type = MSG_READ_REDIRECT;
    redirect_header.source_component = COMPONENT_NAME_SERVER;
    redirect_header.dest_component = COMPONENT_CLIENT;
    redirect_header.payload_length = sizeof(SSReadPayload);

    if (send_header(sock_fd, &redirect_header) == -1) { return; }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { return; }

    write_log("CLIENT_CMD", "Socket %d: Sent STREAM redirect for '%s' to SS at %s:%d",
              sock_fd, header->filename, payload.ip_addr, payload.port);
}

void handle_checkpoint_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_CHECKPOINT for file '%s'",
              client_username, sock_fd, header->filename);

    // Check write permission since checkpoints modify file state
    if (!search_check_permission(header->filename, client_username, PERM_WRITE)) {
        send_error_to_client(sock_fd, "Access Denied (Write Permission Required).");
        return;
    }

    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    SSReadPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.ip_addr, ss->ip_addr, 64);
    payload.port = ss->client_facing_port;

    MessageHeader redirect_header;
    memset(&redirect_header, 0, sizeof(redirect_header));
    redirect_header.msg_type = MSG_READ_REDIRECT; // Reuse existing redirect message type
    redirect_header.source_component = COMPONENT_NAME_SERVER;
    redirect_header.dest_component = COMPONENT_CLIENT;
    redirect_header.payload_length = sizeof(SSReadPayload);

    if (send_header(sock_fd, &redirect_header) == -1) { return; }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { return; }

    write_log("CLIENT_CMD", "Socket %d: Sent CHECKPOINT redirect for '%s' to SS at %s:%d",
              sock_fd, header->filename, payload.ip_addr, payload.port);
}

void handle_viewcheckpoint_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_VIEWCHECKPOINT for file '%s'",
              client_username, sock_fd, header->filename);

    // Check read permission for viewing checkpoints
    if (!search_check_permission(header->filename, client_username, PERM_READ)) {
        send_error_to_client(sock_fd, "Access Denied (Read Permission Required).");
        return;
    }

    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    SSReadPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.ip_addr, ss->ip_addr, 64);
    payload.port = ss->client_facing_port;

    MessageHeader redirect_header;
    memset(&redirect_header, 0, sizeof(redirect_header));
    redirect_header.msg_type = MSG_READ_REDIRECT;
    redirect_header.source_component = COMPONENT_NAME_SERVER;
    redirect_header.dest_component = COMPONENT_CLIENT;
    redirect_header.payload_length = sizeof(SSReadPayload);

    if (send_header(sock_fd, &redirect_header) == -1) { return; }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { return; }

    write_log("CLIENT_CMD", "Socket %d: Sent VIEWCHECKPOINT redirect for '%s' to SS at %s:%d",
              sock_fd, header->filename, payload.ip_addr, payload.port);
}

void handle_revert_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_REVERT for file '%s'",
              client_username, sock_fd, header->filename);

    // Check write permission since revert modifies file content
    if (!search_check_permission(header->filename, client_username, PERM_WRITE)) {
        send_error_to_client(sock_fd, "Access Denied (Write Permission Required).");
        return;
    }

    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    SSReadPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.ip_addr, ss->ip_addr, 64);
    payload.port = ss->client_facing_port;

    MessageHeader redirect_header;
    memset(&redirect_header, 0, sizeof(redirect_header));
    redirect_header.msg_type = MSG_READ_REDIRECT;
    redirect_header.source_component = COMPONENT_NAME_SERVER;
    redirect_header.dest_component = COMPONENT_CLIENT;
    redirect_header.payload_length = sizeof(SSReadPayload);

    if (send_header(sock_fd, &redirect_header) == -1) { return; }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { return; }

    write_log("CLIENT_CMD", "Socket %d: Sent REVERT redirect for '%s' to SS at %s:%d",
              sock_fd, header->filename, payload.ip_addr, payload.port);
}

void handle_listcheckpoints_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_LISTCHECKPOINTS for file '%s'",
              client_username, sock_fd, header->filename);

    // Check read permission for listing checkpoints
    if (!search_check_permission(header->filename, client_username, PERM_READ)) {
        send_error_to_client(sock_fd, "Access Denied (Read Permission Required).");
        return;
    }

    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    SSReadPayload payload;
    memset(&payload, 0, sizeof(payload));
    strncpy(payload.ip_addr, ss->ip_addr, 64);
    payload.port = ss->client_facing_port;

    MessageHeader redirect_header;
    memset(&redirect_header, 0, sizeof(redirect_header));
    redirect_header.msg_type = MSG_READ_REDIRECT;
    redirect_header.source_component = COMPONENT_NAME_SERVER;
    redirect_header.dest_component = COMPONENT_CLIENT;
    redirect_header.payload_length = sizeof(SSReadPayload);

    if (send_header(sock_fd, &redirect_header) == -1) { return; }
    if (send_all(sock_fd, &payload, sizeof(payload)) == -1) { return; }

    write_log("CLIENT_CMD", "Socket %d: Sent LISTCHECKPOINTS redirect for '%s' to SS at %s:%d",
              sock_fd, header->filename, payload.ip_addr, payload.port);
}

// =========================================================================
//  COMMAND HANDLERS (READ-ONLY)
// =========================================================================

#define USER_LIST_BUFFER_SIZE 4096
#define FILE_LIST_BUFFER_SIZE 8192 // 8KB, file lists could be long

void handle_list_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_LIST",
              client_username, sock_fd);

    char list_buffer[USER_LIST_BUFFER_SIZE];
    int list_size = user_manager_get_list(list_buffer, USER_LIST_BUFFER_SIZE);

    if (list_size == 0) {
        write_log("CLIENT_CMD", "Sending empty user list to '%s'", client_username);
    }

    MessageHeader resp_header;
    memset(&resp_header, 0, sizeof(resp_header));
    resp_header.msg_type = MSG_LIST_RESPONSE;
    resp_header.source_component = COMPONENT_NAME_SERVER;
    resp_header.dest_component = COMPONENT_CLIENT;
    resp_header.payload_length = list_size; 

    if (send_header(sock_fd, &resp_header) == -1) { return; }
    if (list_size > 0) {
        if (send_all(sock_fd, list_buffer, list_size) == -1) { return; }
    }

    write_log("CLIENT_CMD", "Socket %d: Sent user list (%d bytes) to '%s'",
              sock_fd, list_size, client_username);
}

void handle_view_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_VIEW",
              client_username, sock_fd);

    if (header->payload_length != sizeof(ViewPayload)) {
        send_error_to_client(sock_fd, "Bad payload for MSG_VIEW.");
        return;
    }
    ViewPayload payload;
    if (recv_all(sock_fd, &payload, sizeof(payload)) == -1) {
        return; // Client disconnected
    }

    char* list_buffer = malloc(FILE_LIST_BUFFER_SIZE);
    if (list_buffer == NULL) {
        send_error_to_client(sock_fd, "Internal server error (malloc).");
        return;
    }

    int list_size = search_get_file_list(client_username, payload.flags, 
                                         list_buffer, FILE_LIST_BUFFER_SIZE);

    MessageHeader resp_header;
    memset(&resp_header, 0, sizeof(resp_header));
    resp_header.msg_type = MSG_VIEW_RESPONSE;
    resp_header.source_component = COMPONENT_NAME_SERVER;
    resp_header.dest_component = COMPONENT_CLIENT;
    resp_header.payload_length = list_size;

    if (send_header(sock_fd, &resp_header) == -1) {
        free(list_buffer);
        return; 
    }

    if (list_size > 0) {
        if (send_all(sock_fd, list_buffer, list_size) == -1) {
            free(list_buffer);
            return;
        }
    }

    free(list_buffer);
    write_log("CLIENT_CMD", "Socket %d: Sent file list (%d bytes) to '%s'",
              sock_fd, list_size, client_username);
}

void handle_create_folder_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_CREATE_FOLDER for '%s'",
              client_username, sock_fd, header->filename);

    if (search_add_folder(header->filename, client_username) == 0) {
        send_ack_to_client(sock_fd);
    } else {
        send_error_to_client(sock_fd, "Folder already exists or could not be created.");
    }
}

void handle_move_file_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_MOVE_FILE for '%s'",
              client_username, sock_fd, header->filename);

    if (header->payload_length == 0 || header->payload_length > MAX_FILENAME) {
        send_error_to_client(sock_fd, "Bad payload for MOVE.");
        return;
    }
    char foldername[MAX_FILENAME];
    if (recv_all(sock_fd, foldername, header->payload_length) == -1) return;

    int ss_index = search_set_file_folder(header->filename, foldername, client_username);
    if (ss_index == -1) {
        send_error_to_client(sock_fd, "File not found.");
        return;
    }
    if (ss_index == -2) {
        send_error_to_client(sock_fd, "Access Denied (Only owner can move file).\n");
        return;
    }

    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(sock_fd, "File is on an inactive server.");
        return;
    }

    // Notify SS to persist folder change
    MessageHeader ss_header;
    memset(&ss_header, 0, sizeof(ss_header));
    ss_header.msg_type = MSG_INTERNAL_SET_FOLDER;
    ss_header.source_component = COMPONENT_NAME_SERVER;
    strncpy(ss_header.filename, header->filename, MAX_FILENAME - 1);
    ss_header.payload_length = strlen(foldername) + 1;

    pthread_mutex_lock(&ss->socket_mutex);
    send_header(ss->ss_socket_fd, &ss_header);
    send_all(ss->ss_socket_fd, foldername, ss_header.payload_length);

    // Wait for ACK from SS
    MessageHeader resp;
    if (recv_header(ss->ss_socket_fd, &resp) == -1 || resp.msg_type != MSG_ACK) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(sock_fd, "Storage server failed to update folder.");
        return;
    }
    pthread_mutex_unlock(&ss->socket_mutex);

    send_ack_to_client(sock_fd);
}

void handle_move_folder_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_MOVE_FOLDER for '%s'",
              client_username, sock_fd, header->filename);

    if (header->payload_length == 0 || header->payload_length > MAX_FILENAME) {
        send_error_to_client(sock_fd, "Bad payload for MOVEFOLDER.");
        return;
    }
    char dst_folder[MAX_FILENAME];
    if (recv_all(sock_fd, dst_folder, header->payload_length) == -1) return;

    // Prepare buffer for updates
    int max_updates = 4096;
    MoveFileUpdate* updates = malloc(sizeof(MoveFileUpdate) * max_updates);
    if (!updates) { send_error_to_client(sock_fd, "Internal server error."); return; }

    int updated_count = search_move_folder(header->filename, dst_folder, client_username, updates, max_updates);
    if (updated_count < 0) {
        free(updates);
        send_error_to_client(sock_fd, "Folder move failed (not found or permission denied).");
        return;
    }

    // Notify each SS of changed files
    for (int i = 0; i < updated_count; i++) {
        MoveFileUpdate *u = &updates[i];
        StorageServerInfo* ss = get_ss_by_index(u->ss_index);
        if (ss == NULL || !ss->is_active) continue;

        MessageHeader ss_header;
        memset(&ss_header, 0, sizeof(ss_header));
        ss_header.msg_type = MSG_INTERNAL_SET_FOLDER;
        ss_header.source_component = COMPONENT_NAME_SERVER;
        strncpy(ss_header.filename, u->filename, MAX_FILENAME - 1);
        ss_header.payload_length = strlen(u->folder) + 1;

        pthread_mutex_lock(&ss->socket_mutex);
        send_header(ss->ss_socket_fd, &ss_header);
        send_all(ss->ss_socket_fd, u->folder, ss_header.payload_length);

        // Wait for ACK
        MessageHeader resp;
        recv_header(ss->ss_socket_fd, &resp);
        pthread_mutex_unlock(&ss->socket_mutex);
    }

    free(updates);
    send_ack_to_client(sock_fd);
}

void handle_view_folder_request(int sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_VIEWFOLDER request",
              client_username, sock_fd);

    if (header->payload_length != sizeof(ViewFolderPayload)) {
        send_error_to_client(sock_fd, "Bad payload for MSG_VIEWFOLDER.");
        return;
    }
    ViewFolderPayload payload;
    if (recv_all(sock_fd, &payload, sizeof(payload)) == -1) return;

    char* list_buffer = malloc(FILE_LIST_BUFFER_SIZE);
    if (!list_buffer) { send_error_to_client(sock_fd, "Internal server error (malloc)."); return; }

    int list_size = search_get_files_in_folder(payload.folder, client_username, payload.flags, list_buffer, FILE_LIST_BUFFER_SIZE);

    MessageHeader resp_header;
    memset(&resp_header, 0, sizeof(resp_header));
    resp_header.msg_type = MSG_VIEW_RESPONSE;
    resp_header.source_component = COMPONENT_NAME_SERVER;
    resp_header.dest_component = COMPONENT_CLIENT;
    resp_header.payload_length = list_size;

    if (send_header(sock_fd, &resp_header) == -1) { free(list_buffer); return; }
    if (list_size > 0) {
        send_all(sock_fd, list_buffer, list_size);
    }
    free(list_buffer);
}

// =========================================================================
//  MESSAGE ROUTER
// =========================================================================

static void route_message(int sock_fd, MessageHeader* header, const char* client_username) {
    switch (header->msg_type) {
        case MSG_CREATE:
            handle_create_request(sock_fd, header, client_username);
            break;
        case MSG_CREATE_FOLDER:
            handle_create_folder_request(sock_fd, header, client_username);
            break;
        case MSG_READ:
            handle_read_request(sock_fd, header, client_username);
            break;
        case MSG_ADD_ACCESS:
            handle_add_access(sock_fd, header, client_username);
            break;
        case MSG_REM_ACCESS:
            handle_rem_access(sock_fd, header, client_username);
            break;
        case MSG_EXEC:
            handle_exec_request(sock_fd, header, client_username);
            break;
        case MSG_DELETE:
            handle_delete_request(sock_fd, header, client_username);
            break;
        case MSG_WRITE:
            handle_write_request(sock_fd, header, client_username);
            break;
        case MSG_STREAM:
            handle_stream_request(sock_fd, header, client_username);
            break;
        case MSG_UNDO:
            handle_undo_request(sock_fd, header, client_username);
            break;
        case MSG_INFO:
            handle_info_request(sock_fd, header, client_username);
            break;
        case MSG_LIST:
            handle_list_request(sock_fd, header, client_username);
            break;
        case MSG_VIEW:
            handle_view_request(sock_fd, header, client_username);
            break;
        case MSG_VIEWFOLDER:
            handle_view_folder_request(sock_fd, header, client_username);
            break;
        case MSG_MOVE_FILE:
            handle_move_file_request(sock_fd, header, client_username);
            break;
        case MSG_MOVE_FOLDER:
            handle_move_folder_request(sock_fd, header, client_username);
            break;
        case MSG_SS_DEAD_REPORT:
            handle_ss_dead_report(sock_fd, header);
            break;
        case MSG_CHECKPOINT:
            handle_checkpoint_request(sock_fd, header, client_username);
            break;
        case MSG_VIEWCHECKPOINT:
            handle_viewcheckpoint_request(sock_fd, header, client_username);
            break;
        case MSG_REVERT:
            handle_revert_request(sock_fd, header, client_username);
            break;
        case MSG_LISTCHECKPOINTS:
            handle_listcheckpoints_request(sock_fd, header, client_username);
            break;
        case MSG_LOCATE_FILE:
            handle_locate_file_request(sock_fd, header, client_username);
            break;
        default:
            write_log("WARN", "Socket %d: Received unknown msg_type: %d",
                      sock_fd, header->msg_type);
            send_error_to_client(sock_fd, "Unknown command.");
            break;
    }
}

// =========================================================================
//  MAIN CLIENT HANDLER FUNCTION
// =========================================================================

void handle_client_connection(int sock_fd, MessageHeader* initial_header) {
    char client_username[64];

    // 1. Authenticate (Register)
    if (initial_header->msg_type != MSG_REGISTER_CLIENT) {
        write_log("WARN", "Socket %d: First msg was %d, not MSG_REGISTER_CLIENT. Closing.",
                  sock_fd, initial_header->msg_type);
        send_error_to_client(sock_fd, "Must register username first.");
        close(sock_fd);
        return;
    }
    
    strncpy(client_username, initial_header->filename, 64);
    client_username[63] = '\0';
    write_log("CLIENT_HANDLER", "Client '%s' registered on socket %d.", 
              client_username, sock_fd);

    // 2. Send ACK for registration
    send_ack_to_client(sock_fd);

    // 3. Register user with the global user manager
    user_manager_register(client_username);
    
    // 4. Loop to receive all subsequent messages
    MessageHeader subsequent_header;
    int connection_alive = 1;
    while (connection_alive && recv_header(sock_fd, &subsequent_header) == 0) {
        
        if (subsequent_header.msg_type == MSG_EXEC) {
            connection_alive = 0; // The exec handler will take over
        }
        
        route_message(sock_fd, &subsequent_header, client_username);
    }
    
    // 5. If recv_header fails, the client disconnected
    if (connection_alive) { 
        write_log("CLIENT_HANDLER", "Client '%s' (Socket %d): Disconnected.", 
                  client_username, sock_fd);
        close(sock_fd);
    }
    
    // 6. Deregister user from the global list
    user_manager_deregister(client_username);
}