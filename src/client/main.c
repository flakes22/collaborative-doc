/*
 * =================================================================
 * FINAL INTEGRATED Client main.c
 * - Speaks the binary 'protocol.h' protocol to the Name Server.
 * - Speaks the text-based protocol to the Storage Server.
 * =================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h> // For ctime_r, strftime, and nanosleep

// Headers from 'common'
#include "../../include/common.h"
#include "../../include/logger.h"
#include "../../include/protocol.h"
#include "../../include/socket_utils.h"

// --- Defines ---
#define BUF_SZ 8192 // Larger buffer for file reads

// --- Globals ---
static int g_ns_socket = -1; // Persistent connection to Name Server
static char g_username[64];
static char* g_ns_ip_global; // For EXEC reconnect
static int g_ns_port_global; // For EXEC reconnect


// --- Function Prototypes ---
void command_loop();
int connect_and_login(const char* ns_ip, int ns_port, const char* username);

// Command Handlers
void handle_proxy_command(int msg_type, const char* filename, const char* success_msg);
void handle_redirect_command(int msg_type, const char* filename, int sentence_num);
void handle_list_command();
void handle_view_command(int flags);
void handle_info_command(const char* filename);
void handle_access_command(int msg_type, const char* filename, const char* target_user, int permission);
void handle_exec_command(const char* filename);
void handle_checkpoint_command(const char* filename, const char* checkpoint_tag);
void handle_viewcheckpoint_command(const char* filename, const char* checkpoint_tag);
void handle_revert_command(const char* filename, const char* checkpoint_tag);
void handle_listcheckpoints_command(const char* filename);
void handle_requestaccess_command(const char* filename, const char* permission);
void handle_viewrequests_command(const char* filename);
void handle_approverequest_command(const char* filename, const char* username, const char* permission);
void handle_denyrequest_command(const char* filename, const char* username);


/**
 * @brief Main entry point. Connects, logs in, and starts command loop.
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

    char username[64];
    printf("Enter username: ");
    fflush(stdout);
    
    if (fgets(username, sizeof(username), stdin) == NULL) {
        fprintf(stderr, "Error reading username.\n");
        exit(EXIT_FAILURE);
    }
    
    // Remove newline character from username
    username[strcspn(username, "\n")] = '\0';
    
    // Validate username (not empty)
    if (strlen(username) == 0) {
        fprintf(stderr, "Username cannot be empty.\n");
        exit(EXIT_FAILURE);
    }

    // Store connection details globally for EXEC reconnect
    // Note: We need to allocate memory for these since they're no longer argv
    g_ns_ip_global = malloc(strlen(ns_ip) + 1);
    strcpy(g_ns_ip_global, ns_ip);
    g_ns_port_global = ns_port;

    init_logger(ns_ip, ns_port); // Updated to use the NS IP and port

    if (connect_and_login(ns_ip, ns_port, username) != 0) {
        printf("Failed to login to Name Server. Exiting.\n");
        exit(EXIT_FAILURE);
    }

    printf("Welcome, %s! You are connected to the Name Server at %s:%d.\n", username, ns_ip, ns_port);
    printf("Type 'help' for commands or 'exit' to quit.\n");

    command_loop();

    printf("Logging out...\n");
    close(g_ns_socket);
    close_logger();
    return 0;
}

/**
 * @brief Connects to the NS and performs the "login" (MSG_REGISTER_CLIENT)
 * @return 0 on success, -1 on failure.
 */
int connect_and_login(const char* ns_ip, int ns_port, const char* username) {
    g_ns_socket = create_socket();
    
    // connect_socket exits on failure
    connect_socket(g_ns_socket, ns_ip, ns_port);
    write_log("INFO", "Connected to Name Server.");
    strncpy(g_username, username, 64);
    g_username[63] = '\0';

    // 1. Prepare login header
    MessageHeader login_header;
    memset(&login_header, 0, sizeof(login_header));
    login_header.msg_type = MSG_REGISTER_CLIENT;
    login_header.source_component = COMPONENT_CLIENT;
    strncpy(login_header.filename, username, MAX_FILENAME - 1); // Send username

    if (send_header(g_ns_socket, &login_header) == -1) {
        write_log("FATAL", "Failed to send login header.");
        return -1;
    }

    // 2. Wait for ACK
    MessageHeader ack_header;
    if (recv_header(g_ns_socket, &ack_header) == -1) {
        write_log("FATAL", "Server disconnected during login.");
        return -1;
    }

    if (ack_header.msg_type == MSG_ACK) {
        write_log("INFO", "Successfully logged in as '%s'", username);
        return 0;
    } else {
        write_log("FATAL", "Name Server did not ACK login. (Got %d)", ack_header.msg_type);
        printf("Name Server rejected login: %s\n", ack_header.filename);
        return -1;
    }
}

/**
 * @brief Main command input loop
 */
