#ifndef CLIENT_HANDLER_H
#define CLIENT_HANDLER_H

#include "protocol.h" // For MessageHeader
#include "executor.h" // For handle_exec_request

/**
 * @brief Handles the entire lifecycle of a client connection.
 * This is the main function for the client's thread.
 */
void handle_client_connection(int sock_fd, MessageHeader* initial_header);

/**
 * @brief Handles a MSG_CREATE request from a client.
 */
void handle_create_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_READ request from a client.
 */
void handle_read_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_ADD_ACCESS request from a client.
 */
void handle_add_access(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_REM_ACCESS request from a client.
 */
void handle_rem_access(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_DELETE request from a client.
 */
void handle_delete_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_WRITE request from a client (redirects to SS).
 */
void handle_write_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_STREAM request from a client (redirects to SS).
 */
void handle_stream_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_UNDO request from a client.
 */
void handle_undo_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_INFO request from a client.
 */
void handle_info_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_LIST request from a client.
 */
void handle_list_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_VIEW request from a client.
 */
void handle_view_request(int sock_fd, MessageHeader* header, const char* client_username);

/* Folder-related handlers */
void handle_create_folder_request(int sock_fd, MessageHeader* header, const char* client_username);
void handle_move_file_request(int sock_fd, MessageHeader* header, const char* client_username);
void handle_move_folder_request(int sock_fd, MessageHeader* header, const char* client_username);
void handle_view_folder_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_SS_DEAD_REPORT from a client.
 */
void handle_ss_dead_report(int sock_fd, MessageHeader* header);

/**
 * @brief Handles a MSG_CHECKPOINT request from a client.
 */
void handle_checkpoint_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_VIEWCHECKPOINT request from a client.
 */
void handle_viewcheckpoint_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_REVERT request from a client.
 */
void handle_revert_request(int sock_fd, MessageHeader* header, const char* client_username);

/**
 * @brief Handles a MSG_LISTCHECKPOINTS request from a client.
 */
void handle_listcheckpoints_request(int sock_fd, MessageHeader* header, const char* client_username);

#endif // CLIENT_HANDLER_H