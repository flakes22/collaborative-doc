#include "socket_utils.h"
#include "common.h"

// Create socket safely
int create_socket() {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    return sockfd;
}

// Bind a socket to a given port (for servers)
void bind_socket(int sockfd, int port) {
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("Bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
}

// Listen for incoming connections
void listen_socket(int sockfd) {
    if (listen(sockfd, 5) < 0) {
        perror("Listen failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
}

// Accept an incoming connection
int accept_connection(int sockfd) {
    struct sockaddr_in client;
    socklen_t len = sizeof(client);
    int new_sock = accept(sockfd, (struct sockaddr*)&client, &len);
    if (new_sock < 0) {
        perror("Accept failed");
        return -1;
    }
    return new_sock;
}

// Connect to a remote server (for clients)
void connect_socket(int sockfd, const char *ip, int port) {
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address");
        exit(EXIT_FAILURE);
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        exit(EXIT_FAILURE);
    }
}


// Connect to a remote server (for clients) - DOES NOT EXIT
// Returns 0 on success, -1 on failure
int connect_socket_no_exit(int sockfd, const char *ip, int port) {
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &serv_addr.sin_addr) <= 0) {
        perror("Invalid address");
        return -1; // Return -1 instead of exiting
    }

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Connection failed");
        return -1; // Return -1 instead of exiting
    }
    
    return 0; // Success
}