void command_loop() {
    char line_buffer[MAX_BUFFER];
    while (1) {
        printf("%s > ", g_username);
        if (fgets(line_buffer, sizeof(line_buffer), stdin) == NULL) {
            break; // EOF (Ctrl+D)
        }
        
        line_buffer[strcspn(line_buffer, "\n")] = 0;
        char cmd[64], arg1[MAX_FILENAME], arg2[MAX_BUFFER], arg3[MAX_BUFFER];
        
        memset(cmd, 0, sizeof(cmd));
        memset(arg1, 0, sizeof(arg1));
        memset(arg2, 0, sizeof(arg2));
        memset(arg3, 0, sizeof(arg3));
        
        // Use sscanf to parse input
        sscanf(line_buffer, "%63s %255s %1023s %1023s", cmd, arg1, arg2, arg3);

        if (strlen(cmd) == 0) continue;

        if (strcmp(cmd, "EXIT") == 0) {
            break;
        } 
        else if (strcmp(cmd, "LIST") == 0) {
            handle_list_command();
        }
        else if (strcmp(cmd, "CREATE") == 0) {
            if (strlen(arg1) == 0) printf("Usage: create <filename>\n");
            else handle_proxy_command(MSG_CREATE, arg1, "File created successfully.");
        }
        else if (strcmp(cmd, "DELETE") == 0) {
            if (strlen(arg1) == 0) printf("Usage: delete <filename>\n");
            else handle_proxy_command(MSG_DELETE, arg1, "File deleted successfully.");
        }
        else if (strcmp(cmd, "UNDO") == 0) {
            if (strlen(arg1) == 0) printf("Usage: undo <filename>\n");
            else handle_proxy_command(MSG_UNDO, arg1, "Undo successful.");
        }
        else if (strcmp(cmd, "READ") == 0) {
            if (strlen(arg1) == 0) printf("Usage: read <filename>\n");
            else handle_redirect_command(MSG_READ, arg1, 0);
        }
         else if (strcmp(cmd, "STREAM") == 0) {
            if (strlen(arg1) == 0) printf("Usage: stream <filename>\n");
            else handle_redirect_command(MSG_STREAM, arg1, 0);
        }
        else if (strcmp(cmd, "WRITE") == 0) {
            int sent_num = atoi(arg2);
            if (strlen(arg1) == 0 || sent_num == 0) printf("Usage: write <filename> <sentence_number>\n");
            else handle_redirect_command(MSG_WRITE, arg1, sent_num);
        }
        else if (strcmp(cmd, "EXEC") == 0) {
            if (strlen(arg1) == 0) printf("Usage: exec <filename>\n");
            else handle_exec_command(arg1);
        }
        else if (strcmp(cmd, "INFO") == 0) {
            if (strlen(arg1) == 0) printf("Usage: info <filename>\n");
            else handle_info_command(arg1);
        }
        else if (strcmp(cmd, "VIEW") == 0) {
            int flags = 0;
            if (strcmp(arg1, "-a") == 0) flags |= VIEW_FLAG_ALL;
            else if (strcmp(arg1, "-l") == 0) flags |= VIEW_FLAG_LONG;
            else if (strcmp(arg1, "-al") == 0 || strcmp(arg1, "-la") == 0) flags |= (VIEW_FLAG_ALL | VIEW_FLAG_LONG);
            handle_view_command(flags);
        }
        else if (strcmp(cmd, "CREATEFOLDER") == 0) {
            if (strlen(arg1) == 0) printf("Usage: createfolder <foldername>\n");
            else {
                MessageHeader header;
                memset(&header, 0, sizeof(header));
                header.msg_type = MSG_CREATE_FOLDER;
                header.source_component = COMPONENT_CLIENT;
                strncpy(header.filename, arg1, MAX_FILENAME - 1);
                if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                MessageHeader resp;
                if (recv_header(g_ns_socket, &resp) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                if (resp.msg_type == MSG_ACK) printf("Folder created successfully.\n"); else printf("Error: %s\n", resp.filename);
            }
        }
        else if (strcmp(cmd, "MOVE") == 0) {
            if (strlen(arg1) == 0 || strlen(arg2) == 0) printf("Usage: move <filename> <folder>\n");
            else {
                MessageHeader header;
                memset(&header, 0, sizeof(header));
                header.msg_type = MSG_MOVE_FILE;
                header.source_component = COMPONENT_CLIENT;
                strncpy(header.filename, arg1, MAX_FILENAME - 1);
                header.payload_length = strlen(arg2) + 1;
                if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                if (send_all(g_ns_socket, arg2, header.payload_length) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                MessageHeader resp;
                if (recv_header(g_ns_socket, &resp) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                if (resp.msg_type == MSG_ACK) printf("Move completed.\n"); else printf("Error: %s\n", resp.filename);
            }
        }
        else if (strcmp(cmd, "MOVEFOLDER") == 0) {
            if (strlen(arg1) == 0 || strlen(arg2) == 0) printf("Usage: movefolder <src> <dst>\n");
            else {
                MessageHeader header;
                memset(&header, 0, sizeof(header));
                header.msg_type = MSG_MOVE_FOLDER;
                header.source_component = COMPONENT_CLIENT;
                strncpy(header.filename, arg1, MAX_FILENAME - 1);
                header.payload_length = strlen(arg2) + 1;
                if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                if (send_all(g_ns_socket, arg2, header.payload_length) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                MessageHeader resp;
                if (recv_header(g_ns_socket, &resp) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                if (resp.msg_type == MSG_ACK) printf("Folder moved successfully.\n"); else printf("Error: %s\n", resp.filename);
            }
        }
        else if (strcmp(cmd, "VIEWFOLDER") == 0) {
            if (strlen(arg1) == 0) { printf("Usage: viewfolder <folder> [-l|-a]\n"); }
            else {
                int flags = 0;
                if (strcmp(arg2, "-a") == 0) flags |= VIEW_FLAG_ALL;
                if (strcmp(arg2, "-l") == 0) flags |= VIEW_FLAG_LONG;

                MessageHeader header;
                memset(&header, 0, sizeof(header));
                header.msg_type = MSG_VIEWFOLDER;
                header.source_component = COMPONENT_CLIENT;
                header.payload_length = sizeof(ViewFolderPayload);

                ViewFolderPayload payload;
                payload.flags = flags;
                strncpy(payload.folder, arg1, MAX_FILENAME - 1);

                if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                if (send_all(g_ns_socket, &payload, sizeof(payload)) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }

                MessageHeader resp;
                if (recv_header(g_ns_socket, &resp) == -1) { write_log("ERROR", "Connection to NS lost."); continue; }
                if (resp.msg_type == MSG_VIEW_RESPONSE) {
                    if (resp.payload_length == 0) { printf("(No entries)\n"); continue; }
                    char* buf = malloc(resp.payload_length + 1);
                    if (!buf) { printf("Internal error\n"); continue; }
                    if (recv_all(g_ns_socket, buf, resp.payload_length) == -1) { free(buf); continue; }
                    buf[resp.payload_length] = '\0';
                    if (flags & VIEW_FLAG_LONG) {
                        printf("---------------------------------------------------------------\n");
                        printf("| T |  Filename   | Words | Chars | Last Access Time  | Owner  |\n");
                        printf("|---|-------------|-------|-------|-------------------|--------|\n");
                        printf("%s", buf);
                        printf("---------------------------------------------------------------\n");
                    } else {
                        printf("%s", buf);
                    }
                    free(buf);
                } else {
                    printf("Error: %s\n", resp.filename);
                }
            }
        }
        else if (strcmp(cmd, "ADDACCESS") == 0) {
            int perm = (strcmp(arg2, "-W") == 0) ? PERM_WRITE : PERM_READ;
            if (strlen(arg3) == 0) printf("Usage: addaccess <filename> -R/-W <username>\n");
            else handle_access_command(MSG_ADD_ACCESS, arg1, arg3, perm);
        }
        else if (strcmp(cmd, "REMACCESS") == 0) {
            if (strlen(arg2) == 0) printf("Usage: remaccess <filename> <username>\n");
            else handle_access_command(MSG_REM_ACCESS, arg1, arg2, 0);
        }
        else if (strcmp(cmd, "CHECKPOINT") == 0) {
            if (strlen(arg1) == 0 || strlen(arg2) == 0) {
                printf("Usage: checkpoint <filename> <tag>\n");
            } else {
                handle_checkpoint_command(arg1, arg2);
            }
        }
        else if (strcmp(cmd, "VIEWCHECKPOINT") == 0) {
            if (strlen(arg1) == 0 || strlen(arg2) == 0) {
                printf("Usage: viewcheckpoint <filename> <tag>\n");
            } else {
                handle_viewcheckpoint_command(arg1, arg2);
            }
        }
        else if (strcmp(cmd, "REVERT") == 0) {
            if (strlen(arg1) == 0 || strlen(arg2) == 0) {
                printf("Usage: revert <filename> <tag>\n");
            } else {
                handle_revert_command(arg1, arg2);
            }
        }
        else if (strcmp(cmd, "LISTCHECKPOINTS") == 0) {
            if (strlen(arg1) == 0) {
                printf("Usage: listcheckpoints <filename>\n");
            } else {
                handle_listcheckpoints_command(arg1);
            }
        }
        else if (strcmp(cmd, "REQUESTACCESS") == 0) {
            if (strlen(arg1) == 0 || strlen(arg2) == 0) {
                printf("Usage: requestaccess <filename> <-R/-W>\n");
            } else {
                handle_requestaccess_command(arg1, arg2);
            }
        }
        else if (strcmp(cmd, "VIEWREQUESTS") == 0) {
            handle_viewrequests_command(strlen(arg1) > 0 ? arg1 : NULL);
        }
        else if (strcmp(cmd, "APPROVEREQUEST") == 0) {
            if (strlen(arg1) == 0 || strlen(arg2) == 0 || strlen(arg3) == 0) {
                printf("Usage: approverequest <filename> <username> <-R/-W>\n");
            } else {
                handle_approverequest_command(arg1, arg2, arg3);
            }
        }
        else if (strcmp(cmd, "DENYREQUEST") == 0) {
            if (strlen(arg1) == 0 || strlen(arg2) == 0) {
                printf("Usage: denyrequest <filename> <username>\n");
            } else {
                handle_denyrequest_command(arg1, arg2);
            }
        }
        else if (strcmp(cmd, "help") == 0) {
            printf("--- Available Commands ---\n");
            printf("  create <file>\n");
            printf("  read <file>\n");
            printf("  write <file> <sent_#>\n");
            printf("  delete <file>\n");
            printf("  undo <file>\n");
            printf("  stream <file>\n");
            printf("  exec <file>\n");
            printf("  info <file>\n");
            printf("  view [-a, -l, -al]\n");
            printf("  list\n");
            printf("  addaccess <file> <-R/-W> <user>\n");
            printf("  remaccess <file> <user>\n");
            printf("  checkpoint <file> <tag>\n");
            printf("  viewcheckpoint <file> <tag>\n");
            printf("  revert <file> <tag>\n");
            printf("  listcheckpoints <file>\n");
            printf("  createfolder <foldername>\n");
            printf("  move <file> <folder>\n");
            printf("  movefolder <src_folder> <dst_folder>\n");
            printf("  requestaccess <file> <-R/-W>\n");
            printf("  viewrequests [file]\n");
            printf("  approverequest <file> <username> <-R/-W>\n");
            printf("  denyrequest <file> <username>\n");
            printf("  exit\n");
        }
        else {
            printf("Unknown command. Type 'help' for a list.\n");
        }
    }
}

/**
 * @brief Generic handler for simple proxy commands
 * (CREATE, DELETE, UNDO)
 */
void handle_proxy_command(int msg_type, const char* filename, const char* success_msg) {
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = msg_type;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);

    if (send_header(g_ns_socket, &header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }

    // Wait for final ACK from NS
    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }

    if (resp_header.msg_type == MSG_ACK) {
        printf("%s\n", success_msg);
    } else {
        printf("Error: %s\n", resp_header.filename);
    }
}


void handle_ss_dead_report(SSReadPayload* dead_ss_payload) {
    write_log("ERROR", "Reporting dead SS at %s:%d to Name Server.", 
              dead_ss_payload->ip_addr, dead_ss_payload->port);

    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_SS_DEAD_REPORT;
    header.source_component = COMPONENT_CLIENT;
    header.payload_length = sizeof(SSReadPayload);

    // Send the report to the NS
    if (send_header(g_ns_socket, &header) == -1) {
        write_log("ERROR", "Connection to NS lost while sending dead SS report.");
        return;
    }
    if (send_all(g_ns_socket, dead_ss_payload, sizeof(SSReadPayload)) == -1) {
        write_log("ERROR", "Connection to NS lost while sending dead SS report.");
        return;
    }

    // Wait for a simple ACK from the NS
    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        write_log("ERROR", "Connection to NS lost after sending dead SS report.");
        return;
    }

    if (resp_header.msg_type == MSG_ACK) {
        printf("Notified Name Server of the disconnected storage server.\n");
    }
}
/**
 * @brief Handler for LIST command
 */
void handle_list_command() {
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_LIST;
    header.source_component = COMPONENT_CLIENT;

    if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }

    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }

    if (resp_header.msg_type == MSG_LIST_RESPONSE) {
        if (resp_header.payload_length == 0) {
            printf("--- Active Users ---\n(No users online)\n--------------------\n");
            return;
        }
        
        char* list_buffer = malloc(resp_header.payload_length + 1);
        if (recv_all(g_ns_socket, list_buffer, resp_header.payload_length) == -1) {
            write_log("ERROR", "Failed to receive LIST payload.");
            free(list_buffer);
            return;
        }
        list_buffer[resp_header.payload_length] = '\0';
        
        printf("\n--- Active Users ---\n%s--------------------\n", list_buffer);
        free(list_buffer);
    } else {
        printf("Error: %s\n", resp_header.filename);
    }
}

/**
 * @brief Handler for VIEW command
 */
void handle_view_command(int flags) {
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_VIEW;
    header.source_component = COMPONENT_CLIENT;
    header.payload_length = sizeof(ViewPayload);

    ViewPayload payload;
    payload.flags = flags;

    if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }
    if (send_all(g_ns_socket, &payload, sizeof(payload)) == -1) { write_log("ERROR", "Connection to NS lost."); return; }

    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }

    if (resp_header.msg_type == MSG_VIEW_RESPONSE) {
        if (resp_header.payload_length == 0) {
            printf("(No files found)\n");
            return;
        }
        
        char* list_buffer = malloc(resp_header.payload_length + 1);
        if (recv_all(g_ns_socket, list_buffer, resp_header.payload_length) == -1) {
            free(list_buffer);
            return;
        }
        list_buffer[resp_header.payload_length] = '\0';
        
        // If -l flag is set, we have a formatted table. Otherwise, simple list.
        if (flags & VIEW_FLAG_LONG) {
            // Print formatted table header
            printf("-----------------------------------------------------------------\n");
            printf("| T |  Filename  | Words | Chars | Last Access Time | Owner |\n");
            printf("|---|------------|-------|-------|------------------|-------|\n");
            printf("%s", list_buffer);
            printf("---------------------------------------------------------\n");
        } else {
            printf("%s", list_buffer);
        }
        free(list_buffer);
    } else {
        printf("Error: %s\n", resp_header.filename);
    }
}

