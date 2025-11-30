#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "protocol.h"

/**
 * @brief Handles a MSG_EXEC request from a client.
 * This is the full orchestration function.
 */
void handle_exec_request(int client_sock_fd, MessageHeader* header, const char* client_username);

#endif // EXECUTOR_H