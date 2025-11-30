#include "storage_manager.h"
#include "logger.h"
#include <string.h>
#include <unistd.h> // for close()
#include "search.h"

// --- Global Data Definitions ---
StorageServerInfo ss_registry[MAX_STORAGE_SERVERS];
pthread_mutex_t ss_registry_mutex;

static int next_ss_index = 0;
/**
 * @brief Initializes the storage server registry and its mutex.
 */
void init_storage_manager() {
    pthread_mutex_init(&ss_registry_mutex, NULL);
    for (int i = 0; i < MAX_STORAGE_SERVERS; i++) {
        ss_registry[i].is_active = 0;
        ss_registry[i].ss_socket_fd = -1;
        pthread_mutex_init(&ss_registry[i].socket_mutex, NULL);
    }
    write_log("INIT", "Storage Manager initialized.");
}

/**
 * @brief The core logic for registering a new server.
 * This is called by the connection handler thread.
 */
int register_storage_server(int sock_fd, MessageHeader* header) {
    // 1. Check payload length to make sure it matches our struct
    if (header->payload_length != sizeof(SSRegistrationPayload)) {
        write_log("ERROR", "SS %d: Bad registration packet size. Got %u, expected %lu",
                  sock_fd, header->payload_length, sizeof(SSRegistrationPayload));
        return -1;
    }

    // 2. Receive the registration payload
    SSRegistrationPayload payload;
    if (recv_all(sock_fd, &payload, sizeof(payload)) == -1) {
        write_log("ERROR", "SS %d: Failed to receive registration payload.", sock_fd);
        return -1;
    }

    // 3. Lock the registry to find a free slot
    pthread_mutex_lock(&ss_registry_mutex);

    int found_slot = -1;
    for (int i = 0; i < MAX_STORAGE_SERVERS; i++) {
        if (!ss_registry[i].is_active) {
            found_slot = i;
            break;
        }
    }

    if (found_slot == -1) {
        pthread_mutex_unlock(&ss_registry_mutex);
        write_log("ERROR", "SS %d: No free slots in registry. Registration failed.", sock_fd);
        return -1;
    }

    // 4. Fill the slot with the new server's info
    ss_registry[found_slot].is_active = 1;
    ss_registry[found_slot].ss_socket_fd = sock_fd;
    ss_registry[found_slot].client_facing_port = payload.client_facing_port;
    strncpy(ss_registry[found_slot].ip_addr, payload.ip_addr, 64);
    // TODO: Receive and store the file list

    pthread_mutex_unlock(&ss_registry_mutex);

    write_log("INFO", "Storage Server registered successfully on slot %d (Socket %d)",
              found_slot, sock_fd);
    
    // 5. Send ACK back to the Storage Server
    MessageHeader ack_header;
    memset(&ack_header, 0, sizeof(ack_header));
    ack_header.msg_type = MSG_ACK;
    ack_header.source_component = COMPONENT_NAME_SERVER;
    ack_header.dest_component = COMPONENT_STORAGE_SERVER;
    ack_header.payload_length = 0;
    
    if (send_header(sock_fd, &ack_header) == -1) {
        write_log("ERROR", "SS %d: Failed to send ACK.", sock_fd);
        // We're registered, but SS might not know. Let's disconnect.
        return -1;
    }

    return found_slot; // Success
}