/**
 * @brief Handler for INFO command
 */
void handle_info_command(const char* filename) {
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_INFO;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);
    
    if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }
    
    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }

    if (resp_header.msg_type == MSG_INFO_RESPONSE) {
        FileInfoPayload payload;
        if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
            write_log("ERROR", "Failed to receive INFO payload.");
            return;
        }
        
        // Format the output as per specification
        printf("--> File: %s\n", payload.filename);
        printf("--> Owner: %s\n", payload.owner_username);
        
        // Format created time
        char created_str[30];
        strftime(created_str, sizeof(created_str), "%Y-%m-%d %H:%M", localtime(&payload.created));
        printf("--> Created: %s\n", created_str);
        
        // Format last modified time
        char modified_str[30];
        strftime(modified_str, sizeof(modified_str), "%Y-%m-%d %H:%M", localtime(&payload.last_modified));
        printf("--> Last Modified: %s\n", modified_str);
        
        printf("--> Size: %ld bytes\n", payload.char_count);
        
        // Format access permissions
        printf("--> Access: %s (RW)", payload.owner_username);
        for(int i = 0; i < payload.acl_count; i++) {
            printf(", %s (", payload.acl[i].username);
            if (payload.acl[i].permission == PERM_WRITE) {
                printf("RW");
            } else if (payload.acl[i].permission == PERM_READ) {
                printf("R");
            }
            printf(")");
        }
        printf("\n");
        
        // Format last accessed
        char accessed_str[30];
        strftime(accessed_str, sizeof(accessed_str), "%Y-%m-%d %H:%M", localtime(&payload.last_accessed));
        printf("--> Last Accessed: %s by %s\n", accessed_str, 
               (payload.last_accessed_by[0] != '\0') ? payload.last_accessed_by : "N/A");

    } else {
        printf("Error: %s\n", resp_header.filename);
    }
}

