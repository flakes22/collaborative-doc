#include "executor.h"
#include "logger.h"
#include "protocol.h"
#include "search.h"
#include "storage_manager.h"
#include "socket_utils.h" // For connect_socket_no_exit (though we're removing it)

#include <stdio.h>    // For popen, pclose, fgets
#include <string.h>
#include <unistd.h>   // For close()
#include <stdlib.h>   // For malloc/free
#include <pthread.h>  // For pthread_mutex_lock/unlock
#include "common.h"

#define EXEC_BUFFER_SIZE 4096

// Helper function from client_handler (we should move this to common)
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

/**
 * @brief Handles a MSG_EXEC request from a client.
 */
void handle_exec_request(int client_sock_fd, MessageHeader* header, const char* client_username) {
    write_log("CLIENT_CMD", "User '%s' (Socket %d): Received MSG_EXEC for file '%s'",
              client_username, client_sock_fd, header->filename);

    // 1. Check permissions
    if (!search_check_permission(header->filename, client_username, PERM_READ)) {
        send_error_to_client(client_sock_fd, "Access Denied (Read Permission Required).");
        close(client_sock_fd);
        return;
    }

    // 2. Find the file
    int ss_index = search_find_file(header->filename);
    if (ss_index == -1) {
        send_error_to_client(client_sock_fd, "File not found.");
        close(client_sock_fd);
        return;
    }
    
    // 3. Get the SS info
    StorageServerInfo* ss = get_ss_by_index(ss_index);
    if (ss == NULL || !ss->is_active) {
        send_error_to_client(client_sock_fd, "File is on an inactive server.");
        close(client_sock_fd);
        return;
    }

    // 4. --- NEW LOGIC: Use the EXISTING SS socket ---
    int ss_sock = ss->ss_socket_fd; // Get the socket from registration
    
    // LOCK SS SOCKET TO ENSURE EXCLUSIVE ACCESS
    pthread_mutex_lock(&ss->socket_mutex);
    
    // 5. Send the internal read request
    MessageHeader req_header;
    memset(&req_header, 0, sizeof(req_header));
    req_header.msg_type = MSG_INTERNAL_READ;
    req_header.source_component = COMPONENT_NAME_SERVER;
    strncpy(req_header.filename, header->filename, MAX_FILENAME - 1);

    if (send_header(ss_sock, &req_header) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(client_sock_fd, "Failed to send INTERNAL_READ to SS.");
        remove_storage_server(ss_sock); // The SS is disconnected
        close(client_sock_fd);
        return;
    }

    // 6. Receive the response header (MSG_INTERNAL_DATA)
    MessageHeader resp_header;
    if (recv_header(ss_sock, &resp_header) == -1 || resp_header.msg_type != MSG_INTERNAL_DATA) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(client_sock_fd, "Did not receive valid INTERNAL_DATA from SS.");
        remove_storage_server(ss_sock); // The SS is disconnected
        close(client_sock_fd);
        return;
    }

    // 7. Receive the payload (file content)
    char* file_content = malloc(resp_header.payload_length + 1);
    if (file_content == NULL) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(client_sock_fd, "Internal server error (malloc).");
        close(client_sock_fd);
        return;
    }

    if (recv_all(ss_sock, file_content, resp_header.payload_length) == -1) {
        pthread_mutex_unlock(&ss->socket_mutex);
        send_error_to_client(client_sock_fd, "Failed to receive file content from SS.");
        remove_storage_server(ss_sock);
        free(file_content);
        close(client_sock_fd);
        return;
    }
    file_content[resp_header.payload_length] = '\0';
    
    pthread_mutex_unlock(&ss->socket_mutex);
    // --- END OF NEW LOGIC ---

    write_log("EXEC", "Executing command: \"%s\"", file_content);

    // 8. Execute the content using popen
    FILE* pipe = popen(file_content, "r");
    free(file_content); // We're done with this buffer
    
    if (pipe == NULL) {
        send_error_to_client(client_sock_fd, "Failed to execute command on server.");
        close(client_sock_fd);
        return;
    }

    // 9. Pipe the output of popen directly to the client
    char pipe_buffer[EXEC_BUFFER_SIZE];
    while (fgets(pipe_buffer, sizeof(pipe_buffer), pipe) != NULL) {
        if (send_all(client_sock_fd, pipe_buffer, strlen(pipe_buffer)) == -1) {
            write_log("WARN", "[EXEC] Client disconnected during output stream.");
            break; // Stop streaming
        }
    }

    // 10. Cleanup
    pclose(pipe);
    close(client_sock_fd); // The EXEC command is a one-shot, close connection after.
    write_log("EXEC", "Execution and streaming complete for socket %d.", client_sock_fd);
}