void handle_storage_server_connection(int sock_fd, MessageHeader* initial_header) {
    write_log("SS_HANDLER", "New SS connection on socket %d. Initial msg_type: %d",
              sock_fd, initial_header->msg_type);
    
    int ss_index = -1;

    // 1. Register the server and get its assigned slot index
    if (initial_header->msg_type == MSG_REGISTER) {
        ss_index = register_storage_server(sock_fd, initial_header);
        if (ss_index == -1) {
            write_log("SS_HANDLER", "SS %d: Registration failed. Closing.", sock_fd);
            close(sock_fd);
            return;
        }
    } else {
        write_log("SS_HANDLER", "SS %d: Sent msg %d instead of MSG_REGISTER. Closing.",
                  sock_fd, initial_header->msg_type);
        close(sock_fd);
        return;
    }

    // 2. Send ACK for registration and wait for file list
    MessageHeader ack_header;
    memset(&ack_header, 0, sizeof(ack_header));
    ack_header.msg_type = MSG_ACK;
    ack_header.source_component = COMPONENT_NAME_SERVER;
    ack_header.dest_component = COMPONENT_STORAGE_SERVER;
    if (send_header(sock_fd, &ack_header) == -1) {
        // This is a passive disconnect. The SS connected and immediately died.
        write_log("SS_HANDLER", "SS %d (Slot %d): Failed to send ACK. Closing.", sock_fd, ss_index);
        remove_storage_server(sock_fd); // Purge
        close(sock_fd);
        return;
    }
    write_log("SS_HANDLER", "SS %d (Slot %d): Awaiting file list...", sock_fd, ss_index);

    // 3. File Sync Loop
    MessageHeader file_header;
    while (recv_header(sock_fd, &file_header) == 0) {
        
        if (file_header.msg_type == MSG_REGISTER_FILE) {
            SSFileRecordPayload file_payload;
            if (file_header.payload_length != sizeof(SSFileRecordPayload) ||
                recv_all(sock_fd, &file_payload, sizeof(file_payload)) == -1) {
                write_log("ERROR", "SS %d: Bad payload for MSG_REGISTER_FILE. Closing.", sock_fd);
                goto disconnect; // Go to cleanup
            }
            // Log received payload values for debugging word/char counts
            write_log("DEBUG", "Received REGISTER_FILE from SS %d: filename=%s, word_count=%ld, char_count=%ld, last_accessed=%ld",
                      ss_index, file_payload.filename, file_payload.word_count, file_payload.char_count, (long)file_payload.last_accessed);

            search_rebuild_add_file(ss_index, &file_payload);

        } else if (file_header.msg_type == MSG_REGISTER_COMPLETE) {
            write_log("SS_HANDLER", "SS %d (Slot %d): File list sync complete.", sock_fd, ss_index);
            goto complete; // Exit the loop
        
        } else {
            write_log("WARN", "SS %d: Sent unexpected msg %d during file sync. Closing.",
                      sock_fd, file_header.msg_type);
            goto disconnect; // Go to cleanup
        }
    }

disconnect:
    // We land here if recv_header failed
    write_log("SS_HANDLER", "SS %d (Slot %d): Disconnected during file sync.", sock_fd, ss_index);
    remove_storage_server(sock_fd); // Purge all its files
    close(sock_fd);
    return;

complete:
    // We land here on a successful registration
    // This thread's job is done. It EXITS, leaving the socket open
    // and idle in the registry, protected by its mutex.
    write_log("SS_HANDLER", "SS %d (Slot %d): Registration complete. Thread exiting.", 
              sock_fd, ss_index);

    
}
/**
 * @brief Finds an active storage server for a new file (Round-Robin).
 * NOTE: This function MUST be called while the ss_registry_mutex is HELD.
 */
static StorageServerInfo* find_next_active_ss(int start_index) {
    for (int i = 0; i < MAX_STORAGE_SERVERS; i++) {
        int index = (start_index + i) % MAX_STORAGE_SERVERS;
        if (ss_registry[index].is_active) {
            // Found one. Update the next_ss_index for the *next* call.
            next_ss_index = (index + 1) % MAX_STORAGE_SERVERS;
            return &ss_registry[index];
        }
    }
    return NULL; // No active servers
}

/**
 * @brief Public function to get an available SS for a new file.
 */
StorageServerInfo* get_ss_for_new_file() {
    pthread_mutex_lock(&ss_registry_mutex);
    
    StorageServerInfo* ss = find_next_active_ss(next_ss_index);
    
    pthread_mutex_unlock(&ss_registry_mutex);

    if (ss == NULL) {
        write_log("ERROR", "get_ss_for_new_file: No active storage servers found!");
    }
    
    return ss;
}

/**
 * @brief Gets a pointer to an active storage server by its index.
 */
StorageServerInfo* get_ss_by_index(int ss_index) {
    if (ss_index < 0 || ss_index >= MAX_STORAGE_SERVERS) {
        return NULL;
    }

    // We don't need to lock here for a simple read,
    // but if we were modifying the struct, we would.
    // A quick check for activity is good.
    if (ss_registry[ss_index].is_active) {
        return &ss_registry[ss_index];
    }

    return NULL;
}

/**
 * @brief Finds and deactivates a server from the registry by its socket.
 * This will also call the search module to purge all files.
 */
void remove_storage_server(int sock_fd) {
    int ss_index = -1;

    pthread_mutex_lock(&ss_registry_mutex);

    for (int i = 0; i < MAX_STORAGE_SERVERS; i++) {
        if (ss_registry[i].is_active && ss_registry[i].ss_socket_fd == sock_fd) {
            ss_registry[i].is_active = 0;
            ss_registry[i].ss_socket_fd = -1;
            ss_index = i; 
            write_log("STORAGE_MGR", "Removed Storage Server (socket %d) from slot %d", sock_fd, i);
            break;
        }
    }

    pthread_mutex_unlock(&ss_registry_mutex);

    if (ss_index != -1) {
        // This is just a CALL to the function.
        search_purge_by_ss(ss_index); 
    }
}

/**
 * @brief Finds an active storage server by its client-facing address.
 */
int get_ss_sock_by_address(const char* ip, int port) {
    int sock_fd = -1;
    pthread_mutex_lock(&ss_registry_mutex);

    for (int i = 0; i < MAX_STORAGE_SERVERS; i++) {
        if (ss_registry[i].is_active &&
            ss_registry[i].client_facing_port == port &&
            strcmp(ss_registry[i].ip_addr, ip) == 0) 
        {
            sock_fd = ss_registry[i].ss_socket_fd;
            break;
        }
    }

    pthread_mutex_unlock(&ss_registry_mutex);
    return sock_fd;
}