/**
 * @brief Handler for ADDACCESS and REMACCESS
 */
void handle_access_command(int msg_type, const char* filename, const char* target_user, int permission) {
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = msg_type;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);
    
    if (msg_type == MSG_ADD_ACCESS) {
        AccessControlPayload payload;
        strncpy(payload.target_username, target_user, 64 - 1);
        payload.permission = (PermissionType)permission;
        header.payload_length = sizeof(AccessControlPayload);
        
        if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }
        if (send_all(g_ns_socket, &payload, sizeof(payload)) == -1) { write_log("ERROR", "Connection to NS lost."); return; }

    } else { // MSG_REM_ACCESS
        header.payload_length = strlen(target_user) + 1;
        if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }
        if (send_all(g_ns_socket, target_user, header.payload_length) == -1) { write_log("ERROR", "Connection to NS lost."); return; }
    }

    MessageHeader response;
    if (recv_header(g_ns_socket, &response) == -1) { write_log("ERROR", "Connection to NS lost."); return; }
    
    if (response.msg_type == MSG_ACK) {
        printf("Access updated successfully.\n");
    } else {
        printf("Error: %s\n", response.filename);
    }
}

/**
 * @brief Handler for EXEC command
 */
void handle_exec_command(const char* filename) {
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_EXEC;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);
    
    if (send_header(g_ns_socket, &header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }
    
    // NS will stream output and then close the connection.
    // We just read and print until the socket closes.
    printf("--- Server Exec Output ---\n");
    char buffer[1024];
    ssize_t bytes_read;
    while ((bytes_read = recv(g_ns_socket, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytes_read] = '\0';
        printf("%s", buffer); // Print raw output
    }
    printf("\n--- Exec Finished (Connection closed by server) ---\n");

    // We must reconnect and log in again
    printf("Reconnecting to Name Server...\n");
    close(g_ns_socket); // g_ns_socket is now invalid
    
    if (connect_and_login(g_ns_ip_global, g_ns_port_global, g_username) != 0) {
        printf("Failed to reconnect. Exiting.\n");
        exit(1);
    }
    printf("Reconnected as %s.\n", g_username);
}

/**
 * @brief Handles the bilingual flow for READ/WRITE/STREAM
 */
void handle_redirect_command(int msg_type, const char* filename, int sentence_num) {
    // 1. Ask NS for redirect
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = msg_type;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);

    if (send_header(g_ns_socket, &header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }

    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) { write_log("ERROR", "Connection to NS lost."); return; }
    
    // 2. Check response
    if (resp_header.msg_type == MSG_ERROR) {
        printf("Error: %s\n", resp_header.filename);
        return;
    }
    if (resp_header.msg_type != MSG_READ_REDIRECT) {
        printf("Error: Name Server sent unexpected response.\n");
        return;
    }
    
    // 3. Get the redirect payload
    SSReadPayload payload;
    if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
        write_log("ERROR", "Failed to receive redirect payload.");
        return;
    }
    
    write_log("INFO", "Redirected to SS at %s:%d", payload.ip_addr, payload.port);
    
    // 4. Connect DIRECTLY to the Storage Server
    int ss_sock = create_socket();
    if (connect_socket_no_exit(ss_sock, payload.ip_addr, payload.port) == -1) {
        printf("Error: Could not connect to Storage Server at %s:%d.\n", payload.ip_addr, payload.port);
        close(ss_sock);
        handle_ss_dead_report(&payload);
        return;
    }
    
    // 5. --- SPEAK PERSON B's TEXT PROTOCOL ---
    char buffer[BUF_SZ];
    
    // 5a. Send USER handshake
    snprintf(buffer, BUF_SZ, "USER %s\n", g_username);
    send(ss_sock, buffer, strlen(buffer), 0);
    recv(ss_sock, buffer, BUF_SZ - 1, 0); // Get "OK_200 USER_ACCEPTED"
    
    // --- READ/STREAM Logic ---
    if (msg_type == MSG_READ || msg_type == MSG_STREAM) {
        
        char* cmd_str = (msg_type == MSG_READ) ? "READ" : "STREAM";
        snprintf(buffer, BUF_SZ, "%s %s\n", cmd_str, filename);
        send(ss_sock, buffer, strlen(buffer), 0);
        
        printf("--- File Content ---\n");
        if(msg_type == MSG_STREAM) {
            // Use the word-by-word streaming logic from Person B's client
            // We assume the SS sends one word per packet, followed by "STREAM_COMPLETE"
            printf("Streaming content: ");
            fflush(stdout);
            
            while (1) {
                ssize_t n = recv(ss_sock, buffer, BUF_SZ - 1, 0);
                if (n <= 0) break;
                buffer[n] = '\0';
                
                // Check for control messages from SS
                if (strstr(buffer, "STREAM_COMPLETE")) break;
                if (strstr(buffer, "OK_200 EMPTY_FILE")) break;
                if (strstr(buffer, "ERR_")) { printf("%s", buffer); break; }
                
                // Process word-by-word
                printf("%s ", buffer);
                fflush(stdout);
                
                // This is the 0.1s delay
                struct timespec ts;
                ts.tv_sec = 0;
                ts.tv_nsec = 100000000L; // 100 million nanoseconds
                nanosleep(&ts, NULL);
            }
        } else {
            // --- THIS IS THE CORRECTED READ LOGIC ---
            // It handles partial reads and looks for the
            // *exact* terminators your SS is sending.
            char read_buffer[BUF_SZ];
            int first_packet = 1;
            while (1) {
                ssize_t n = recv(ss_sock, read_buffer, BUF_SZ - 1, 0);
                if (n <= 0) break; // Connection closed
                read_buffer[n] = '\0';

                char* content_to_print = read_buffer;

                // Check for the "EMPTY" message (from your log)
                if (strstr(content_to_print, "OK_200 EMPTY_FILE")) {
                    break; // Just break, print nothing
                }

                // Check for the "START" message (from your log)
                if (first_packet) {
                    char* start_ptr = strstr(content_to_print, "OK_200 FILE_CONTENT\n");
                    if (start_ptr) {
                        // Move the pointer past this header
                        content_to_print = start_ptr + strlen("OK_200 FILE_CONTENT\n");
                    }
                    first_packet = 0;
                }

                // Check for the "END" message (from your log)
                char* end_ptr = strstr(content_to_print, "END_OF_FILE");
                if (end_ptr) {
                    *end_ptr = '\0'; // NUL-terminate before the "END"
                    printf("%s", content_to_print); // Print the last chunk
                    break; // Exit the loop
                }

                // If no terminators, just print the buffer
                printf("%s", content_to_print);
            }
        }
        printf("\n--- End of File ---\n");
    }
    
    // --- WRITE Logic ---
    else if (msg_type == MSG_WRITE) {
        snprintf(buffer, BUF_SZ, "WRITE %s %d\n", filename, sentence_num);
        send(ss_sock, buffer, strlen(buffer), 0);

        ssize_t n = recv(ss_sock, buffer, BUF_SZ - 1, 0);
        if (n <= 0) {
            printf("Error: Storage Server disconnected.\n");
            close(ss_sock);
            return;
        }
        buffer[n] = '\0';
        printf("%s", buffer); // Print response (e.g., "OK_200 WRITE MODE" or "ERR_409")

        if (strncmp(buffer, "OK_200", 6) != 0) {
            close(ss_sock); // Not OK, something went wrong
            return;
        }

        // Enter Person B's "WRITE mode" loop
        printf("Entering WRITE mode. Send '<word_index> <content>' or 'ETIRW' to finish.\n");
        while(1) {
            printf("write > ");
            if (fgets(buffer, sizeof(buffer), stdin) == NULL) {
                break;
            }
            
            send(ss_sock, buffer, strlen(buffer), 0);
            
            n = recv(ss_sock, buffer, BUF_SZ - 1, 0);
            if (n <= 0) {
                printf("Storage Server disconnected.\n");
                break;
            }
            buffer[n] = '\0';
            printf("%s", buffer); // Print "OK_200 CONTENT INSERTED" or "OK_200 WRITE COMPLETED"
            
            if (strncmp(buffer, "OK_200 WRITE COMPLETED", 22) == 0) {
                break; // We're done
            }
        }
    }
    
    // 5d. Send EXIT
    send(ss_sock, "EXIT\n", 5, 0);
    close(ss_sock);
}

