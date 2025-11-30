#include "protocol.h"
#include "common.h" // For <sys/socket.h>, <unistd.h>, etc.
#include "logger.h"

/**
 * @brief Helper function to reliably send an exact number of bytes.
 * Handles partial sends in a loop.
 */
int send_all(int socket_fd, const void *buf, size_t len) {
    const char *ptr = (const char *)buf;
    size_t total_sent = 0;

    while (total_sent < len) {
        ssize_t bytes_sent = send(socket_fd, ptr + total_sent, len - total_sent, 0);
        
        if (bytes_sent < 0) {
            perror("send failed");
            return -1; // Send error
        }
        if (bytes_sent == 0) {
            // This shouldn't happen for a blocking TCP socket,
            // but if it does, it means the connection is closed.
            write_log("WARN", "send_all: Connection closed by peer.");
            return -1; 
        }
        
        total_sent += (size_t)bytes_sent;
    }

    return 0; // Success
}

/**
 * @brief Helper function to reliably receive an exact number of bytes.
 * Handles partial receives in a loop.
 */
int recv_all(int socket_fd, void *buf, size_t len) {
    char *ptr = (char *)buf;
    size_t total_received = 0;

    while (total_received < len) {
        ssize_t bytes_received = recv(socket_fd, ptr + total_received, len - total_received, 0);

        if (bytes_received < 0) {
            perror("recv failed");
            return -1; // Receive error
        }
        if (bytes_received == 0) {
            // Connection closed gracefully by the peer
            write_log("WARN", "recv_all: Connection closed by peer.");
            return -1;
        }

        total_received += (size_t)bytes_received;
    }

    return 0; // Success
}

/**
 * @brief Sends just the message header.
 */
int send_header(int socket_fd, MessageHeader *header) {
    return send_all(socket_fd, header, sizeof(MessageHeader));
}

/**
 * @brief Receives just the message header.
 */
int recv_header(int socket_fd, MessageHeader *header) {
    return recv_all(socket_fd, header, sizeof(MessageHeader));
}