#include "common.h"
#include "logger.h"
#include "socket_utils.h"
#include "protocol.h"
#include "init.h"            // For init_server()
#include "client_handler.h"  // For routing
#include "storage_manager.h" // For routing

#include <pthread.h>
#include <stdlib.h> // For malloc, free
#include <unistd.h> // For close

/**
 * @brief The main function for each new thread.
 * Reads the first message to identify the component, then routes to the
 * appropriate handler function which will take over the connection.
 */
void* handle_connection(void* arg) {
    // 1. Get socket FD from argument and free the memory
    int sock_fd = *(int*)arg;
    free(arg);

    // 2. Detach thread so resources are freed automatically
    pthread_detach(pthread_self());

    write_log("THREAD", "New thread started to handle socket %d", sock_fd);

    // 3. Read the first header to identify the connection
    MessageHeader header;
    if (recv_header(sock_fd, &header) == -1) {
        // This handles clients disconnecting immediately or read errors
        write_log("THREAD", "Socket %d disconnected or failed to read header.", sock_fd);
        close(sock_fd);
        return NULL;
    }

    // 4. Route to the correct handler based on who is connecting
    switch (header.source_component) {
        case COMPONENT_STORAGE_SERVER:
            handle_storage_server_connection(sock_fd, &header);
            break;
        
        case COMPONENT_CLIENT:
            handle_client_connection(sock_fd, &header);
            break;
        
        default:
            write_log("WARN", "Socket %d sent unknown component type: %d. Closing.", 
                      sock_fd, header.source_component);
            close(sock_fd); // Unknown component, drop connection
            break;
    }

    // The handler functions are now responsible for the socket's lifecycle.
    // This thread's job is done.
    return NULL;
}

/**
 * @brief Main server entry point.
 */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ns_ip> <ns_port>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 5000\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char* ns_ip = argv[1];
    int ns_port = atoi(argv[2]);

    if (ns_port <= 1024 || ns_port > 65535) {
        fprintf(stderr, "Error: Port must be between 1025 and 65535.\n");
        exit(EXIT_FAILURE);
    }
    
    // 1. Initialization
    init_logger(ns_ip, ns_port);
    init_server(); // Call the function from init.c
    
    write_log("STARTUP", "Name Server starting...");

    // 2. Socket Setup
    int server_sock = create_socket();
    int opt = 1;
    if (setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        // We can continue, but it's good to know
    }
    // bind_socket and listen_socket will exit() on failure
    // as per your original socket_utils.c design
    bind_socket(server_sock, ns_port);
    listen_socket(server_sock); 
    
    write_log("STARTUP", "Server listening on %s:%d", ns_ip, ns_port);
    printf("Name Server is running on %s:%d...\n", ns_ip, ns_port);

    // 3. Main Accept Loop
    while(1) {
        int client_sock = accept_connection(server_sock);
        if (client_sock < 0) {
            write_log("ERROR", "Accept failed. Continuing...");
            continue; // Don't crash the server, just log and continue
        }

        write_log("ACCEPT", "Accepted new connection on socket %d", client_sock);

        // 4. Thread Creation
        pthread_t thread_id;
        
        // We must malloc memory for the socket FD to avoid a race condition
        // where the next loop iteration overwrites the variable
        int* p_client_sock = malloc(sizeof(int));
        if (p_client_sock == NULL) {
             write_log("FATAL", "Failed to allocate memory for thread argument.");
             close(client_sock);
             continue;
        }
        *p_client_sock = client_sock;

        // Create the new thread
        if (pthread_create(&thread_id, NULL, handle_connection, p_client_sock) != 0) {
            write_log("ERROR", "Failed to create thread for socket %d", client_sock);
            close(client_sock);
            free(p_client_sock); // Free memory if thread creation failed
        }
    }

    // 5. Cleanup (Server never actually reaches here)
    close(server_sock);
    close_logger();
    return 0;
}