/**
 * @brief Handler for CHECKPOINT command - creates a checkpoint via direct SS connection
 */
void handle_checkpoint_command(const char* filename, const char* checkpoint_tag) {
    // First get redirect to the storage server
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_READ; // Use READ to get SS redirect
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);

    if (send_header(g_ns_socket, &header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }

    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }
    
    if (resp_header.msg_type == MSG_ERROR) {
        printf("Error: %s\n", resp_header.filename);
        return;
    }
    if (resp_header.msg_type != MSG_READ_REDIRECT) {
        printf("Error: Name Server sent unexpected response.\n");
        return;
    }
    
    // Get the redirect payload
    SSReadPayload payload;
    if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
        write_log("ERROR", "Failed to receive redirect payload.");
        return;
    }
    
    write_log("INFO", "Redirected to SS at %s:%d for CHECKPOINT", payload.ip_addr, payload.port);
    
    // Connect to Storage Server
    int ss_sock = create_socket();
    if (connect_socket_no_exit(ss_sock, payload.ip_addr, payload.port) == -1) {
        printf("Error: Could not connect to Storage Server at %s:%d.\n", payload.ip_addr, payload.port);
        close(ss_sock);
        handle_ss_dead_report(&payload);
        return;
    }
    
    // Send USER handshake
    char buffer[BUF_SZ];
    snprintf(buffer, BUF_SZ, "USER %s\n", g_username);
    send(ss_sock, buffer, strlen(buffer), 0);
    recv(ss_sock, buffer, BUF_SZ - 1, 0); // Get "OK_200 USER_ACCEPTED"
    
    // Send CHECKPOINT command
    snprintf(buffer, BUF_SZ, "CHECKPOINT %s %s\n", filename, checkpoint_tag);
    send(ss_sock, buffer, strlen(buffer), 0);
    
    // Receive response
    ssize_t n = recv(ss_sock, buffer, BUF_SZ - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);
        
        if (strncmp(buffer, "OK_200", 6) == 0) {
            printf("Checkpoint '%s' created successfully for file '%s'.\n", checkpoint_tag, filename);
        }
    } else {
        printf("Error: Storage Server disconnected.\n");
    }
    
    // Send EXIT and close
    send(ss_sock, "EXIT\n", 5, 0);
    close(ss_sock);
}

/**
 * @brief Handler for VIEWCHECKPOINT command - views a checkpoint via direct SS connection
 */
