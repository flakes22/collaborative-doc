#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

// This header only needs to declare the functions.
// The .c file handles the includes like <arpa/inet.h>

/**
 * @brief Creates a new TCP socket.
 * Exits on failure.
 * @return The socket file descriptor.
 */
int create_socket();

/**
 * @brief Binds a socket to INADDR_ANY and a specified port.
 * Exits on failure.
 * @param sockfd The socket file descriptor.
 * @param port The port number to bind to.
 */
void bind_socket(int sockfd, int port);

/**
 * @brief Sets a socket to listen for incoming connections.
 * Exits on failure.
 * @param sockfd The socket file descriptor.
 */
void listen_socket(int sockfd);

/**
 * @brief Accepts a new connection on a listening socket.
 * @param sockfd The listening socket file descriptor.
 * @return The new client socket file descriptor, or -1 on failure.
 */
int accept_connection(int sockfd);

/**
 * @brief Connects a socket to a remote server.
 * Exits on failure.
 * @param sockfd The socket file descriptor.
 * @param ip The IP address of the server (e.g., "12_7.0.0.1").
 * @param port The port number of the server.
 */
void connect_socket(int sockfd, const char *ip, int port);

/**
 * @brief Connects a socket to a remote server.
 * DOES NOT EXIT on failure.
 * @param sockfd The socket file descriptor.
 * @param ip The IP address of the server.
 * @param port The port number of the server.
 * @return 0 on success, -1 on failure.
 */
int connect_socket_no_exit(int sockfd, const char *ip, int port);

#endif // SOCKET_UTILS_H