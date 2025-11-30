#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include "protocol.h"
#include <pthread.h>

#define MAX_STORAGE_SERVERS 10
#define MAX_FILES_PER_SERVER 100

// This is the data structure for the SS registration payload
typedef struct {
    char ip_addr[64];
    int client_facing_port;
    // We can add the file list here later
} SSRegistrationPayload;


// This struct holds the server's state on the Name Server
typedef struct {
    int ss_socket_fd;
    char ip_addr[64];
    int client_facing_port;
    int is_active;
    pthread_mutex_t socket_mutex; // <-- ADD THIS
    // char file_list[MAX_FILES_PER_SERVER][MAX_FILENAME];
    // int file_count;
} StorageServerInfo;


// --- Global Data ---
// A global registry of all storage servers
extern StorageServerInfo ss_registry[MAX_STORAGE_SERVERS];
// A mutex to protect the global registry
extern pthread_mutex_t ss_registry_mutex;


// --- Functions ---

// Sets up the storage manager
void init_storage_manager();

// Handles the entire lifecycle of a storage server connection
void handle_storage_server_connection(int sock_fd, MessageHeader* initial_header);

// The core logic for registering a new server
int register_storage_server(int sock_fd, MessageHeader* header);

// Removes a storage server from the registry by its socket FD
void remove_storage_server(int sock_fd);

// Finds an active storage server for a new file (for CREATE)
StorageServerInfo* get_ss_for_new_file();

/**
 * @brief Gets a pointer to an active storage server by its index. (for READ)
 * @param ss_index The index in the ss_registry.
 * @return A pointer to the StorageServerInfo, or NULL if inactive/invalid.
 */
StorageServerInfo* get_ss_by_index(int ss_index); // <-- THIS IS THE NEW ONE


/**
 * @brief Finds an active storage server by its client-facing address.
 * @param ip The IP address.
 * @param port The client-facing port.
 * @return The internal socket_fd of the SS, or -1 if not found.
 */
int get_ss_sock_by_address(const char* ip, int port);

#endif // STORAGE_MANAGER_H