void handle_viewcheckpoint_command(const char* filename, const char* checkpoint_tag) {
    // Get redirect to storage server (same pattern as checkpoint)
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_READ;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);

    if (send_header(g_ns_socket, &header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }

    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }
    
    if (resp_header.msg_type == MSG_ERROR) {
        printf("Error: %s\n", resp_header.filename);
        return;
    }
    if (resp_header.msg_type != MSG_READ_REDIRECT) {
        printf("Error: Name Server sent unexpected response.\n");
        return;
    }
    
    SSReadPayload payload;
    if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
        write_log("ERROR", "Failed to receive redirect payload.");
        return;
    }
    
    // Connect to Storage Server
    int ss_sock = create_socket();
    if (connect_socket_no_exit(ss_sock, payload.ip_addr, payload.port) == -1) {
        printf("Error: Could not connect to Storage Server at %s:%d.\n", payload.ip_addr, payload.port);
        close(ss_sock);
        handle_ss_dead_report(&payload);
        return;
    }
    
    // Send USER handshake
    char buffer[BUF_SZ];
    snprintf(buffer, BUF_SZ, "USER %s\n", g_username);
    send(ss_sock, buffer, strlen(buffer), 0);
    recv(ss_sock, buffer, BUF_SZ - 1, 0);
    
    // Send VIEWCHECKPOINT command
    snprintf(buffer, BUF_SZ, "VIEWCHECKPOINT %s %s\n", filename, checkpoint_tag);
    send(ss_sock, buffer, strlen(buffer), 0);
    
    // Receive and display response
    printf("--- Checkpoint Content: %s ---\n", checkpoint_tag);
    
    char read_buffer[BUF_SZ];
    int first_packet = 1;
    while (1) {
        ssize_t n = recv(ss_sock, read_buffer, BUF_SZ - 1, 0);
        if (n <= 0) break;
        read_buffer[n] = '\0';

        char* content_to_print = read_buffer;

        // Handle empty checkpoint
        if (strstr(content_to_print, "OK_200 EMPTY_CHECKPOINT")) {
            printf("(Checkpoint is empty)\n");
            break;
        }

        // Handle error responses
        if (strstr(content_to_print, "ERR_404")) {
            printf("Error: Checkpoint '%s' not found for file '%s'\n", checkpoint_tag, filename);
            break;
        }

        // Handle start of content
        if (first_packet) {
            char* start_ptr = strstr(content_to_print, "OK_200 CHECKPOINT_CONTENT\n");
            if (start_ptr) {
                content_to_print = start_ptr + strlen("OK_200 CHECKPOINT_CONTENT\n");
            }
            first_packet = 0;
        }

        // Handle end of content
        char* end_ptr = strstr(content_to_print, "END_OF_CHECKPOINT");
        if (end_ptr) {
            *end_ptr = '\0';
            printf("%s", content_to_print);
            break;
        }

        printf("%s", content_to_print);
    }
    
    printf("\n--- End of Checkpoint ---\n");
    
    // Send EXIT and close
    send(ss_sock, "EXIT\n", 5, 0);
    close(ss_sock);
}

/**
 * @brief Handler for REVERT command - reverts file to a checkpoint via direct SS connection
 */
void handle_revert_command(const char* filename, const char* checkpoint_tag) {
    // Get redirect to storage server
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_READ;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);

    if (send_header(g_ns_socket, &header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }

    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }
    
    if (resp_header.msg_type == MSG_ERROR) {
        printf("Error: %s\n", resp_header.filename);
        return;
    }
    if (resp_header.msg_type != MSG_READ_REDIRECT) {
        printf("Error: Name Server sent unexpected response.\n");
        return;
    }
    
    SSReadPayload payload;
    if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
        write_log("ERROR", "Failed to receive redirect payload.");
        return;
    }
    
    // Connect to Storage Server
    int ss_sock = create_socket();
    if (connect_socket_no_exit(ss_sock, payload.ip_addr, payload.port) == -1) {
        printf("Error: Could not connect to Storage Server at %s:%d.\n", payload.ip_addr, payload.port);
        close(ss_sock);
        handle_ss_dead_report(&payload);
        return;
    }
    
    // Send USER handshake
    char buffer[BUF_SZ];
    snprintf(buffer, BUF_SZ, "USER %s\n", g_username);
    send(ss_sock, buffer, strlen(buffer), 0);
    recv(ss_sock, buffer, BUF_SZ - 1, 0);
    
    // Send REVERT command
    snprintf(buffer, BUF_SZ, "REVERT %s %s\n", filename, checkpoint_tag);
    send(ss_sock, buffer, strlen(buffer), 0);
    
    // Receive response
    ssize_t n = recv(ss_sock, buffer, BUF_SZ - 1, 0);
    if (n > 0) {
        buffer[n] = '\0';
        printf("%s", buffer);
        
        if (strncmp(buffer, "OK_200", 6) == 0) {
            printf("File '%s' successfully reverted to checkpoint '%s'.\n", filename, checkpoint_tag);
        } else if (strstr(buffer, "ERR_404")) {
            printf("Error: Checkpoint '%s' not found for file '%s'\n", checkpoint_tag, filename);
        } else if (strstr(buffer, "ERR_409")) {
            printf("Error: Cannot revert - file is currently being edited by another user.\n");
        }
    } else {
        printf("Error: Storage Server disconnected.\n");
    }
    
    // Send EXIT and close
    send(ss_sock, "EXIT\n", 5, 0);
    close(ss_sock);
}

/**
 * @brief Handler for LISTCHECKPOINTS command - lists all checkpoints for a file
 */
void handle_listcheckpoints_command(const char* filename) {
    // Get redirect to storage server
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_READ;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);

    if (send_header(g_ns_socket, &header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }

    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        write_log("ERROR", "Connection to NS lost.");
        return;
    }
    
    if (resp_header.msg_type == MSG_ERROR) {
        printf("Error: %s\n", resp_header.filename);
        return;
    }
    if (resp_header.msg_type != MSG_READ_REDIRECT) {
        printf("Error: Name Server sent unexpected response.\n");
        return;
    }
    
    SSReadPayload payload;
    if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
        write_log("ERROR", "Failed to receive redirect payload.");
        return;
    }
    
    // Connect to Storage Server
    int ss_sock = create_socket();
    if (connect_socket_no_exit(ss_sock, payload.ip_addr, payload.port) == -1) {
        printf("Error: Could not connect to Storage Server at %s:%d.\n", payload.ip_addr, payload.port);
        close(ss_sock);
        handle_ss_dead_report(&payload);
        return;
    }
    
    // Send USER handshake
    char buffer[BUF_SZ];
    snprintf(buffer, BUF_SZ, "USER %s\n", g_username);
    send(ss_sock, buffer, strlen(buffer), 0);
    recv(ss_sock, buffer, BUF_SZ - 1, 0);
    
    // Send LISTCHECKPOINTS command
    snprintf(buffer, BUF_SZ, "LISTCHECKPOINTS %s\n", filename);
    send(ss_sock, buffer, strlen(buffer), 0);
    
    // Receive and display response
    printf("--- Checkpoints for %s ---\n", filename);
    
    char read_buffer[BUF_SZ];
    int first_packet = 1;
    while (1) {
        ssize_t n = recv(ss_sock, read_buffer, BUF_SZ - 1, 0);
        if (n <= 0) break;
        read_buffer[n] = '\0';

        char* content_to_print = read_buffer;

        // Handle start of list
        if (first_packet) {
            char* start_ptr = strstr(content_to_print, "OK_200 CHECKPOINT_LIST\n");
            if (start_ptr) {
                content_to_print = start_ptr + strlen("OK_200 CHECKPOINT_LIST\n");
            }
            first_packet = 0;
        }

        // Handle end of list
        char* end_ptr = strstr(content_to_print, "END_OF_LIST");
        if (end_ptr) {
            *end_ptr = '\0';
            printf("%s", content_to_print);
            break;
        }

        printf("%s", content_to_print);
    }
    
    printf("--- End of List ---\n");
    
    // Send EXIT and close
    send(ss_sock, "EXIT\n", 5, 0);
    close(ss_sock);
}

/**
 * @brief Handle REQUESTACCESS command - Fixed to use MSG_LOCATE_FILE
 */
void handle_requestaccess_command(const char* filename, const char* permission) {
    write_log("INFO", "Requesting %s access to file: %s", permission, filename);
    
    // Use MSG_LOCATE_FILE to find the storage server without access restrictions
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_LOCATE_FILE;  // Use new message type
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);
    
    if (send_header(g_ns_socket, &header) == -1) {
        printf("Error: Connection to Name Server lost.\n");
        return;
    }
    
    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        printf("Error: Connection to Name Server lost.\n");
        return;
    }
    
    if (resp_header.msg_type == MSG_LOCATE_RESPONSE) {
        SSReadPayload payload;  // Reuse existing payload structure
        if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
            printf("Error: Failed to receive storage server info.\n");
            return;
        }
        
        // Connect to storage server using location info
        int ss_socket = create_socket();
        if (connect_socket_no_exit(ss_socket, payload.ip_addr, payload.port) == -1) {
            printf("Error: Could not connect to storage server at %s:%d.\n", payload.ip_addr, payload.port);
            close(ss_socket);
            return;
        }
        
        // Send user identification
        char user_msg[256];
        snprintf(user_msg, sizeof(user_msg), "USER %s\n", g_username);
        send(ss_socket, user_msg, strlen(user_msg), 0);
        
        // Wait for user acceptance
        char ack_buf[256];
        ssize_t ack_received = recv(ss_socket, ack_buf, sizeof(ack_buf) - 1, 0);
        if (ack_received <= 0) {
            printf("Error: Storage server connection failed.\n");
            close(ss_socket);
            return;
        }
        ack_buf[ack_received] = '\0';
        
        if (strncmp(ack_buf, "OK_200", 6) != 0) {
            printf("Error: Storage server rejected connection: %s", ack_buf);
            close(ss_socket);
            return;
        }
        
        // Send requestaccess command
        char request_cmd[512];
        snprintf(request_cmd, sizeof(request_cmd), "REQUESTACCESS %s %s\n", filename, permission);
        send(ss_socket, request_cmd, strlen(request_cmd), 0);
        
        // Receive response
        char response[1024];
        ssize_t bytes_received = recv(ss_socket, response, sizeof(response) - 1, 0);
        if (bytes_received > 0) {
            response[bytes_received] = '\0';
            
            if (strncmp(response, "OK_200", 6) == 0) {
                printf("Access request submitted successfully.\n");
                write_log("INFO", "Access request submitted: %s for %s access to %s", g_username, permission, filename);
            } else if (strncmp(response, "ERR_400", 7) == 0) {
                char* error_msg = strchr(response, ' ');
                if (error_msg) error_msg++;
                printf("Error: %s", error_msg ? error_msg : "Invalid request\n");
            } else if (strncmp(response, "ERR_404", 7) == 0) {
                printf("Error: File not found.\n");
            } else if (strncmp(response, "ERR_409", 7) == 0) {
                char* error_msg = strchr(response, ' ');
                if (error_msg) error_msg++;
                printf("Error: %s", error_msg ? error_msg : "Request already exists or you already have access\n");
            } else {
                printf("Error: %s", response);
            }
        } else {
            printf("Error: No response from storage server.\n");
        }
        
        // Send EXIT and close
        send(ss_socket, "EXIT\n", 5, 0);
        close(ss_socket);
    } else if (resp_header.msg_type == MSG_ERROR) {
        printf("Error: %s\n", resp_header.filename);
    } else {
        printf("Error: File not found in any storage server.\n");
        write_log("ERROR", "REQUESTACCESS failed: File %s not found in any storage server", filename);
    }
}

/**
 * @brief Handle VIEWREQUESTS command - Fixed to use MSG_LOCATE_FILE
 */
void handle_viewrequests_command(const char* filename) {
    write_log("INFO", "Viewing access requests for file: %s", filename ? filename : "all files");
    
    if (!filename) {
        printf("Error: Please specify a filename to determine storage server location.\n");
        printf("Usage: viewrequests <existing_filename>\n");
        return;
    }
    
    // Use MSG_LOCATE_FILE to find the storage server
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_LOCATE_FILE;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);
    
    if (send_header(g_ns_socket, &header) == -1) {
        printf("Error: Connection to Name Server lost.\n");
        return;
    }
    
    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        printf("Error: Connection to Name Server lost.\n");
        return;
    }
    
    if (resp_header.msg_type == MSG_LOCATE_RESPONSE) {
        SSReadPayload payload;
        if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
            printf("Error: Failed to receive storage server info.\n");
            return;
        }
        
        // Connect to storage server
        int ss_socket = create_socket();
        if (connect_socket_no_exit(ss_socket, payload.ip_addr, payload.port) == -1) {
            printf("Error: Could not connect to storage server at %s:%d.\n", payload.ip_addr, payload.port);
            close(ss_socket);
            return;
        }
        
        // Send user identification
        char user_msg[256];
        snprintf(user_msg, sizeof(user_msg), "USER %s\n", g_username);
        send(ss_socket, user_msg, strlen(user_msg), 0);
        
        // Wait for user acceptance
        char ack_buf[256];
        ssize_t ack_received = recv(ss_socket, ack_buf, sizeof(ack_buf) - 1, 0);
        if (ack_received <= 0) {
            printf("Error: Storage server connection failed.\n");
            close(ss_socket);
            return;
        }
        ack_buf[ack_received] = '\0';
        
        // Send viewrequests command
        char view_cmd[512];
        snprintf(view_cmd, sizeof(view_cmd), "VIEWREQUESTS %s\n", filename);
        send(ss_socket, view_cmd, strlen(view_cmd), 0);
        
        // Receive and process response
        char response[1024];
        ssize_t bytes_received = recv(ss_socket, response, sizeof(response) - 1, 0);
        if (bytes_received > 0) {
            response[bytes_received] = '\0';
            
            if (strncmp(response, "OK_200", 6) == 0) {
                printf("\n--- Access Requests ---\n");
                
                // Continue reading the content until we see END_OF_REQUESTS
                char content_buffer[8192] = "";
                ssize_t total_received = 0;
                
                while (total_received < sizeof(content_buffer) - 1) {
                    char chunk[1024];
                    ssize_t chunk_received = recv(ss_socket, chunk, sizeof(chunk) - 1, 0);
                    if (chunk_received <= 0) break;
                    
                    chunk[chunk_received] = '\0';
                    
                    // Append to content buffer
                    if (total_received + chunk_received < sizeof(content_buffer) - 1) {
                        strcat(content_buffer, chunk);
                        total_received += chunk_received;
                    }
                    
                    // Check for end marker
                    if (strstr(content_buffer, "END_OF_REQUESTS")) {
                        break;
                    }
                }
                
                // Remove end marker from output
                char* end_marker = strstr(content_buffer, "\nEND_OF_REQUESTS");
                if (end_marker) {
                    *end_marker = '\0';
                }
                
                printf("%s\n", content_buffer);
                printf("--- End of Requests ---\n");
            } else if (strncmp(response, "ERR_403", 7) == 0) {
                printf("Error: You can only view requests for files you own.\n");
            } else {
                printf("Error: %s", response);
            }
        } else {
            printf("Error: No response from storage server.\n");
        }
        
        // Send EXIT and close
        send(ss_socket, "EXIT\n", 5, 0);
        close(ss_socket);
    } else {
        printf("Error: File not found in any storage server.\n");
    }
}

/**
 * @brief Handle APPROVEREQUEST command - Fixed to use MSG_LOCATE_FILE
 */
void handle_approverequest_command(const char* filename, const char* username, const char* permission) {
    write_log("INFO", "Approving %s access for user %s on file: %s", permission, username, filename);
    
    // Use MSG_LOCATE_FILE to find the storage server
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_LOCATE_FILE;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);
    
    if (send_header(g_ns_socket, &header) == -1) {
        printf("Error: Connection to Name Server lost.\n");
        return;
    }
    
    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        printf("Error: Connection to Name Server lost.\n");
        return;
    }
    
    if (resp_header.msg_type == MSG_LOCATE_RESPONSE) {
        SSReadPayload payload;
        if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
            printf("Error: Failed to receive storage server info.\n");
            return;
        }
        
        // Connect to storage server
        int ss_socket = create_socket();
        if (connect_socket_no_exit(ss_socket, payload.ip_addr, payload.port) == -1) {
            printf("Error: Could not connect to storage server at %s:%d.\n", payload.ip_addr, payload.port);
            close(ss_socket);
            return;
        }
        
        // Send user identification
        char user_msg[256];
        snprintf(user_msg, sizeof(user_msg), "USER %s\n", g_username);
        send(ss_socket, user_msg, strlen(user_msg), 0);
        
        // Wait for user acceptance
        char ack_buf[256];
        ssize_t ack_received = recv(ss_socket, ack_buf, sizeof(ack_buf) - 1, 0);
        if (ack_received <= 0) {
            printf("Error: Storage server connection failed.\n");
            close(ss_socket);
            return;
        }
        ack_buf[ack_received] = '\0';
        
        // Send approve command
        char approve_cmd[512];
        snprintf(approve_cmd, sizeof(approve_cmd), "APPROVEREQUEST %s %s %s\n", filename, username, permission);
        send(ss_socket, approve_cmd, strlen(approve_cmd), 0);
        
        // Receive response
        char response[1024];
        ssize_t bytes_received = recv(ss_socket, response, sizeof(response) - 1, 0);
        if (bytes_received > 0) {
            response[bytes_received] = '\0';
            
            if (strncmp(response, "OK_200", 6) == 0) {
                printf("Access request approved successfully.\n");
                write_log("INFO", "Access request approved: %s granted %s access to %s", username, permission, filename);
            } else if (strncmp(response, "ERR_403", 7) == 0) {
                printf("Error: You don't own this file.\n");
            } else if (strncmp(response, "ERR_404", 7) == 0) {
                printf("Error: Access request not found.\n");
            } else {
                char* error_msg = response + 8; // Skip error code
                printf("Error: %s", error_msg);
            }
        } else {
            printf("Error: No response from storage server.\n");
        }
        
        // Send EXIT and close
        send(ss_socket, "EXIT\n", 5, 0);
        close(ss_socket);
    } else {
        printf("Error: File not found in any storage server.\n");
    }
}

/**
 * @brief Handle DENYREQUEST command - Fixed to use MSG_LOCATE_FILE
 */
void handle_denyrequest_command(const char* filename, const char* username) {
    write_log("INFO", "Denying access request for user %s on file: %s", username, filename);
    
    // Use MSG_LOCATE_FILE to find the storage server
    MessageHeader header;
    memset(&header, 0, sizeof(header));
    header.msg_type = MSG_LOCATE_FILE;
    header.source_component = COMPONENT_CLIENT;
    strncpy(header.filename, filename, MAX_FILENAME - 1);
    
    if (send_header(g_ns_socket, &header) == -1) {
        printf("Error: Connection to Name Server lost.\n");
        return;
    }
    
    MessageHeader resp_header;
    if (recv_header(g_ns_socket, &resp_header) == -1) {
        printf("Error: Connection to Name Server lost.\n");
        return;
    }
    
    if (resp_header.msg_type == MSG_LOCATE_RESPONSE) {
        SSReadPayload payload;
        if (recv_all(g_ns_socket, &payload, sizeof(payload)) == -1) {
            printf("Error: Failed to receive storage server info.\n");
            return;
        }
        
        // Connect to storage server
        int ss_socket = create_socket();
        if (connect_socket_no_exit(ss_socket, payload.ip_addr, payload.port) == -1) {
            printf("Error: Could not connect to storage server at %s:%d.\n", payload.ip_addr, payload.port);
            close(ss_socket);
            return;
        }
        
        // Send user identification
        char user_msg[256];
        snprintf(user_msg, sizeof(user_msg), "USER %s\n", g_username);
        send(ss_socket, user_msg, strlen(user_msg), 0);
        
        // Wait for user acceptance
        char ack_buf[256];
        ssize_t ack_received = recv(ss_socket, ack_buf, sizeof(ack_buf) - 1, 0);
        if (ack_received <= 0) {
            printf("Error: Storage server connection failed.\n");
            close(ss_socket);
            return;
        }
        ack_buf[ack_received] = '\0';
        
        // Send deny command
        char deny_cmd[512];
        snprintf(deny_cmd, sizeof(deny_cmd), "DENYREQUEST %s %s\n", filename, username);
        send(ss_socket, deny_cmd, strlen(deny_cmd), 0);
        
        // Receive response
        char response[1024];
        ssize_t bytes_received = recv(ss_socket, response, sizeof(response) - 1, 0);
        if (bytes_received > 0) {
            response[bytes_received] = '\0';
            
            if (strncmp(response, "OK_200", 6) == 0) {
                printf("Access request denied successfully.\n");
                write_log("INFO", "Access request denied: %s denied access to %s", username, filename);
            } else if (strncmp(response, "ERR_403", 7) == 0) {
                printf("Error: You don't own this file.\n");
            } else if (strncmp(response, "ERR_404", 7) == 0) {
                printf("Error: Access request not found.\n");
            } else {
                char* error_msg = response + 8; // Skip error code
                printf("Error: %s", error_msg);
            }
        } else {
            printf("Error: No response from storage server.\n");
        }
        
        // Send EXIT and close
        send(ss_socket, "EXIT\n", 5, 0);
        close(ss_socket);
    } else {
        printf("Error: File not found in any storage server.\n");
    }
}