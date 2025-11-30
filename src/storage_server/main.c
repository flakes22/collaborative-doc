/*
 * =================================================================
 * FINAL INTEGRATED Storage Server main.c
 *
 * This server is multi-threaded and does two jobs at once:
 * 1. Main Thread: Connects to the Name Server, registers,
 * and handles forwarded commands (CREATE, DELETE, etc.).
 * 2. Listener Thread: Listens on its public port for
 * direct client connections (READ, WRITE, STREAM).
 * =================================================================
 */

#define _POSIX_C_SOURCE 112L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>  // For usleep() - should already be there
#include <netinet/tcp.h>  // For TCP_NODELAY

// Headers from 'common'
#include "../../include/common.h"
#include "../../include/logger.h"
#include "../../include/protocol.h"
#include "../../include/socket_utils.h"
#include "../../include/storage_manager.h" // For SSRegistrationPayload

// Headers from 'storage_server'
#include "../../include/storage_server.h"
#include "../../include/persistence.h"

// --- Defines, Structs, and Globals ---

#define BUF_SZ 2048

// Context for a direct client connection
typedef struct {
    int client_fd;
    struct sockaddr_in client_addr;
    int server_port;
} client_ctx_t;

// Sentence-level lock tracking
typedef struct sentence_lock {
    char filename[256];
    int sentence_num;
    int client_fd;
    struct sentence_lock *next;
} sentence_lock_t;

// Client list for shutdown
typedef struct client_node {
    int fd;
    struct client_node *next;
} client_node_t;

// --- Globals ---
static int g_ns_socket = -1;
static int g_my_port = 0;
static char g_my_ip[64] = "127.0.0.1";
static char g_meta_dir[256];
static int g_running = 1;

// Globals for Person B's sentence locking
static sentence_lock_t *sentence_locks = NULL;
static pthread_mutex_t lock_mutex = PTHREAD_MUTEX_INITIALIZER;

// Globals for Person B's client list
static client_node_t *client_list = NULL;
static pthread_mutex_t client_lock = PTHREAD_MUTEX_INITIALIZER;

// --- Function Prototypes (Helpers) ---
void *client_handler_thread(void *arg);
int register_with_name_server(const char* ns_ip, int ns_port);
void handle_ns_commands();
void* client_listener_thread(void* arg);
void handle_sigint(int sig);

// Prototypes for Person B's helper functions
static void add_client_fd(int fd);
static void remove_client_fd(int fd);
static void close_all_clients();
static void free_client_list();
static int is_sentence_locked(const char* filename, int sentence_num, int client_fd);
static void add_sentence_lock(const char* filename, int sentence_num, int client_fd);
static void remove_sentence_lock(const char* filename, int sentence_num, int client_fd);
static void remove_client_locks(int client_fd);
static int get_client_write_info(int client_fd, char* filename, int* sentence_num);
static int create_file_backup(const char* filename, int server_port, const char* username);
static int perform_undo(const char* filename, int server_port, const char* username);
static void update_file_access_time(const char* meta_dir, const char* filename);
static int get_sentence_info_simple(const char* filename, int server_port, sentence_info_t sentences[], int max_sentences);

// Add these helper function prototypes after the existing prototypes (around line 80)
static int create_checkpoint(const char* filename, const char* checkpoint_tag, int server_port, const char* username);
static int view_checkpoint(const char* filename, const char* checkpoint_tag, int server_port, char* content_buffer, size_t buffer_size);
static int revert_to_checkpoint(const char* filename, const char* checkpoint_tag, int server_port, const char* username);
static int list_checkpoints(const char* filename, int server_port, char* list_buffer, size_t buffer_size);

// Add these helper function prototypes after the existing prototypes (around line 90)
static int request_file_access(const char* filename, const char* username, const char* permission, int server_port);
static int list_access_requests(const char* filename, const char* owner_username, int server_port, char* list_buffer, size_t buffer_size);
static int approve_access_request(const char* filename, const char* requester_username, const char* permission, const char* owner_username, int server_port);
static int deny_access_request(const char* filename, const char* requester_username, const char* owner_username, int server_port);
static int check_file_owner(const char* filename, const char* username, int server_port);

// =========================================================================
//  SENTENCE PARSING HELPER FUNCTION
// =========================================================================

static int get_sentence_info_simple(const char* filename, int server_port, sentence_info_t sentences[], int max_sentences) {
    char files_dir[256];
    snprintf(files_dir, sizeof(files_dir), "data/ss_%d/files", server_port);
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, filename);
    
    FILE* file = fopen(filepath, "r");
    if (!file) return 0;
    
    char content[8192] = {0};
    size_t bytes_read = fread(content, 1, sizeof(content) - 1, file);
    fclose(file);
    content[bytes_read] = '\0';
    
    // *** FIXED: Parse sentences - only LAST delimiter in each word creates sentence boundary ***
    int word_count = 0;
    int sent_count = 0;
    int current_sent_start = 0;
    
    char words[1024][512];
    char temp_content[8192];
    strcpy(temp_content, content);
    
    char *token = strtok(temp_content, " \t\n");
    while (token && word_count < 1024) {
        strcpy(words[word_count], token);
        
        // *** KEY FIX: Only check the LAST character of each word ***
        int word_len = strlen(words[word_count]);
        if (word_len > 0) {
            char last_char = words[word_count][word_len - 1];
            if (last_char == '.' || last_char == '!' || last_char == '?') {
                // Found a delimiter at end of word - end current sentence
                if (sent_count < max_sentences) {
                    sentences[sent_count].start_word_idx = current_sent_start;
                    sentences[sent_count].end_word_idx = word_count;
                    sentences[sent_count].delimiter = last_char;
                    sent_count++;
                    current_sent_start = word_count + 1;  // Next sentence starts from next word
                }
            }
        }
        
        word_count++;
        token = strtok(NULL, " \t\n");
    }
    
    // Handle incomplete sentence at the end
    if (current_sent_start < word_count && sent_count < max_sentences) {
        sentences[sent_count].start_word_idx = current_sent_start;
        sentences[sent_count].end_word_idx = word_count - 1;
        sentences[sent_count].delimiter = '\0';
        sent_count++;
    }
    
    return sent_count;
}

// =========================================================================
//  MAIN FUNCTION (Entry Point)
// =========================================================================

int main(int argc, char *argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <ss_ip> <ss_port> <ns_ip> <ns_port>\n", argv[0]);
        fprintf(stderr, "Example: %s 127.0.0.1 9001 127.0.0.1 5000\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Get Storage Server connection details
    strcpy(g_my_ip, argv[1]);
    g_my_port = atoi(argv[2]);
    
    // Get Name Server connection details
    const char* ns_ip = argv[3];
    int ns_port = atoi(argv[4]);

    if (g_my_port <= 1024 || g_my_port > 65535) {
        fprintf(stderr, "Error: Storage Server port must be between 1025 and 65535.\n");
        exit(EXIT_FAILURE);
    }

    if (ns_port <= 1024 || ns_port > 65535) {
        fprintf(stderr, "Error: Name Server port must be between 1025 and 65535.\n");
        exit(EXIT_FAILURE);
    }

    signal(SIGPIPE, SIG_IGN); // Ignore broken pipe signals
    signal(SIGINT, handle_sigint);

    // 1. Init SS (creates folders, loads metadata)
    init_storage_server(g_my_port);
    init_logger(g_my_ip, g_my_port);
    snprintf(g_meta_dir, sizeof(g_meta_dir), "data/ss_%d/metadata", g_my_port);
    load_metadata(g_meta_dir);
    write_log("INFO", "SS started on %s:%d. Loaded %d files.", g_my_ip, g_my_port, file_count);

    // 2. Start the Client Listener Thread (Job 1)
    pthread_t listener_tid;
    int* port_arg = malloc(sizeof(int));
    *port_arg = g_my_port;
    if (pthread_create(&listener_tid, NULL, client_listener_thread, port_arg) != 0) {
        perror("Failed to create listener thread");
        exit(EXIT_FAILURE);
    }
    pthread_detach(listener_tid);

    // 3. Register with Name Server (Job 2)
    if (register_with_name_server(ns_ip, ns_port) != 0) {
        g_running = 0; // Signal listener thread to stop
        close_logger();
        exit(EXIT_FAILURE);
    }

    // 4. Main thread becomes the NS command handler
    write_log("INFO", "Entering main command loop, listening for NS commands.");
    handle_ns_commands(); // This loop blocks until NS disconnects or Ctrl+C

    // 5. Cleanup
    close_all_clients(); // Close all direct client sockets
    close_logger();
    if (g_ns_socket != -1) {
        close(g_ns_socket);
    }
    
    free_client_list();
    
    printf("[SS %d] Shutdown complete.\n", g_my_port);
    return 0;
}

// =========================================================================
//  JOB 1: LISTENER THREAD (Handles direct Client connections)
// =========================================================================

void* client_listener_thread(void* arg) {
    int port = *(int*)arg;
    free(arg);

    int listen_fd = create_socket();
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    bind_socket(listen_fd, port);
    listen_socket(listen_fd);
    
    write_log("INFO", "Client Listener Thread started. Listening on port %d...", port);

    while (g_running) {
        client_ctx_t *ctx = malloc(sizeof(client_ctx_t));
        socklen_t addrlen = sizeof(ctx->client_addr);
        
        ctx->client_fd = accept(listen_fd, (struct sockaddr *)&ctx->client_addr, &addrlen);
        
        if (ctx->client_fd < 0) {
            free(ctx);
            if (!g_running) break;
            perror("accept");
            continue;
        }
        ctx->server_port = port;

        pthread_t tid;
        if (pthread_create(&tid, NULL, client_handler_thread, ctx) != 0) {
            perror("pthread_create (client)");
            close(ctx->client_fd);
            free(ctx);
        }
        pthread_detach(tid);
    }
    
    close(listen_fd);
    write_log("INFO", "Client Listener Thread shutting down.");
    return NULL;
}

// =========================================================================
//  JOB 2: MAIN THREAD (Handles Name Server connection)
// =========================================================================

void handle_ns_commands() {
    MessageHeader cmd_header;
    
    while (g_running && recv_header(g_ns_socket, &cmd_header) == 0) {
        
        MessageHeader ack_header;
        memset(&ack_header, 0, sizeof(ack_header));
        ack_header.msg_type = MSG_ACK;
        ack_header.source_component = COMPONENT_STORAGE_SERVER;
        ack_header.dest_component = COMPONENT_NAME_SERVER;

        switch (cmd_header.msg_type) {
            
            case MSG_CREATE:
            {
                write_log("INFO", "NS forwarded MSG_CREATE for '%s'", cmd_header.filename);
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "data/ss_%d/files/%s", g_my_port, cmd_header.filename);
                FILE *f = fopen(filepath, "w");
                if (f) {
                    fclose(f);
                    add_metadata_entry(g_meta_dir, cmd_header.filename);
                    send_header(g_ns_socket, &ack_header);
                } else { /* TODO: Send MSG_ERROR */ }
                break;
            }
                
            case MSG_DELETE:
            {
                write_log("INFO", "NS forwarded MSG_DELETE for '%s'", cmd_header.filename);
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "data/ss_%d/files/%s", g_my_port, cmd_header.filename);
                if (remove(filepath) == 0) {
                    remove_metadata_entry(g_meta_dir, cmd_header.filename);
                    send_header(g_ns_socket, &ack_header);
                } else { /* TODO: Send MSG_ERROR */ }
                break;
            }

            case MSG_UNDO:
            {
                write_log("INFO", "NS forwarded MSG_UNDO for '%s'", cmd_header.filename);
                // TODO: The username ("NameServer") is wrong.
                // NS needs to send the real username in the payload.
                int result = perform_undo(cmd_header.filename, g_my_port, "NameServer");
                if (result == 1) {
                    update_metadata_entry(g_meta_dir, cmd_header.filename);
                    
                    // Invalidate cache after undo
                    // Cache removed for simplicity
                    
                    send_header(g_ns_socket, &ack_header);
                } else { /* TODO: Send MSG_ERROR */ }
                break;
            }

            case MSG_INTERNAL_GET_METADATA:
            {
                write_log("INFO", "NS requested metadata for '%s'", cmd_header.filename);
                SSMetadataPayload meta_payload;
                memset(&meta_payload, 0, sizeof(meta_payload));
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].filename, cmd_header.filename) == 0) {
                        meta_payload.char_count = file_table[i].size;
                        meta_payload.word_count = file_table[i].word_count;
                        meta_payload.created = file_table[i].created;
                        meta_payload.last_modified = file_table[i].modified;
                        meta_payload.last_accessed = file_table[i].last_accessed;
                        strncpy(meta_payload.last_accessed_by, file_table[i].last_accessed_by, 64 - 1);
                        break;
                    }
                }

                MessageHeader resp_header;
                memset(&resp_header, 0, sizeof(resp_header));
                resp_header.msg_type = MSG_INTERNAL_METADATA_RESP;
                resp_header.source_component = COMPONENT_STORAGE_SERVER;
                resp_header.payload_length = sizeof(SSMetadataPayload);
                
                send_header(g_ns_socket, &resp_header);
                send_all(g_ns_socket, &meta_payload, sizeof(meta_payload));
                break;
            }

            case MSG_INTERNAL_SET_OWNER:
            {
                write_log("INFO", "NS set owner for '%s'", cmd_header.filename);
                // The owner's username is sent as payload. Read it and set owner.
                if (cmd_header.payload_length > 0 && cmd_header.payload_length < 256) {
                    char owner_buf[256];
                    if (recv_all(g_ns_socket, owner_buf, cmd_header.payload_length) == 0) {
                        owner_buf[cmd_header.payload_length - 1] = '\0';
                        persist_set_owner(g_meta_dir, cmd_header.filename, owner_buf);
                        write_log("INFO", "Persisted owner '%s' for file '%s'", owner_buf, cmd_header.filename);
                    }
                } else {
                    write_log("WARN", "MSG_INTERNAL_SET_OWNER with empty or too large payload for '%s'", cmd_header.filename);
                }
                // No ACK needed
                break;
            }

            case MSG_INTERNAL_READ: // This is 100
            {
                write_log("INFO", "NS requested file content for '%s'", cmd_header.filename);
                
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "data/ss_%d/files/%s", g_my_port, cmd_header.filename);
                
                FILE *f = fopen(filepath, "r");
                long file_size = 0;
                char* file_content = NULL;

                if (f) {
                    // File exists, read it
                    fseek(f, 0, SEEK_END);
                    file_size = ftell(f);
                    fseek(f, 0, SEEK_SET);

                    file_content = malloc(file_size + 1); // +1 for safety
                    if (file_content) {
                        file_size = fread(file_content, 1, file_size, f);
                        file_content[file_size] = '\0';
                    } else {
                        write_log("ERROR", "Malloc failed for MSG_INTERNAL_READ");
                        file_size = 0; // Send no data
                    }
                    fclose(f);
                } else {
                    // File not found, will send no data
                    write_log("WARN", "NS requested '%s' for EXEC, but file not found.", cmd_header.filename);
                    file_size = 0;
                }

                // Send response header (ALWAYS send this)
                MessageHeader resp_header;
                memset(&resp_header, 0, sizeof(resp_header));
                resp_header.msg_type = MSG_INTERNAL_DATA; // This is 101
                resp_header.source_component = COMPONENT_STORAGE_SERVER;
                resp_header.payload_length = file_size;
                
                if (send_header(g_ns_socket, &resp_header) == -1) {
                    // Failed to send header, just free and break
                    if (file_content) free(file_content);
                    break; // NS will disconnect
                }

                // Send payload (if any)
                if (file_size > 0 && file_content) {
                    send_all(g_ns_socket, file_content, file_size);
                }
                
                if (file_content) free(file_content);
                break;
            }
            
            case MSG_INTERNAL_ADD_ACCESS:
            {
                AccessControlPayload payload;
                if (recv_all(g_ns_socket, &payload, sizeof(payload)) == 0) {
                    persist_set_acl(g_meta_dir, cmd_header.filename, payload.target_username, payload.permission);
                    write_log("INFO", "NS set ACL for '%s': User %s -> Perm %d",
                              cmd_header.filename, payload.target_username, payload.permission);
                    send_header(g_ns_socket, &ack_header);
                }
                break;
            }
                
            case MSG_INTERNAL_REM_ACCESS:
            {
                // The header.filename is the target file. The payload contains the username to remove.
                if (cmd_header.payload_length > 0 && cmd_header.payload_length < 256) {
                    char target_user[256];
                    if (recv_all(g_ns_socket, target_user, cmd_header.payload_length) == 0) {
                        target_user[cmd_header.payload_length - 1] = '\0';
                        persist_remove_acl(g_meta_dir, cmd_header.filename, target_user);
                        write_log("INFO", "NS removed ACL for '%s': User %s",
                                  cmd_header.filename, target_user);
                        send_header(g_ns_socket, &ack_header);
                    }
                } else {
                    write_log("WARN", "MSG_INTERNAL_REM_ACCESS with empty or too large payload for '%s'", cmd_header.filename);
                }
                break;
            }

            default:
                write_log("WARN", "Received unknown command from NS: %d", cmd_header.msg_type);
        }
    }
    
    write_log("FATAL", "Name Server disconnected. Stopping client listener...");
    g_running = 0; // Signal the client listener thread to stop
}

int register_with_name_server(const char* ns_ip, int ns_port) {
    g_ns_socket = create_socket();
    if (connect_socket_no_exit(g_ns_socket, ns_ip, ns_port) == -1) {
        write_log("FATAL", "Could not connect to Name Server at %s:%d. Exiting.", ns_ip, ns_port);
        return -1;
    }
    write_log("INFO", "Connected to Name Server. Registering...");

    // 1. Send Registration
    MessageHeader reg_header;
    memset(&reg_header, 0, sizeof(reg_header));
    reg_header.msg_type = MSG_REGISTER;
    reg_header.source_component = COMPONENT_STORAGE_SERVER;
    reg_header.payload_length = sizeof(SSRegistrationPayload);

    SSRegistrationPayload reg_payload;
    memset(&reg_payload, 0, sizeof(reg_payload));
    strncpy(reg_payload.ip_addr, g_my_ip, 64);
    reg_payload.client_facing_port = g_my_port;
    
    if (send_header(g_ns_socket, &reg_header) == -1) { close(g_ns_socket); return -1; }
    if (send_all(g_ns_socket, &reg_payload, sizeof(reg_payload)) == -1) { close(g_ns_socket); return -1; }

    // 2. Wait for ACK
    MessageHeader ack_header;
    if (recv_header(g_ns_socket, &ack_header) == -1 || ack_header.msg_type != MSG_ACK) {
        write_log("FATAL", "Name Server did not ACK registration. Exiting.");
        close(g_ns_socket);
        return -1;
    }
    write_log("INFO", "Registration ACK received. Sending file list...");

    // 3. Send File List (from persistence.c's file_table)
    for (int i = 0; i < file_count; i++) {
        SSFileRecordPayload file_payload;
        memset(&file_payload, 0, sizeof(file_payload));
        strncpy(file_payload.filename, file_table[i].filename, MAX_FILENAME - 1);
        strncpy(file_payload.owner_username, file_table[i].owner_username, 64 - 1);
        file_payload.acl_count = file_table[i].acl_count;
        memcpy(file_payload.acl, file_table[i].acl, sizeof(AclEntryPayload) * file_table[i].acl_count);
        file_payload.word_count = file_table[i].word_count;
        file_payload.char_count = file_table[i].size;
        file_payload.created = file_table[i].created;
        file_payload.modified = file_table[i].modified;
        file_payload.last_accessed = file_table[i].last_accessed;
        strncpy(file_payload.last_accessed_by, file_table[i].last_accessed_by, 64 - 1);

        MessageHeader file_header;
        memset(&file_header, 0, sizeof(file_header));
        file_header.msg_type = MSG_REGISTER_FILE;
        file_header.source_component = COMPONENT_STORAGE_SERVER;
        file_header.payload_length = sizeof(SSFileRecordPayload);
        
        if (send_header(g_ns_socket, &file_header) == -1) { close(g_ns_socket); return -1; }
        if (send_all(g_ns_socket, &file_payload, sizeof(file_payload)) == -1) { close(g_ns_socket); return -1; }
    }
    
    // 4. Send "Complete"
    MessageHeader complete_header;
    memset(&complete_header, 0, sizeof(complete_header));
    complete_header.msg_type = MSG_REGISTER_COMPLETE;
    complete_header.source_component = COMPONENT_STORAGE_SERVER;
    if (send_header(g_ns_socket, &complete_header) == -1) { close(g_ns_socket); return -1; }
    
    write_log("INFO", "File list sync complete. Registration successful.");
    return 0; // Success
}

// Handle Ctrl+C
void handle_sigint(int sig) {
    (void)sig;
    printf("\n[SS %d] Caught SIGINT (Ctrl+C), shutting down...\n", g_my_port);
    g_running = 0;
    if (g_ns_socket != -1) {
        close(g_ns_socket);
    }
    int self_sock = create_socket();
    if (connect_socket_no_exit(self_sock, g_my_ip, g_my_port) != -1) {
        close(self_sock);
    } else {
        close(self_sock);
    }
}

// =========================================================================
//  CLIENT HANDLER THREAD
// =========================================================================

void *client_handler_thread(void *arg) {
    client_ctx_t *ctx = (client_ctx_t *)arg;
    int fd = ctx->client_fd;
    add_client_fd(fd);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &ctx->client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(ctx->client_addr.sin_port);

    char buf[BUF_SZ];
    ssize_t r = recv(fd, buf, sizeof(buf) - 1, 0);
    if (r <= 0) {
        close(fd);
        remove_client_fd(fd);
        free(ctx);
        return NULL;
    }
    buf[r] = '\0';

    char username[128] = "N/A";
    if (sscanf(buf, "USER %127s", username) == 1) {
        set_logger_username(username);
        write_log("ACTION", "Direct connection from %s:%d USER=%s", client_ip, client_port, username);
    } else {
        write_log("WARN", "Direct connection from %s:%d without USER handshake", client_ip, client_port);
    }

    const char *ack = "OK_200 USER_ACCEPTED\n";
    send(fd, ack, strlen(ack), 0);

    printf("[SERVER %d] Connected: %s:%d (%s)\n", ctx->server_port, client_ip, client_port, username);

    while (g_running) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl) *nl = '\0';

        write_log("REQUEST", "DIRECT USER=%s CMD=\"%s\"", username, buf);
        
        char current_file[256];
        int current_sentence;
        int is_in_write_mode = get_client_write_info(fd, current_file, &current_sentence);
        
        if (is_in_write_mode && strncmp(buf, "ETIRW", 5) == 0) {
            char files_dir[256], meta_dir[256];
            snprintf(files_dir, sizeof(files_dir), "data/ss_%d/files", ctx->server_port);
            snprintf(meta_dir, sizeof(meta_dir), "data/ss_%d/metadata", ctx->server_port);
            
            char orig_path[512], swap_path[512];
            snprintf(orig_path, sizeof(orig_path), "%s/%s", files_dir, current_file);
            snprintf(swap_path, sizeof(swap_path), "%s/%s_%d_%d.swap", files_dir, current_file, current_sentence, fd);

            FILE* swap_check = fopen(swap_path, "r");
            if (swap_check) {
                fclose(swap_check);
                create_file_backup(current_file, ctx->server_port, username);
                
                // *** FIXED CONCURRENT MERGING LOGIC WITH PROPER DELIMITER HANDLING ***
                
                // 1. Read the LATEST state of the original file
                FILE* current_orig_file = fopen(orig_path, "r");
                char current_orig_content[8192] = {0};
                size_t current_orig_bytes = 0;
                
                if (current_orig_file) {
                    current_orig_bytes = fread(current_orig_content, 1, sizeof(current_orig_content) - 1, current_orig_file);
                    fclose(current_orig_file);
                    current_orig_content[current_orig_bytes] = '\0';
                }
                
                // 2. Read this client's swap content
                FILE* swap_file = fopen(swap_path, "r");
                char swap_content[8192] = {0};
                size_t swap_bytes = fread(swap_content, 1, sizeof(swap_content) - 1, swap_file);
                fclose(swap_file);
                swap_content[swap_bytes] = '\0';
                
                // 3. Parse CURRENT original file into words and sentences WITH FIXED LOGIC
                char current_words[1024][512];
                int current_word_count = 0;
                sentence_info_t current_sentences[256];
                int current_sentence_count = 0;
                
                if (current_orig_bytes > 0) {
                    char temp[8192];
                    strcpy(temp, current_orig_content);
                    
                    // Tokenize current content
                    char *token = strtok(temp, " \t\n");
                    while (token && current_word_count < 1024) {
                        strcpy(current_words[current_word_count], token);
                        current_word_count++;
                        token = strtok(NULL, " \t\n");
                    }
                    
                    // *** FIXED: Parse current sentence boundaries with SPACE-SEPARATED delimiter handling ***
                    int sent_start = 0;
                    for (int i = 0; i < current_word_count; i++) {
                        int len = strlen(current_words[i]);
                        if (len > 0) {
                            char last_char = current_words[i][len - 1];
                            if (last_char == '.' || last_char == '!' || last_char == '?') {
                                // Found a delimiter at end of word - end current sentence
                                if (current_sentence_count < 256) {
                                    current_sentences[current_sentence_count].start_word_idx = sent_start;
                                    current_sentences[current_sentence_count].end_word_idx = i;
                                    current_sentences[current_sentence_count].delimiter = last_char;
                                    current_sentence_count++;
                                    sent_start = i + 1;  // Next sentence starts from next word
                                }
                            }
                        }
                    }
                    
                    // Handle incomplete sentence at end
                    if (sent_start < current_word_count && current_sentence_count < 256) {
                        current_sentences[current_sentence_count].start_word_idx = sent_start;
                        current_sentences[current_sentence_count].end_word_idx = current_word_count - 1;
                        current_sentences[current_sentence_count].delimiter = '\0';
                        current_sentence_count++;
                    }
                }
                
                // 4. Parse SWAP content into words and sentences WITH FIXED LOGIC
                char swap_words[1024][512];
                int swap_word_count = 0;
                sentence_info_t swap_sentences[256];
                int swap_sentence_count = 0;
                
                if (swap_bytes > 0) {
                    char temp[8192];
                    strcpy(temp, swap_content);
                    
                    // Tokenize swap content
                    char *token = strtok(temp, " \t\n");
                    while (token && swap_word_count < 1024) {
                        strcpy(swap_words[swap_word_count], token);
                        swap_word_count++;
                        token = strtok(NULL, " \t\n");
                    }
                    
                    // *** FIXED: Parse swap sentence boundaries with SPACE-SEPARATED delimiter handling ***
                    int sent_start = 0;
                    for (int i = 0; i < swap_word_count; i++) {
                        int len = strlen(swap_words[i]);
                        if (len > 0) {
                            char last_char = swap_words[i][len - 1];
                            if (last_char == '.' || last_char == '!' || last_char == '?') {
                                // Found a delimiter at end of word - end current sentence
                                if (swap_sentence_count < 256) {
                                    swap_sentences[swap_sentence_count].start_word_idx = sent_start;
                                    swap_sentences[swap_sentence_count].end_word_idx = i;
                                    swap_sentences[swap_sentence_count].delimiter = last_char;
                                    swap_sentence_count++;
                                    sent_start = i + 1;  // Next sentence starts from next word
                                }
                            }
                        }
                    }
                    
                    if (sent_start < swap_word_count && swap_sentence_count < 256) {
                        swap_sentences[swap_sentence_count].start_word_idx = sent_start;
                        swap_sentences[swap_sentence_count].end_word_idx = swap_word_count - 1;
                        swap_sentences[swap_sentence_count].delimiter = '\0';
                        swap_sentence_count++;
                    }
                }
                
                // 5. *** SMART MERGE: Replace ONLY the target sentence ***
                char final_content[8192] = "";
                
                if (current_sentence_count == 0) {
                    // No sentences in current file, use entire swap content
                    strcpy(final_content, swap_content);
                } else if (current_sentence > current_sentence_count) {
                    // Adding new sentence beyond existing ones
                    strcpy(final_content, current_orig_content);
                    if (strlen(final_content) > 0) {
                        strcat(final_content, " ");
                    }
                    
                    // Find the target sentence in swap content (should be the last one)
                    if (swap_sentence_count > 0) {
                        int target_sent_in_swap = swap_sentence_count - 1; // Last sentence
                        for (int w = swap_sentences[target_sent_in_swap].start_word_idx; 
                             w <= swap_sentences[target_sent_in_swap].end_word_idx; w++) {
                            if (w > swap_sentences[target_sent_in_swap].start_word_idx) {
                                strcat(final_content, " ");
                            }
                            strcat(final_content, swap_words[w]);
                        }
                    }
                } else {
                    // Replace specific sentence in existing content
                    
                    // Add sentences BEFORE target sentence from CURRENT file
                    for (int s = 0; s < current_sentence - 1; s++) {
                        if (strlen(final_content) > 0) {
                            strcat(final_content, " ");
                        }
                        
                        for (int w = current_sentences[s].start_word_idx; 
                             w <= current_sentences[s].end_word_idx; w++) {
                            if (w > current_sentences[s].start_word_idx) {
                                strcat(final_content, " ");
                            }
                            strcat(final_content, current_words[w]);
                        }
                    }
                    
                    // Add the MODIFIED sentence from SWAP content
                    // Find corresponding sentence in swap (should be at same index)
                    int target_sent_in_swap = current_sentence - 1;
                    if (target_sent_in_swap < swap_sentence_count) {
                        if (strlen(final_content) > 0) {
                            strcat(final_content, " ");
                        }
                        
                        for (int w = swap_sentences[target_sent_in_swap].start_word_idx; 
                             w <= swap_sentences[target_sent_in_swap].end_word_idx; w++) {
                            if (w > swap_sentences[target_sent_in_swap].start_word_idx) {
                                strcat(final_content, " ");
                            }
                            strcat(final_content, swap_words[w]);
                        }
                    }
                    
                    // Add sentences AFTER target sentence from CURRENT file
                    for (int s = current_sentence; s < current_sentence_count; s++) {
                        if (strlen(final_content) > 0) {
                            strcat(final_content, " ");
                        }
                        
                        for (int w = current_sentences[s].start_word_idx; 
                             w <= current_sentences[s].end_word_idx; w++) {
                            if (w > current_sentences[s].start_word_idx) {
                                strcat(final_content, " ");
                            }
                            strcat(final_content, current_words[w]);
                        }
                    }
                }
                
                // 6. Write the final merged content
                FILE* final_file = fopen(orig_path, "w");
                if (final_file) {
                    fprintf(final_file, "%s", final_content);
                    fclose(final_file);
                    remove(swap_path);
                    
                    // Cache removed for simplicity
                    update_metadata_entry(meta_dir, current_file);
                    send(fd, "OK_200 WRITE COMPLETED\n", 24, 0);
                    
                    printf("[SERVER %d] WRITE completed for %s [Sentence %d] by %s (MERGED WITH CONCURRENT CHANGES)\n",
                           ctx->server_port, current_file, current_sentence, username);
                    
                    write_log("INFO", "WRITE completed with concurrent merge for %s [Sentence %d] by %s", 
                             current_file, current_sentence, username);
                } else {
                    write_log("ERROR", "WRITE failed: Could not finalize merged changes to %s", current_file);
                    send(fd, "ERR_500 Could not finalize changes\n", 35, 0);
                }
            } else {
                // No changes were made, just release lock
                write_log("INFO", "WRITE completed without changes to %s sentence %d", current_file, current_sentence);
                send(fd, "OK_200 WRITE COMPLETED\n", 24, 0);
            }

            printf("[SERVER %d] Released WRITE lock for %s [Sentence %d] by %s\n",
                   ctx->server_port, current_file, current_sentence, username);
            remove_sentence_lock(current_file, current_sentence, fd);
            continue;
        }

        if (is_in_write_mode) {
            int word_idx;
            char new_content[2048];
            
            if (sscanf(buf, "%d %2047[^\n]", &word_idx, new_content) == 2) {
                if (word_idx < 1) {
                    send(fd, "ERR_400 Word index must be positive (1-based)\n", 46, 0);
                    continue;
                }

                char files_dir[256];
                snprintf(files_dir, sizeof(files_dir), "data/ss_%d/files", ctx->server_port);
                char orig_path[512], swap_path[512];
                snprintf(orig_path, sizeof(orig_path), "%s/%s", files_dir, current_file);
                snprintf(swap_path, sizeof(swap_path), "%s/%s_%d_%d.swap", files_dir, current_file, current_sentence, fd);

                // Read current state (from swap file if exists, otherwise original)
                FILE *current_file_ptr = fopen(swap_path, "r");
                if (!current_file_ptr) {
                    current_file_ptr = fopen(orig_path, "r");
                }
                
                if (!current_file_ptr) {
                    send(fd, "ERR_404 File not found during update\n", 37, 0);
                    continue;
                }

                char content[8192] = {0};
                size_t bytes_read = fread(content, 1, sizeof(content) - 1, current_file_ptr);
                fclose(current_file_ptr);
                content[bytes_read] = '\0';

                // Handle empty file case
                if (bytes_read == 0 && current_sentence == 1) {
                    if (word_idx == 1) {
                        FILE *swap = fopen(swap_path, "w");
                        if (!swap) {
                            send(fd, "ERR_500 Could not create temporary file\n", 40, 0);
                            continue;
                        }
                        fprintf(swap, "%s", new_content);
                        fclose(swap);
                        send(fd, "OK_200 CONTENT INSERTED\n", 24, 0);
                        continue;
                    } else {
                        send(fd, "ERR_404 Empty file: only word index 1 allowed\n", 46, 0);
                        continue;
                    }
                }

                // *** FIXED WORD INSERTION LOGIC ***
                
                // 1. Tokenize content into words
                char all_words[1024][512];
                int total_word_count = 0;
                char temp_content[8192];
                strcpy(temp_content, content);
                
                char *token = strtok(temp_content, " \t\n");
                while (token && total_word_count < 1024) {
                    strcpy(all_words[total_word_count], token);
                    total_word_count++;
                    token = strtok(NULL, " \t\n");
                }

                // 2. Parse sentence boundaries using FIXED logic
                sentence_info_t sentence_info[256];
                int total_sentences = 0;
                int current_sent_start = 0;
                
                for (int i = 0; i < total_word_count; i++) {
                    int word_len = strlen(all_words[i]);
                    if (word_len > 0) {
                        char last_char = all_words[i][word_len - 1];
                        if (last_char == '.' || last_char == '!' || last_char == '?') {
                            if (total_sentences < 256) {
                                sentence_info[total_sentences].start_word_idx = current_sent_start;
                                sentence_info[total_sentences].end_word_idx = i;
                                sentence_info[total_sentences].delimiter = last_char;
                                total_sentences++;
                                current_sent_start = i + 1;
                            }
                        }
                    }
                }
                
                // Handle incomplete sentence at the end
                if (current_sent_start < total_word_count && total_sentences < 256) {
                    sentence_info[total_sentences].start_word_idx = current_sent_start;
                    sentence_info[total_sentences].end_word_idx = total_word_count - 1;
                    sentence_info[total_sentences].delimiter = '\0';
                    total_sentences++;
                }

                // Handle case where there are no complete sentences but content exists
                if (total_sentences == 0 && total_word_count > 0) {
                    sentence_info[0].start_word_idx = 0;
                    sentence_info[0].end_word_idx = total_word_count - 1;
                    sentence_info[0].delimiter = '\0';
                    total_sentences = 1;
                }

                // 3. Handle writing to new sentence beyond existing ones
                if (current_sentence > total_sentences) {
                    if (word_idx == 1) {
                        char updated_content[8192];
                        if (bytes_read > 0) {
                            snprintf(updated_content, sizeof(updated_content), "%s %s", content, new_content);
                        } else {
                            strcpy(updated_content, new_content);
                        }
                        
                        FILE *swap = fopen(swap_path, "w");
                        if (!swap) {
                            send(fd, "ERR_500 Could not create temporary file\n", 40, 0);
                            continue;
                        }
                        fprintf(swap, "%s", updated_content);
                        fclose(swap);
                        send(fd, "OK_200 CONTENT INSERTED\n", 24, 0);
                        continue;
                    } else {
                        send(fd, "ERR_404 New sentence: only word index 1 allowed\n", 48, 0);
                        continue;
                    }
                }

                // 4. Get current sentence boundaries
                int sent_start = sentence_info[current_sentence - 1].start_word_idx;
                int sent_end = sentence_info[current_sentence - 1].end_word_idx;
                int original_sentence_word_count = sent_end - sent_start + 1;

                // 5. Validate word index within sentence
                if (word_idx > original_sentence_word_count + 1) {
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), 
                            "ERR_404 Word index %d out of range. Sentence %d has %d words (positions 1-%d available)\n", 
                            word_idx, current_sentence, original_sentence_word_count, original_sentence_word_count + 1);
                    send(fd, err_msg, strlen(err_msg), 0);
                    continue;
                }

                // 6. *** CORRECTED INSERTION LOGIC ***
                char new_all_words[1024][512];
                int new_total_words = 0;
                
                // Copy words BEFORE the target sentence
                for (int i = 0; i < sent_start; i++) {
                    strcpy(new_all_words[new_total_words], all_words[i]);
                    new_total_words++;
                }
                
                // Process the target sentence with insertion
                // *** KEY FIX: Handle delimiter separation properly ***

                // First, let's separate the delimiter from the last word of the sentence if it exists
                char sentence_words[256][512];
                int adjusted_sentence_word_count = 0;  // *** RENAMED to avoid conflict ***
                char sentence_delimiter = '\0';

                for (int i = sent_start; i <= sent_end; i++) {
                    strcpy(sentence_words[adjusted_sentence_word_count], all_words[i]);
                    
                    // Check if this is the last word and has a delimiter
                    if (i == sent_end) {
                        int word_len = strlen(sentence_words[adjusted_sentence_word_count]);
                        if (word_len > 0) {
                            char last_char = sentence_words[adjusted_sentence_word_count][word_len - 1];
                            if (last_char == '.' || last_char == '!' || last_char == '?') {
                                // Remove delimiter from word and store it separately
                                sentence_delimiter = last_char;
                                sentence_words[adjusted_sentence_word_count][word_len - 1] = '\0';
                                
                                // If word becomes empty after removing delimiter, don't add it
                                if (strlen(sentence_words[adjusted_sentence_word_count]) > 0) {
                                    adjusted_sentence_word_count++;
                                }
                            } else {
                                adjusted_sentence_word_count++;
                            }
                        } else {
                            adjusted_sentence_word_count++;
                        }
                    } else {
                        adjusted_sentence_word_count++;
                    }
                }

                // Validate word index within the adjusted sentence
                if (word_idx > adjusted_sentence_word_count + 1) {
                    char err_msg[256];
                    snprintf(err_msg, sizeof(err_msg), 
                            "ERR_404 Word index %d out of range. Sentence %d has %d words (positions 1-%d available)\n", 
                            word_idx, current_sentence, adjusted_sentence_word_count, adjusted_sentence_word_count + 1);
                    send(fd, err_msg, strlen(err_msg), 0);
                    continue;
                }

                // Now insert the new content at the correct position
                for (int i = 0; i < adjusted_sentence_word_count; i++) {
                    int position_in_sentence = i + 1; // 1-based position
                    
                    // If this is the insertion point, insert new content first
                    if (position_in_sentence == word_idx) {
                        // Split new content into words and add them
                        char temp_new_content[2048];
                        strcpy(temp_new_content, new_content);
                        
                        char *new_token = strtok(temp_new_content, " \t");
                        while (new_token) {
                            strcpy(new_all_words[new_total_words], new_token);
                            new_total_words++;
                            new_token = strtok(NULL, " \t");
                        }
                    }
                    
                    // Add the original word at this position
                    strcpy(new_all_words[new_total_words], sentence_words[i]);
                    new_total_words++;
                }

                // Handle insertion at the very end of the sentence (after last word but before delimiter)
                if (word_idx > adjusted_sentence_word_count) {
                    // Insert new content at the end
                    char temp_new_content[2048];
                    strcpy(temp_new_content, new_content);
                    
                    char *new_token = strtok(temp_new_content, " \t");
                    while (new_token) {
                        strcpy(new_all_words[new_total_words], new_token);
                        new_total_words++;
                        new_token = strtok(NULL, " \t");
                    }
                }

                // Add the delimiter back to the last word of this sentence if it existed
                if (sentence_delimiter != '\0' && new_total_words > 0) {
                    // Find the last word that belongs to this sentence
                    int last_sentence_word_idx = new_total_words - 1;
                    
                    // Add delimiter to the last word
                    int current_len = strlen(new_all_words[last_sentence_word_idx]);
                    if (current_len < 511) { // Make sure we have space
                        new_all_words[last_sentence_word_idx][current_len] = sentence_delimiter;
                        new_all_words[last_sentence_word_idx][current_len + 1] = '\0';
                    }
                }

                // Copy words AFTER the target sentence
                for (int i = sent_end + 1; i < total_word_count; i++) {
                    strcpy(new_all_words[new_total_words], all_words[i]);
                    new_total_words++;
                }

                // 7. Reconstruct the final content
                char final_content[8192] = "";
                for (int i = 0; i < new_total_words; i++) {
                    if (i > 0) {
                        strcat(final_content, " ");
                    }
                    strcat(final_content, new_all_words[i]);
                }

                // 8. Write to swap file
                FILE *swap = fopen(swap_path, "w");
                if (!swap) {
                    send(fd, "ERR_500 Could not create temporary file\n", 40, 0);
                    continue;
                }
                fprintf(swap, "%s", final_content);
                fclose(swap);

                // 9. Send response
                send(fd, "OK_200 CONTENT INSERTED\n", 24, 0);
                write_log("INFO", "Content '%s' inserted at position %d in %s [Sentence %d] by user %s", 
                         new_content, word_idx, current_file, current_sentence, username);
                printf("[SERVER %d] Inserted content '%s' at position %d in %s [Sentence %d] by %s\n",
                       ctx->server_port, new_content, word_idx, current_file, current_sentence, username);
            } else {
                send(fd, "ERR_400 Invalid format. Use: <word_index> <content>\n", 52, 0);
            }
            continue;
        }

        // Parse regular commands
        char cmd[64], fname[256], rest[1024];
        int matched = sscanf(buf, "%63s %255s %[^\n]", cmd, fname, rest);

        char files_dir[256], meta_dir[256];
        snprintf(files_dir, sizeof(files_dir), "data/ss_%d/files", ctx->server_port);
        snprintf(meta_dir, sizeof(meta_dir), "data/ss_%d/metadata", ctx->server_port);

        // CREATE
        if (matched >= 1 && strcmp(cmd, "CREATE") == 0 && matched >= 2) {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname);
            FILE *f = fopen(filepath, "w");
            if (!f) {
                send(fd, "ERR_500\n", 8, 0);
            } else {
                fclose(f);
                add_metadata_entry(meta_dir, fname);
                send(fd, "OK_201 CREATED\n", 15, 0);
                 printf("[SERVER %d] File created: %s\n", ctx->server_port, fname);
            }
        }

        // READ
        else if (matched >= 1 && strcmp(cmd, "READ") == 0 && matched >= 2) {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname);
            
            // Check if file exists
            FILE *f = fopen(filepath, "r");
            if (!f) {
                send(fd, "ERR_404 File not found\n", 23, 0);
                write_log("WARN", "READ failed: File %s not found", fname);
                printf("[SERVER %d] READ failed: File %s not found (requested by %s)\n", 
                       ctx->server_port, fname, username);
            } else {
                // Get file size for logging
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, 0, SEEK_SET);
                
                if (file_size == 0) {
                    // Handle empty file
                    send(fd, "OK_200 EMPTY_FILE\n", 18, 0);
                    write_log("INFO", "READ: Empty file %s sent to user %s", fname, username);
                    printf("[SERVER %d] READ: Empty file %s sent to %s\n", 
                           ctx->server_port, fname, username);
                } else {
                    // Send file contents
                    send(fd, "OK_200 FILE_CONTENT\n", 20, 0);
                    
                    char buffer[1024];
                    size_t bytes_read;
                    size_t total_sent = 0;
                    
                    while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
                        if (send(fd, buffer, bytes_read, 0) == -1) {
                            write_log("ERROR", "Failed to send file content for %s to user %s", fname, username);
                            break;
                        }
                        total_sent += bytes_read;
                    }
                    
                    // Send end marker
                    send(fd, "\nEND_OF_FILE\n", 13, 0);
                    
                    write_log("INFO", "READ: File %s (%ld bytes) sent to user %s", fname, file_size, username);
                    printf("[SERVER %d] READ: File %s (%zu bytes sent) to %s\n", 
                           ctx->server_port, fname, total_sent, username);
                }
                
                fclose(f);
                
                // Update access metadata with username
                persist_update_last_accessed(meta_dir, fname, username);
            }
        }

        // STREAM command
        else if (matched >= 1 && strcmp(cmd, "STREAM") == 0 && matched >= 2) {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname);
            
            // Check if file exists
            FILE *f = fopen(filepath, "r");
            if (!f) {
                send(fd, "ERR_404 File not found\n", 23, 0);
                write_log("WARN", "STREAM failed: File %s not found", fname);
                printf("[SERVER %d] STREAM failed: File %s not found (requested by %s)\n", 
                       ctx->server_port, fname, username);
            } else {
                // Get file size for logging
                fseek(f, 0, SEEK_END);
                long file_size = ftell(f);
                fseek(f, 0, SEEK_SET);
                
                if (file_size == 0) {
                    // Handle empty file
                    send(fd, "OK_200 EMPTY_FILE_STREAM\n", 25, 0);
                    write_log("INFO", "STREAM: Empty file %s streamed to user %s", fname, username);
                    printf("[SERVER %d] STREAM: Empty file %s streamed to %s\n", 
                           ctx->server_port, fname, username);
                } else {
                    // Send stream start confirmation
                    send(fd, "OK_200 STREAM_START\n", 20, 0);
                    
                    // Sleep for 0.1 seconds
                    struct timespec ts;
                    ts.tv_sec = 0;
                    ts.tv_nsec = 100000000L; // 100 million nanoseconds = 0.1 seconds
                    nanosleep(&ts, NULL);   
                                        
                    // Read entire file content
                    char content[8192] = {0};
                    size_t bytes_read = fread(content, 1, sizeof(content) - 1, f);
                    content[bytes_read] = '\0';
                    
                    // Tokenize content into words
                    char words[1024][512];
                    int word_count = 0;
                    char temp_content[8192];
                    strcpy(temp_content, content);
                    
                    char *token = strtok(temp_content, " \t\n\r");
                    while (token && word_count < 1024) {
                        // Clean up the word (remove extra whitespace)
                        int len = strlen(token);
                        if (len > 0 && len < 512) {
                            strcpy(words[word_count], token);
                            word_count++;
                        }
                        token = strtok(NULL, " \t\n\r");
                    }
                    
                    write_log("INFO", "STREAM: Starting to stream %d words from %s to user %s", 
                             word_count, fname, username);
                    printf("[SERVER %d] STREAM: Starting to stream %d words from %s to %s\n", 
                           ctx->server_port, word_count, fname, username);
                    
                    // Stream words one by one with delay
                    int streaming_active = 1;
                    for (int i = 0; i < word_count && streaming_active; i++) {
                        // Send each word on its own line for easier client parsing
                        char word_packet[1024];
                        snprintf(word_packet, sizeof(word_packet), "%s", words[i]);
                        
                        ssize_t sent = send(fd, word_packet, strlen(word_packet), 0);
                        if (sent == -1) {
                            write_log("ERROR", "Failed to send word %d of %s to user %s", i + 1, fname, username);
                            streaming_active = 0;
                            break;
                        }
                        
                        // Force immediate send
                        int flag = 1;
                        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
                        
                        // 0.1 second delay (100,000 microseconds)
                        struct timespec sleep_time;
                        sleep_time.tv_sec = 0;
                        sleep_time.tv_nsec = 100000000L; // 100 million nanoseconds = 0.1 seconds
                        nanosleep(&sleep_time, NULL);
                        
                        // Check if client is still connected or wants to stop
                        char check_buf[64];
                        ssize_t check_result = recv(fd, check_buf, sizeof(check_buf) - 1, MSG_DONTWAIT | MSG_PEEK);
                        if (check_result == 0) {
                            // Client disconnected
                            write_log("WARN", "Client disconnected during STREAM of %s at word %d", fname, i + 1);
                            streaming_active = 0;
                            break;
                        } else if (check_result > 0) {
                            // Client sent data - check for control commands
                            ssize_t control_recv = recv(fd, check_buf, sizeof(check_buf) - 1, MSG_DONTWAIT);
                            if (control_recv > 0) {
                                check_buf[control_recv] = '\0';
                                if (strncmp(check_buf, "STOP", 4) == 0) {
                                    send(fd, "STREAM_STOPPED\n", 15, 0);
                                    write_log("INFO", "STREAM stopped for %s at word %d by user request", fname, i + 1);
                                    streaming_active = 0;
                                    break;
                                } else if (strncmp(check_buf, "PAUSE", 5) == 0) {
                                    send(fd, "STREAM_PAUSED\n", 14, 0);
                                    write_log("INFO", "STREAM paused for %s at word %d", fname, i + 1);
                                    // Wait for resume or stop command
                                    char resume_buf[64];
                                    ssize_t resume_recv = recv(fd, resume_buf, sizeof(resume_buf) - 1, 0);
                                    if (resume_recv > 0) {
                                        resume_buf[resume_recv] = '\0';
                                        if (strncmp(resume_buf, "RESUME", 6) == 0) {
                                            send(fd, "STREAM_RESUMED\n", 15, 0);
                                            continue;
                                        } else {
                                            streaming_active = 0;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        
                        // Re-enable Nagle's algorithm
                        flag = 0;
                        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
                    }
                    
                    // Send definitive stream end marker
                    if (streaming_active) {
                        send(fd, "STREAM_COMPLETE\n", 16, 0);
                        write_log("INFO", "STREAM: Completed streaming %s (%d words) to user %s", 
                                 fname, word_count, username);
                        printf("[SERVER %d] STREAM: Completed streaming %s (%d words) to %s\n", 
                               ctx->server_port, fname, word_count, username);
                    }
                }
                
                fclose(f);
                
                // Update access metadata with username
                persist_update_last_accessed(meta_dir, fname, username);
            }
        }

        // WRITE command
        else if (strncmp(cmd, "WRITE", 5) == 0) {
            char fname_write[256];
            int sentence_num;
            
            if (sscanf(buf, "WRITE %s %d", fname_write, &sentence_num) == 2) {
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname_write);
                
                FILE* check_file = fopen(filepath, "r");
                if (!check_file) {
                    send(fd, "ERR_404 File not found\n", 23, 0);
                    continue;
                }
                
                char content[8192] = {0};
                size_t bytes_read = fread(content, 1, sizeof(content) - 1, check_file);
                fclose(check_file);
                content[bytes_read] = '\0';
                
                int available_sentences = 0;
                
                if (bytes_read == 0) {
                    available_sentences = 1;
                } else {
                    // Use cached sentence info - this is the key optimization!
                    sentence_info_t cached_sentences[256];
                    int total_sentences = get_sentence_info_simple(fname_write, ctx->server_port, cached_sentences, 256);
                    
                    if (total_sentences == 0) {
                        available_sentences = 2;
                    } else {
                        available_sentences = total_sentences;
                        
                        // Check if last sentence is complete (only when needed)
                        if (total_sentences > 0) {
                            int last_sentence_end = cached_sentences[total_sentences - 1].end_word_idx;
                            
                            // Only tokenize to count words if we need to check completeness
                            char temp_content_for_counting[8192];
                            strcpy(temp_content_for_counting, content);
                            int total_content_words = 0;
                            
                            char *token = strtok(temp_content_for_counting, " \t\n");
                            while (token && total_content_words < 1024) {
                                total_content_words++;
                                token = strtok(NULL, " \t\n");
                            }
                            
                            if (last_sentence_end < total_content_words - 1) {
                                available_sentences = total_sentences + 1;
                            } else if (total_content_words > 0) {
                                // Check if last word has sentence delimiter
                                char temp_check[8192];
                                strcpy(temp_check, content);
                                char *check_token = strtok(temp_check, " \t\n");
                                char last_word[512] = "";
                                
                                for (int i = 0; i < total_content_words && check_token; i++) {
                                    if (i == last_sentence_end) {
                                        strcpy(last_word, check_token);
                                        break;
                                    }
                                    check_token = strtok(NULL, " \t\n");
                                }
                                
                                int word_len = strlen(last_word);
                                if (word_len > 0) {
                                    char last_char = last_word[word_len - 1];
                                    if (last_char == '.' || last_char == '!' || last_char == '?') {
                                        available_sentences = total_sentences + 1;
                                    } else {
                                        available_sentences = total_sentences;
                                    }
                                }
                            }
                        }
                    }
                }
                
                // Validation
                if (sentence_num < 1) {
                    send(fd, "ERR_404 Sentence number must be positive\n", 41, 0);
                    continue;
                }
                
                if (sentence_num > available_sentences) {
                    char err_msg[256];
                    if (available_sentences == 1) {
                        snprintf(err_msg, sizeof(err_msg), 
                            "ERR_404 Sentence %d not available. File allows sentence 1 only.\n", 
                            sentence_num);
                    } else {
                        snprintf(err_msg, sizeof(err_msg), 
                            "ERR_404 Sentence %d not available. File allows sentences 1-%d.\n", 
                            sentence_num, available_sentences);
                    }
                    send(fd, err_msg, strlen(err_msg), 0);
                    write_log("WARN", "WRITE failed: Sentence %d out of range (1-%d) for file %s", 
                             sentence_num, available_sentences, fname_write);
                    continue;
                }
                
                // Locking logic
                if (is_sentence_locked(fname_write, sentence_num, fd)) {
                    send(fd, "ERR_409 This sentence is currently being edited by another user\n", 64, 0);
                    write_log("WARN", "WRITE blocked: %s sentence %d already locked by another user", fname_write, sentence_num);
                } else {
                    add_sentence_lock(fname_write, sentence_num, fd);
                    send(fd, "OK_200 WRITE MODE ENABLED\n", 27, 0);
                    write_log("INFO", "WRITE lock acquired on %s [Sentence %d] by user %s (Available: 1-%d)", 
                             fname_write, sentence_num, username, available_sentences);
                    printf("[SERVER %d] WRITE lock on %s [Sentence %d] by %s (Available: 1-%d)\n",
                           ctx->server_port, fname_write, sentence_num, username, available_sentences);
                }
            }
        }

        // UNDO command
        else if (matched >= 1 && strcmp(cmd, "UNDO") == 0 && matched >= 2) {
            int file_locked = 0;
            pthread_mutex_lock(&lock_mutex);
            sentence_lock_t *curr = sentence_locks;
            while (curr) {
                if (strcmp(curr->filename, fname) == 0) {
                    file_locked = 1;
                    break;
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&lock_mutex);
            
            if (file_locked) {
                send(fd, "ERR_409 Cannot undo: file is currently being edited\n", 52, 0);
                write_log("WARN", "UNDO blocked: file %s is currently being edited", fname);
                continue;
            }
            
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname);
            
            FILE* check_file = fopen(filepath, "r");
            if (!check_file) {
                send(fd, "ERR_404 File not found\n", 23, 0);
                write_log("ERROR", "UNDO failed: File %s not found", fname);
                continue;
            }
            fclose(check_file);
            
            int undo_result = perform_undo(fname, ctx->server_port, username);
            
            if (undo_result == 1) {
                char meta_dir_undo[256];
                snprintf(meta_dir_undo, sizeof(meta_dir_undo), "data/ss_%d/metadata", ctx->server_port);
                update_metadata_entry(meta_dir_undo, fname);
                
                // Invalidate cache after undo
                // Cache removed for simplicity
                
                send(fd, "OK_200 UNDO COMPLETED\n", 22, 0);
                write_log("INFO", "UNDO successful for file %s by user %s", fname, username);
                printf("[SERVER %d] UNDO completed for file %s by %s\n", 
                       ctx->server_port, fname, username);
            } else if (undo_result == 0) {
                send(fd, "ERR_404 No undo history available for this file\n", 48, 0);
                write_log("WARN", "UNDO failed: No history available for file %s", fname);
            } else {
                send(fd, "ERR_500 UNDO operation failed\n", 30, 0);
                write_log("ERROR", "UNDO operation failed for file %s", fname);
            }
        }

        // CHECKPOINT command
        else if (matched >= 1 && strcmp(cmd, "CHECKPOINT") == 0 && matched >= 3) {
            char checkpoint_tag[256];
            if (sscanf(buf, "CHECKPOINT %255s %255s", fname, checkpoint_tag) == 2) {
                // Check if file is currently being edited
                int file_locked = 0;
                pthread_mutex_lock(&lock_mutex);
                sentence_lock_t *curr = sentence_locks;
                while (curr) {
                    if (strcmp(curr->filename, fname) == 0) {
                        file_locked = 1;
                        break;
                    }
                    curr = curr->next;
                }
                pthread_mutex_unlock(&lock_mutex);
                
                if (file_locked) {
                    send(fd, "ERR_409 Cannot create checkpoint: file is currently being edited\n", 65, 0);
                    write_log("WARN", "CHECKPOINT blocked: file %s is currently being edited", fname);
                    continue;
                }
                
                // Check if file exists
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname);
                FILE* check_file = fopen(filepath, "r");
                if (!check_file) {
                    send(fd, "ERR_404 File not found\n", 23, 0);
                    write_log("ERROR", "CHECKPOINT failed: File %s not found", fname);
                    continue;
                }
                fclose(check_file);
                
                int result = create_checkpoint(fname, checkpoint_tag, ctx->server_port, username);
                
                if (result == 1) {
                    send(fd, "OK_200 CHECKPOINT CREATED\n", 27, 0);
                    write_log("INFO", "CHECKPOINT '%s' created for file %s by user %s", checkpoint_tag, fname, username);
                    printf("[SERVER %d] CHECKPOINT '%s' created for file %s by %s\n", 
                           ctx->server_port, checkpoint_tag, fname, username);
                } else if (result == -2) {
                    send(fd, "ERR_409 Checkpoint tag already exists\n", 38, 0);
                    write_log("WARN", "CHECKPOINT failed: Tag '%s' already exists for file %s", checkpoint_tag, fname);
                } else {
                    send(fd, "ERR_500 Failed to create checkpoint\n", 36, 0);
                    write_log("ERROR", "CHECKPOINT creation failed for file %s", fname);
                }
            } else {
                send(fd, "ERR_400 Invalid format. Use: CHECKPOINT <filename> <tag>\n", 58, 0);
            }
        }

        // VIEWCHECKPOINT command  
        else if (matched >= 1 && strcmp(cmd, "VIEWCHECKPOINT") == 0 && matched >= 3) {
            char checkpoint_tag[256];
            if (sscanf(buf, "VIEWCHECKPOINT %255s %255s", fname, checkpoint_tag) == 2) {
                char content_buffer[8192];
                int result = view_checkpoint(fname, checkpoint_tag, ctx->server_port, content_buffer, sizeof(content_buffer));
                
                if (result == 1) {
                    if (strlen(content_buffer) == 0) {
                        send(fd, "OK_200 EMPTY_CHECKPOINT\n", 24, 0);
                        write_log("INFO", "VIEWCHECKPOINT: Empty checkpoint '%s' for file %s viewed by user %s", checkpoint_tag, fname, username);
                    } else {
                        send(fd, "OK_200 CHECKPOINT_CONTENT\n", 26, 0);
                        
                        // Send content in chunks
                        size_t content_len = strlen(content_buffer);
                        size_t sent = 0;
                        const size_t chunk_size = 1024;
                        
                        while (sent < content_len) {
                            size_t to_send = (content_len - sent > chunk_size) ? chunk_size : (content_len - sent);
                            if (send(fd, content_buffer + sent, to_send, 0) == -1) {
                                write_log("ERROR", "Failed to send checkpoint content for %s to user %s", fname, username);
                                break;
                            }
                            sent += to_send;
                        }
                        
                        send(fd, "\nEND_OF_CHECKPOINT\n", 19, 0);
                        write_log("INFO", "VIEWCHECKPOINT: Checkpoint '%s' for file %s (%zu bytes) viewed by user %s", 
                                 checkpoint_tag, fname, content_len, username);
                    }
                    printf("[SERVER %d] VIEWCHECKPOINT: Checkpoint '%s' for file %s viewed by %s\n", 
                           ctx->server_port, checkpoint_tag, fname, username);
                } else {
                    send(fd, "ERR_404 Checkpoint not found\n", 29, 0);
                    write_log("WARN", "VIEWCHECKPOINT failed: Checkpoint '%s' not found for file %s", checkpoint_tag, fname);
                }
            } else {
                send(fd, "ERR_400 Invalid format. Use: VIEWCHECKPOINT <filename> <tag>\n", 62, 0);
            }
        }

        // REVERT command
        else if (matched >= 1 && strcmp(cmd, "REVERT") == 0 && matched >= 3) {
            char checkpoint_tag[256];
            if (sscanf(buf, "REVERT %255s %255s", fname, checkpoint_tag) == 2) {
                // Check if file is currently being edited
                int file_locked = 0;
                pthread_mutex_lock(&lock_mutex);
                sentence_lock_t *curr = sentence_locks;
                while (curr) {
                    if (strcmp(curr->filename, fname) == 0) {
                        file_locked = 1;
                        break;
                    }
                    curr = curr->next;
                }
                pthread_mutex_unlock(&lock_mutex);
                
                if (file_locked) {
                    send(fd, "ERR_409 Cannot revert: file is currently being edited\n", 54, 0);
                    write_log("WARN", "REVERT blocked: file %s is currently being edited", fname);
                    continue;
                }
                
                // Check if file exists
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname);
                FILE* check_file = fopen(filepath, "r");
                if (!check_file) {
                    send(fd, "ERR_404 File not found\n", 23, 0);
                    write_log("ERROR", "REVERT failed: File %s not found", fname);
                    continue;
                }
                fclose(check_file);
                
                int result = revert_to_checkpoint(fname, checkpoint_tag, ctx->server_port, username);
                
                if (result == 1) {
                    send(fd, "OK_200 REVERT COMPLETED\n", 24, 0);
                    write_log("INFO", "REVERT successful: File %s reverted to checkpoint '%s' by user %s", fname, checkpoint_tag, username);
                    printf("[SERVER %d] REVERT: File %s reverted to checkpoint '%s' by %s\n", 
                           ctx->server_port, fname, checkpoint_tag, username);
                } else if (result == 0) {
                    send(fd, "ERR_404 Checkpoint not found\n", 29, 0);
                    write_log("WARN", "REVERT failed: Checkpoint '%s' not found for file %s", checkpoint_tag, fname);
                } else {
                    send(fd, "ERR_500 REVERT operation failed\n", 32, 0);
                    write_log("ERROR", "REVERT operation failed for file %s", fname);
                }
            } else {
                send(fd, "ERR_400 Invalid format. Use: REVERT <filename> <tag>\n", 54, 0);
            }
        }

        // LISTCHECKPOINTS command
        else if (matched >= 1 && strcmp(cmd, "LISTCHECKPOINTS") == 0 && matched >= 2) {
            char list_buffer[8192];
            int result = list_checkpoints(fname, ctx->server_port, list_buffer, sizeof(list_buffer));
            
            send(fd, "OK_200 CHECKPOINT_LIST\n", 23, 0);
            
            // Send the list content
            size_t list_len = strlen(list_buffer);
            if (list_len > 0) {
                size_t sent = 0;
                const size_t chunk_size = 1024;
                
                while (sent < list_len) {
                    size_t to_send = (list_len - sent > chunk_size) ? chunk_size : (list_len - sent);
                    if (send(fd, list_buffer + sent, to_send, 0) == -1) {
                        write_log("ERROR", "Failed to send checkpoint list for %s to user %s", fname, username);
                        break;
                    }
                    sent += to_send;
                }
            }
            
            send(fd, "\nEND_OF_LIST\n", 13, 0);
            
            write_log("INFO", "LISTCHECKPOINTS: Listed %d checkpoints for file %s to user %s", result, fname, username);
            printf("[SERVER %d] LISTCHECKPOINTS: Listed checkpoints for file %s to %s\n", 
                   ctx->server_port, fname, username);
        }

        // DELETE
        else if (matched >= 1 && strcmp(cmd, "DELETE") == 0 && matched >= 2) {
            char filepath[512];
            snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname);
            if (remove(filepath) == 0) {
                remove_metadata_entry(meta_dir, fname);
                send(fd, "OK_200 DELETED\n", 15, 0);
                printf("[SERVER %d] Deleted: %s\n", ctx->server_port, fname);
            } else {
                send(fd, "ERR_404\n", 8, 0);
            }
        }

        // EXIT
        else if (matched >= 1 && strcmp(cmd, "EXIT") == 0) {
            send(fd, "OK_200 BYE\n", 11, 0);
            printf("[SERVER %d] Client %s disconnected\n", ctx->server_port, username);
            break;
        }

        // REQUESTACCESS command
        else if (matched >= 1 && strcmp(cmd, "REQUESTACCESS") == 0 && matched >= 3) {
            char permission[8];
            if (sscanf(buf, "REQUESTACCESS %255s %7s", fname, permission) == 2) {
                // Validate permission
                if (strcmp(permission, "-R") != 0 && strcmp(permission, "-W") != 0) {
                    send(fd, "ERR_400 Invalid permission. Use -R for read or -W for write\n", 61, 0);
                    continue;
                }
                
                // Check if file exists
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", files_dir, fname);
                FILE* check_file = fopen(filepath, "r");
                if (!check_file) {
                    send(fd, "ERR_404 File not found\n", 23, 0);
                    write_log("ERROR", "REQUESTACCESS failed: File %s not found", fname);
                    continue;
                }
                fclose(check_file);
                
                // Check if user is the owner
                if (check_file_owner(fname, username, ctx->server_port)) {
                    send(fd, "ERR_400 You already own this file\n", 34, 0);
                    write_log("WARN", "REQUESTACCESS failed: %s already owns file %s", username, fname);
                    continue;
                }
                
                // Check if user already has access
                int has_access = 0;
                for (int i = 0; i < file_count; i++) {
                    if (strcmp(file_table[i].filename, fname) == 0) {
                        for (int j = 0; j < file_table[i].acl_count; j++) {
                            if (strcmp(file_table[i].acl[j].username, username) == 0) {
                                int required_perm = (strcmp(permission, "-W") == 0) ? PERM_WRITE : PERM_READ;
                                if (file_table[i].acl[j].permission >= required_perm) {
                                    has_access = 1;
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }
                
                if (has_access) {
                    send(fd, "ERR_409 You already have the requested access to this file\n", 59, 0);
                    write_log("WARN", "REQUESTACCESS failed: %s already has access to file %s", username, fname);
                    continue;
                }
                
                int result = request_file_access(fname, username, permission, ctx->server_port);
                
                if (result == 1) {
                    send(fd, "OK_200 ACCESS REQUEST SUBMITTED\n", 32, 0);
                    write_log("INFO", "Access request submitted: %s requesting %s access to %s", username, permission, fname);
                    printf("[SERVER %d] Access request: %s requesting %s access to %s\n", 
                           ctx->server_port, username, permission, fname);
                } else if (result == -2) {
                    send(fd, "ERR_409 Access request already exists\n", 38, 0);
                    write_log("WARN", "REQUESTACCESS failed: Request already exists for %s on file %s", username, fname);
                } else {
                    send(fd, "ERR_500 Failed to submit access request\n", 40, 0);
                    write_log("ERROR", "REQUESTACCESS failed for %s on file %s", username, fname);
                }
            } else {
                send(fd, "ERR_400 Invalid format. Use: REQUESTACCESS <filename> <-R/-W>\n", 63, 0);
            }
        }

        // VIEWREQUESTS command
        else if (matched >= 1 && strcmp(cmd, "VIEWREQUESTS") == 0) {
            char target_file[256] = "";
            if (matched >= 2) {
                strcpy(target_file, fname);
                
                // Check if user owns the specified file
                if (!check_file_owner(target_file, username, ctx->server_port)) {
                    send(fd, "ERR_403 You can only view requests for files you own\n", 53, 0);
                    write_log("WARN", "VIEWREQUESTS failed: %s does not own file %s", username, target_file);
                    continue;
                }
            }
            
            char list_buffer[8192];
            int result = list_access_requests(strlen(target_file) > 0 ? target_file : NULL, 
                                            username, ctx->server_port, list_buffer, sizeof(list_buffer));
            
            send(fd, "OK_200 ACCESS_REQUESTS\n", 23, 0);
            
            // Send the list content
            size_t list_len = strlen(list_buffer);
            if (list_len > 0) {
                size_t sent = 0;
                const size_t chunk_size = 1024;
                
                while (sent < list_len) {
                    size_t to_send = (list_len - sent > chunk_size) ? chunk_size : (list_len - sent);
                    if (send(fd, list_buffer + sent, to_send, 0) == -1) {
                        write_log("ERROR", "Failed to send access requests list to user %s", username);
                        break;
                    }
                    sent += to_send;
                }
            }
            
            send(fd, "\nEND_OF_REQUESTS\n", 17, 0);
            
            write_log("INFO", "VIEWREQUESTS: Listed access requests for user %s", username);
            printf("[SERVER %d] VIEWREQUESTS: Listed access requests for %s\n", 
                   ctx->server_port, username);
        }

        // APPROVEREQUEST command
        // Replace the APPROVEREQUEST command section (around line 2100) with this corrected version:

        // APPROVEREQUEST command - FIXED VERSION
        else if (matched >= 1 && strcmp(cmd, "APPROVEREQUEST") == 0) {
            char requester_user[128], permission[8];
            // Use a more flexible parsing approach
            int parse_result = sscanf(buf, "%*s %255s %127s %7s", fname, requester_user, permission);
            
            if (parse_result == 3) {
                // Validate permission
                if (strcmp(permission, "-R") != 0 && strcmp(permission, "-W") != 0) {
                    send(fd, "ERR_400 Invalid permission. Use -R for read or -W for write\n", 61, 0);
                    continue;
                }
                
                // Check if user owns the file
                if (!check_file_owner(fname, username, ctx->server_port)) {
                    send(fd, "ERR_403 You can only approve requests for files you own\n", 56, 0);
                    write_log("WARN", "APPROVEREQUEST failed: %s does not own file %s", username, fname);
                    continue;
                }
                
                int result = approve_access_request(fname, requester_user, permission, username, ctx->server_port);
                
                if (result == 1) {
                    send(fd, "OK_200 ACCESS REQUEST APPROVED\n", 31, 0);
                    write_log("INFO", "Access request approved: %s granted %s access to %s by owner %s", 
                             requester_user, permission, fname, username);
                    printf("[SERVER %d] Access approved: %s granted %s access to %s by %s\n", 
                           ctx->server_port, requester_user, permission, fname, username);
                } else if (result == 0) {
                    send(fd, "ERR_404 Access request not found\n", 33, 0);
                    write_log("WARN", "APPROVEREQUEST failed: Request not found for %s on file %s", requester_user, fname);
                } else {
                    send(fd, "ERR_500 Failed to approve access request\n", 41, 0);
                    write_log("ERROR", "APPROVEREQUEST failed for %s on file %s", requester_user, fname);
                }
            } else {
                send(fd, "ERR_400 Invalid format. Use: APPROVEREQUEST <filename> <username> <-R/-W>\n", 75, 0);
            }
        }

        // DENYREQUEST command - FIXED VERSION
        else if (matched >= 1 && strcmp(cmd, "DENYREQUEST") == 0) {
            char requester_user[128];
            // Use a more flexible parsing approach
            int parse_result = sscanf(buf, "%*s %255s %127s", fname, requester_user);
            
            if (parse_result == 2) {
                // Check if user owns the file
                if (!check_file_owner(fname, username, ctx->server_port)) {
                    send(fd, "ERR_403 You can only deny requests for files you own\n", 53, 0);
                    write_log("WARN", "DENYREQUEST failed: %s does not own file %s", username, fname);
                    continue;
                }
                
                int result = deny_access_request(fname, requester_user, username, ctx->server_port);
                
                if (result == 1) {
                    send(fd, "OK_200 ACCESS REQUEST DENIED\n", 29, 0);
                    write_log("INFO", "Access request denied: %s denied access to %s by owner %s", 
                             requester_user, fname, username);
                    printf("[SERVER %d] Access denied: %s denied access to %s by %s\n", 
                           ctx->server_port, requester_user, fname, username);
                } else if (result == 0) {
                    send(fd, "ERR_404 Access request not found\n", 33, 0);
                    write_log("WARN", "DENYREQUEST failed: Request not found for %s on file %s", requester_user, fname);
                } else {
                    send(fd, "ERR_500 Failed to deny access request\n", 38, 0);
                    write_log("ERROR", "DENYREQUEST failed for %s on file %s", requester_user, fname);
                }
            } else {
                send(fd, "ERR_400 Invalid format. Use: DENYREQUEST <filename> <username>\n", 64, 0);
            }
        }

        // UNKNOWN
        else {
            send(fd, "ERR_400 UNKNOWN_CMD\n", 20, 0);
        }
    }

    remove_client_locks(fd);
    close(fd);
    remove_client_fd(fd);
    printf("[SERVER %d] Closed connection from %s:%d (%s)\n",
           ctx->server_port, client_ip, client_port, username);
    free(ctx);
    return NULL;
}


static int is_sentence_locked(const char* filename, int sentence_num, int client_fd) {
    pthread_mutex_lock(&lock_mutex);
    sentence_lock_t *curr = sentence_locks;
    while (curr) {
        if (strcmp(curr->filename, filename) == 0 && 
            curr->sentence_num == sentence_num && 
            curr->client_fd != client_fd) {
            pthread_mutex_unlock(&lock_mutex);
            return 1; // Locked by another client
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&lock_mutex);
    return 0; // Not locked
}

static void add_sentence_lock(const char* filename, int sentence_num, int client_fd) {
    pthread_mutex_lock(&lock_mutex);
    sentence_lock_t *new_lock = malloc(sizeof(sentence_lock_t));
    if (!new_lock) {
        perror("malloc failed in add_sentence_lock");
        pthread_mutex_unlock(&lock_mutex);
        return;
    }
    strcpy(new_lock->filename, filename);
    new_lock->sentence_num = sentence_num;
    new_lock->client_fd = client_fd;
    new_lock->next = sentence_locks;
    sentence_locks = new_lock;
    pthread_mutex_unlock(&lock_mutex);
}

static void remove_sentence_lock(const char* filename, int sentence_num, int client_fd) {
    pthread_mutex_lock(&lock_mutex);
    sentence_lock_t *prev = NULL, *curr = sentence_locks;
    while (curr) {
        if (strcmp(curr->filename, filename) == 0 && 
            curr->sentence_num == sentence_num && 
            curr->client_fd == client_fd) {
            if (prev) {
                prev->next = curr->next;
            } else {
                sentence_locks = curr->next;
            }
            free(curr);
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&lock_mutex);
}

static void remove_client_locks(int client_fd) {
    pthread_mutex_lock(&lock_mutex);
    sentence_lock_t *prev = NULL, *curr = sentence_locks;
    while (curr) {
        if (curr->client_fd == client_fd) {
            sentence_lock_t *to_remove = curr;
            if (prev) {
                prev->next = curr->next;
            } else {
                sentence_locks = curr->next;
            }
            curr = curr->next;
            free(to_remove);
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    pthread_mutex_unlock(&lock_mutex);
}

static int get_client_write_info(int client_fd, char* filename, int* sentence_num) {
    pthread_mutex_lock(&lock_mutex);
    sentence_lock_t *curr = sentence_locks;
    while (curr) {
        if (curr->client_fd == client_fd) {
            strcpy(filename, curr->filename);
            *sentence_num = curr->sentence_num;
            pthread_mutex_unlock(&lock_mutex);
            return 1; // Found
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&lock_mutex);
    return 0; // Not found
}

static int create_file_backup(const char* filename, int server_port, const char* username) {
    char files_dir[256], versions_dir[256];
    snprintf(files_dir, sizeof(files_dir), "data/ss_%d/files", server_port);
    snprintf(versions_dir, sizeof(versions_dir), "data/ss_%d/versions", server_port);
    
    char source_path[512];
    snprintf(source_path, sizeof(source_path), "%s/%s", files_dir, filename);
    
    FILE* source = fopen(source_path, "r");
    if (!source) {
        return 0; 
    }
    
    char content[8192] = {0};
    size_t bytes_read = fread(content, 1, sizeof(content) - 1, source);
    fclose(source);
    content[bytes_read] = '\0';
    
    time_t now = time(NULL);
    char backup_filename[512];
    snprintf(backup_filename, sizeof(backup_filename), "%s_%ld.bak", filename, now);
    
    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path), "%s/%s", versions_dir, backup_filename);
    
    FILE* backup = fopen(backup_path, "w");
    if (!backup) {
        write_log("ERROR", "Failed to create backup for %s", filename);
        return -1;
    }
    
    fprintf(backup, "%s", content);
    fclose(backup);
    
    char undo_dir[256];
    snprintf(undo_dir, sizeof(undo_dir), "data/ss_%d/undo", server_port);
    char undo_meta_path[512];
    snprintf(undo_meta_path, sizeof(undo_meta_path), "%s/%s.undo", undo_dir, filename);
    
    FILE* undo_meta = fopen(undo_meta_path, "a");
    if (undo_meta) {
        fprintf(undo_meta, "%ld|%s|%s\n", now, backup_filename, username);
        fclose(undo_meta);
    }
    
    write_log("INFO", "Created backup %s for file %s by user %s", backup_filename, filename, username);
    return 1;
}

static int perform_undo(const char* filename, int server_port, const char* username) {
    char undo_dir[256], versions_dir[256], files_dir[256];
    snprintf(undo_dir, sizeof(undo_dir), "data/ss_%d/undo", server_port);
    snprintf(versions_dir, sizeof(versions_dir), "data/ss_%d/versions", server_port);
    snprintf(files_dir, sizeof(files_dir), "data/ss_%d/files", server_port);
    
    char undo_meta_path[512];
    snprintf(undo_meta_path, sizeof(undo_meta_path), "%s/%s.undo", undo_dir, filename);
    
    FILE* undo_meta = fopen(undo_meta_path, "r");
    if (!undo_meta) {
        // Log undo history failure - ADD this missing functionality
        write_log("INFO", "No undo history found for file %s", filename);
        return 0; // No undo history available - FIX: Don't close NULL pointer
    }
    
    typedef struct backup_entry {
        long timestamp;
        char backup_name[256];
        char user[128];
        int used; 
    } backup_entry_t;
    
    backup_entry_t backups[1000];
    int backup_count = 0;
    
    char line[1024];
    while (fgets(line, sizeof(line), undo_meta) && backup_count < 1000) {
        char backup_name[256], user[128];
        long timestamp;
        int used = 0;
        
        // Try to parse with used flag (newer format)
        if (sscanf(line, "%ld|%255[^|]|%127[^|]|%d", &timestamp, backup_name, user, &used) >= 3) {
            backups[backup_count].timestamp = timestamp;
            strcpy(backups[backup_count].backup_name, backup_name);
            strcpy(backups[backup_count].user, user);
            backups[backup_count].used = used;
            backup_count++;
        }
    }
    fclose(undo_meta); // Now it's safe to close
    
    if (backup_count == 0) {
        write_log("INFO", "No backup entries found for file %s", filename);
        return 0; 
    }
    
    // Sort backups by timestamp (newest first)
    for (int i = 0; i < backup_count - 1; i++) {
        for (int j = i + 1; j < backup_count; j++) {
            if (backups[i].timestamp < backups[j].timestamp) {
                backup_entry_t temp = backups[i];
                backups[i] = backups[j];
                backups[j] = temp;
            }
        }
    }
    
    // Find the next unused backup (going backwards in time)
    int target_backup_idx = -1;
    for (int i = 0; i < backup_count; i++) {
        if (!backups[i].used) {
            target_backup_idx = i;
            break;
        }
    }
    
    if (target_backup_idx == -1) {
        write_log("INFO", "No more unused backups available for file %s", filename);
        return 0; // No more unused backups available
    }
    
    backup_entry_t* target_backup = &backups[target_backup_idx];
    
    // Restore from the target backup
    char backup_path[512], current_path[512];
    snprintf(backup_path, sizeof(backup_path), "%s/%s", versions_dir, target_backup->backup_name);
    snprintf(current_path, sizeof(current_path), "%s/%s", files_dir, filename);
    
    FILE* backup_file = fopen(backup_path, "r");
    if (!backup_file) {
        write_log("ERROR", "Backup file %s not found during undo", target_backup->backup_name);
        return -1;
    }
    
    char backup_content[8192] = {0};
    size_t backup_bytes = fread(backup_content, 1, sizeof(backup_content) - 1, backup_file);
    fclose(backup_file);
    backup_content[backup_bytes] = '\0';
    
    FILE* current_file = fopen(current_path, "w");
    if (!current_file) {
        write_log("ERROR", "Failed to open current file %s for undo", filename);
        return -1;
    }
    
    fprintf(current_file, "%s", backup_content);
    fclose(current_file);
    
    // Mark this backup as used instead of deleting it
    target_backup->used = 1;
    
    // Rewrite the undo metadata file with updated used flags
    FILE* new_undo_meta = fopen(undo_meta_path, "w");
    if (new_undo_meta) {
        for (int i = 0; i < backup_count; i++) {
            fprintf(new_undo_meta, "%ld|%s|%s|%d\n", 
                   backups[i].timestamp, 
                   backups[i].backup_name, 
                   backups[i].user,
                   backups[i].used);
        }
        fclose(new_undo_meta);
    }
    
    write_log("INFO", "UNDO completed for %s by %s (restored from %s by %s)", 
             filename, username, target_backup->backup_name, target_backup->user);
    
    // Check if there are more unused backups available
    int remaining_undos = 0;
    for (int i = 0; i < backup_count; i++) {
        if (!backups[i].used) {
            remaining_undos++;
        }
    }
    
    if (remaining_undos > 0) {
        write_log("INFO", "File %s has %d more undo operations available", filename, remaining_undos);
    } else {
        write_log("INFO", "File %s has reached the beginning of its history", filename);
    }
    
    return 1; // Success
}

// Add checkpoint helper functions at the end of the file (before the client list functions)

static int create_checkpoint(const char* filename, const char* checkpoint_tag, int server_port, const char* username) {
    char files_dir[256], checkpoints_dir[256];
    snprintf(files_dir, sizeof(files_dir), "data/ss_%d/files", server_port);
    snprintf(checkpoints_dir, sizeof(checkpoints_dir), "data/ss_%d/checkpoints", server_port);
    
    // Create checkpoints directory if it doesn't exist
    mkdir(checkpoints_dir, 0755);
    
    char source_path[512];
    snprintf(source_path, sizeof(source_path), "%s/%s", files_dir, filename);
    
    FILE* source = fopen(source_path, "r");
    if (!source) {
        write_log("ERROR", "CHECKPOINT failed: Source file %s not found", filename);
        return 0; // File not found
    }
    
    char content[8192] = {0};
    size_t bytes_read = fread(content, 1, sizeof(content) - 1, source);
    fclose(source);
    content[bytes_read] = '\0';
    
    // Create checkpoint filename: filename_tag.checkpoint
    char checkpoint_filename[512];
    snprintf(checkpoint_filename, sizeof(checkpoint_filename), "%s_%s.checkpoint", filename, checkpoint_tag);
    
    char checkpoint_path[512];
    snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/%s", checkpoints_dir, checkpoint_filename);
    
    // Check if checkpoint already exists
    FILE* existing_check = fopen(checkpoint_path, "r");
    if (existing_check) {
        fclose(existing_check);
        write_log("WARN", "CHECKPOINT failed: Checkpoint %s already exists for file %s", checkpoint_tag, filename);
        return -2; // Checkpoint already exists
    }
    
    FILE* checkpoint = fopen(checkpoint_path, "w");
    if (!checkpoint) {
        write_log("ERROR", "Failed to create checkpoint file for %s", filename);
        return -1; // Failed to create checkpoint file
    }
    
    fprintf(checkpoint, "%s", content);
    fclose(checkpoint);
    
    // Update checkpoint metadata
    char checkpoint_meta_dir[256];
    snprintf(checkpoint_meta_dir, sizeof(checkpoint_meta_dir), "data/ss_%d/checkpoint_meta", server_port);
    mkdir(checkpoint_meta_dir, 0755);
    
    char checkpoint_meta_path[512];
    snprintf(checkpoint_meta_path, sizeof(checkpoint_meta_path), "%s/%s.meta", checkpoint_meta_dir, filename);
    
    FILE* meta_file = fopen(checkpoint_meta_path, "a");
    if (meta_file) {
        time_t now = time(NULL);
        fprintf(meta_file, "%ld|%s|%s|%zu\n", now, checkpoint_tag, username, bytes_read);
        fclose(meta_file);
    }
    
    write_log("INFO", "Created checkpoint '%s' for file %s by user %s", checkpoint_tag, filename, username);
    return 1; // Success
}

static int view_checkpoint(const char* filename, const char* checkpoint_tag, int server_port, char* content_buffer, size_t buffer_size) {
    char checkpoints_dir[256];
    snprintf(checkpoints_dir, sizeof(checkpoints_dir), "data/ss_%d/checkpoints", server_port);
    
    char checkpoint_filename[512];
    snprintf(checkpoint_filename, sizeof(checkpoint_filename), "%s_%s.checkpoint", filename, checkpoint_tag);
    
    char checkpoint_path[512];
    snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/%s", checkpoints_dir, checkpoint_filename);
    
    FILE* checkpoint = fopen(checkpoint_path, "r");
    if (!checkpoint) {
        write_log("WARN", "VIEWCHECKPOINT failed: Checkpoint %s not found for file %s", checkpoint_tag, filename);
        return 0; // Checkpoint not found
    }
    
    size_t bytes_read = fread(content_buffer, 1, buffer_size - 1, checkpoint);
    fclose(checkpoint);
    content_buffer[bytes_read] = '\0';
    
    write_log("INFO", "Viewed checkpoint '%s' for file %s (%zu bytes)", checkpoint_tag, filename, bytes_read);
    return 1; // Success
}

static int revert_to_checkpoint(const char* filename, const char* checkpoint_tag, int server_port, const char* username) {
    char files_dir[256], checkpoints_dir[256];
    snprintf(files_dir, sizeof(files_dir), "data/ss_%d/files", server_port);
    snprintf(checkpoints_dir, sizeof(checkpoints_dir), "data/ss_%d/checkpoints", server_port);
    
    char checkpoint_filename[512];
    snprintf(checkpoint_filename, sizeof(checkpoint_filename), "%s_%s.checkpoint", filename, checkpoint_tag);
    
    char checkpoint_path[512];
    snprintf(checkpoint_path, sizeof(checkpoint_path), "%s/%s", checkpoints_dir, checkpoint_filename);
    
    FILE* checkpoint = fopen(checkpoint_path, "r");
    if (!checkpoint) {
        write_log("ERROR", "REVERT failed: Checkpoint %s not found for file %s", checkpoint_tag, filename);
        return 0; // Checkpoint not found
    }
    
    char checkpoint_content[8192] = {0};
    size_t bytes_read = fread(checkpoint_content, 1, sizeof(checkpoint_content) - 1, checkpoint);
    fclose(checkpoint);
    checkpoint_content[bytes_read] = '\0';
    
    // Create backup before reverting
    create_file_backup(filename, server_port, username);
    
    // Write checkpoint content to current file
    char current_path[512];
    snprintf(current_path, sizeof(current_path), "%s/%s", files_dir, filename);
    
    FILE* current_file = fopen(current_path, "w");
    if (!current_file) {
        write_log("ERROR", "REVERT failed: Could not open current file %s for writing", filename);
        return -1; // Failed to open current file
    }
    
    fprintf(current_file, "%s", checkpoint_content);
    fclose(current_file);
    
    // Update metadata
    char meta_dir[256];
    snprintf(meta_dir, sizeof(meta_dir), "data/ss_%d/metadata", server_port);
    update_metadata_entry(meta_dir, filename);
    
    write_log("INFO", "Reverted file %s to checkpoint '%s' by user %s", filename, checkpoint_tag, username);
    return 1; // Success
}

static int list_checkpoints(const char* filename, int server_port, char* list_buffer, size_t buffer_size) {
    char checkpoint_meta_dir[256];
    snprintf(checkpoint_meta_dir, sizeof(checkpoint_meta_dir), "data/ss_%d/checkpoint_meta", server_port);
    
    char checkpoint_meta_path[512];
    snprintf(checkpoint_meta_path, sizeof(checkpoint_meta_path), "%s/%s.meta", checkpoint_meta_dir, filename);
    
    FILE* meta_file = fopen(checkpoint_meta_path, "r");
    if (!meta_file) {
        write_log("INFO", "LISTCHECKPOINTS: No checkpoints found for file %s", filename);
        strncpy(list_buffer, "No checkpoints available", buffer_size - 1);
        list_buffer[buffer_size - 1] = '\0';
        return 0; // No checkpoints found
    }
    
    char line[1024];
    char temp_buffer[8192] = "";
    int checkpoint_count = 0;
    
    strcat(temp_buffer, "Checkpoints for file: ");
    strcat(temp_buffer, filename);
    strcat(temp_buffer, "\n");
    
    while (fgets(line, sizeof(line), meta_file)) {
        long timestamp;
        char tag[256], user[128];
        size_t size;
        
        if (sscanf(line, "%ld|%255[^|]|%127[^|]|%zu", &timestamp, tag, user, &size) == 4) {
            char entry[512];
            struct tm* timeinfo = localtime(&timestamp);
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
            
            snprintf(entry, sizeof(entry), "  Tag: %s | Created: %s | By: %s | Size: %zu bytes\n", 
                    tag, time_str, user, size);
            
            if (strlen(temp_buffer) + strlen(entry) < sizeof(temp_buffer) - 1) {
                strcat(temp_buffer, entry);
                checkpoint_count++;
            }
        }
    }
    fclose(meta_file);
    
    if (checkpoint_count == 0) {
        strcat(temp_buffer, "  No valid checkpoints found\n");
    } else {
        char count_str[64];
        snprintf(count_str, sizeof(count_str), "Total checkpoints: %d\n", checkpoint_count);
        strcat(temp_buffer, count_str);
    }
    
    strncpy(list_buffer, temp_buffer, buffer_size - 1);
    list_buffer[buffer_size - 1] = '\0';
    
    write_log("INFO", "Listed %d checkpoints for file %s", checkpoint_count, filename);
    return checkpoint_count;
}

// Client list functions (stubs for this merged file)
static void add_client_fd(int fd) {
    pthread_mutex_lock(&client_lock);
    client_node_t *node = malloc(sizeof(client_node_t));
    if (!node) { pthread_mutex_unlock(&client_lock); return; }
    node->fd = fd;
    node->next = client_list;
    client_list = node;
    pthread_mutex_unlock(&client_lock);
}

static void remove_client_fd(int fd) {
    pthread_mutex_lock(&client_lock);
    client_node_t *prev = NULL, *curr = client_list;
    while (curr) {
        if (curr->fd == fd) {
            if (prev)
                prev->next = curr->next;
            else
                client_list = curr->next;
            free(curr);
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    pthread_mutex_unlock(&client_lock);
}

static void close_all_clients() {
    pthread_mutex_lock(&client_lock);
    client_node_t *curr = client_list;
    while (curr) {
        shutdown(curr->fd, SHUT_RDWR);
        close(curr->fd);
        curr = curr->next;
    }
    pthread_mutex_unlock(&client_lock);
}

static void free_client_list() {
    pthread_mutex_lock(&client_lock);
    client_node_t *curr = client_list;
    while (curr) {
        client_node_t *tmp = curr;
        curr = curr->next;
        free(tmp);
    }
    client_list = NULL;
    pthread_mutex_unlock(&client_lock);
}

static void update_file_access_time(const char* meta_dir, const char* filename) {
    char access_log_path[512];
    snprintf(access_log_path, sizeof(access_log_path), "%s/.access_log", meta_dir);
    
    FILE* access_file = fopen(access_log_path, "a");
    if (access_file) {
        time_t now = time(NULL);
        fprintf(access_file, "%ld|%s|READ\n", now, filename);
        fclose(access_file);
    }
}

static int check_file_owner(const char* filename, const char* username, int server_port) {
    char meta_dir[256];
    snprintf(meta_dir, sizeof(meta_dir), "data/ss_%d/metadata", server_port);
    
    // Check the file_table for ownership
    for (int i = 0; i < file_count; i++) {
        if (strcmp(file_table[i].filename, filename) == 0) {
            return strcmp(file_table[i].owner_username, username) == 0 ? 1 : 0;
        }
    }
    return 0; // File not found or not owner
}

static int request_file_access(const char* filename, const char* username, const char* permission, int server_port) {
    char requests_dir[256];
    snprintf(requests_dir, sizeof(requests_dir), "data/ss_%d/access_requests", server_port);
    
    // Create requests directory if it doesn't exist
    mkdir(requests_dir, 0755);
    
    char request_file_path[512];
    snprintf(request_file_path, sizeof(request_file_path), "%s/%s.requests", requests_dir, filename);
    
    // Check if request already exists
    FILE* existing_requests = fopen(request_file_path, "r");
    if (existing_requests) {
        char line[512];
        while (fgets(line, sizeof(line), existing_requests)) {
            char existing_user[128], existing_perm[8], status[16];
            long timestamp;
            if (sscanf(line, "%ld|%127[^|]|%7[^|]|%15s", &timestamp, existing_user, existing_perm, status) == 4) {
                if (strcmp(existing_user, username) == 0 && strcmp(existing_perm, permission) == 0 && strcmp(status, "PENDING") == 0) {
                    fclose(existing_requests);
                    write_log("WARN", "Access request already exists: %s requesting %s access to %s", username, permission, filename);
                    return -2; // Request already exists
                }
            }
        }
        fclose(existing_requests);
    }
    
    // Add new request
    FILE* request_file = fopen(request_file_path, "a");
    if (!request_file) {
        write_log("ERROR", "Failed to create access request file for %s", filename);
        return -1; // Failed to create request
    }
    
    time_t now = time(NULL);
    fprintf(request_file, "%ld|%s|%s|PENDING\n", now, username, permission);
    fclose(request_file);
    
    write_log("INFO", "Access request created: %s requesting %s access to %s", username, permission, filename);
    return 1; // Success
}

static int list_access_requests(const char* filename, const char* owner_username, int server_port, char* list_buffer, size_t buffer_size) {
    char requests_dir[256];
    snprintf(requests_dir, sizeof(requests_dir), "data/ss_%d/access_requests", server_port);
    
    char temp_buffer[8192] = "";
    int total_requests = 0;
    
    if (filename && strlen(filename) > 0) {
        // List requests for specific file
        char request_file_path[512];
        snprintf(request_file_path, sizeof(request_file_path), "%s/%s.requests", requests_dir, filename);
        
        FILE* request_file = fopen(request_file_path, "r");
        if (!request_file) {
            strcat(temp_buffer, "No access requests found for this file.\n");
        } else {
            snprintf(temp_buffer + strlen(temp_buffer), sizeof(temp_buffer) - strlen(temp_buffer), 
                    "Access requests for file: %s\n", filename);
            
            char line[512];
            while (fgets(line, sizeof(line), request_file)) {
                long timestamp;
                char user[128], perm[8], status[16];
                if (sscanf(line, "%ld|%127[^|]|%7[^|]|%15s", &timestamp, user, perm, status) == 4) {
                    if (strcmp(status, "PENDING") == 0) {
                        struct tm* timeinfo = localtime(&timestamp);
                        char time_str[64];
                        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
                        
                        char entry[256];
                        snprintf(entry, sizeof(entry), "  User: %s | Permission: %s | Requested: %s\n", 
                                user, perm, time_str);
                        
                        if (strlen(temp_buffer) + strlen(entry) < sizeof(temp_buffer) - 1) {
                            strcat(temp_buffer, entry);
                            total_requests++;
                        }
                    }
                }
            }
            fclose(request_file);
        }
    } else {
        // List all requests for files owned by this user
        strcat(temp_buffer, "All pending access requests for your files:\n");
        
        for (int i = 0; i < file_count; i++) {
            if (strcmp(file_table[i].owner_username, owner_username) == 0) {
                char request_file_path[512];
                snprintf(request_file_path, sizeof(request_file_path), "%s/%s.requests", requests_dir, file_table[i].filename);
                
                FILE* request_file = fopen(request_file_path, "r");
                if (request_file) {
                    char line[512];
                    int file_has_requests = 0;
                    
                    while (fgets(line, sizeof(line), request_file)) {
                        long timestamp;
                        char user[128], perm[8], status[16];
                        if (sscanf(line, "%ld|%127[^|]|%7[^|]|%15s", &timestamp, user, perm, status) == 4) {
                            if (strcmp(status, "PENDING") == 0) {
                                if (!file_has_requests) {
                                    char file_header[256];
                                    snprintf(file_header, sizeof(file_header), "\nFile: %s\n", file_table[i].filename);
                                    strcat(temp_buffer, file_header);
                                    file_has_requests = 1;
                                }
                                
                                struct tm* timeinfo = localtime(&timestamp);
                                char time_str[64];
                                strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", timeinfo);
                                
                                char entry[256];
                                snprintf(entry, sizeof(entry), "  User: %s | Permission: %s | Requested: %s\n", 
                                        user, perm, time_str);
                                
                                if (strlen(temp_buffer) + strlen(entry) < sizeof(temp_buffer) - 1) {
                                    strcat(temp_buffer, entry);
                                    total_requests++;
                                }
                            }
                        }
                    }
                    fclose(request_file);
                }
            }
        }
    }
    
    if (total_requests == 0 && strlen(temp_buffer) < 100) {
        strcat(temp_buffer, "No pending access requests found.\n");
    } else {
        char count_str[64];
        snprintf(count_str, sizeof(count_str), "\nTotal pending requests: %d\n", total_requests);
        strcat(temp_buffer, count_str);
    }
    
    strncpy(list_buffer, temp_buffer, buffer_size - 1);
    list_buffer[buffer_size - 1] = '\0';
    
    write_log("INFO", "Listed %d access requests for user %s", total_requests, owner_username);
    return total_requests;
}

static int approve_access_request(const char* filename, const char* requester_username, const char* permission, const char* owner_username, int server_port) {
    char requests_dir[256];
    snprintf(requests_dir, sizeof(requests_dir), "data/ss_%d/access_requests", server_port);
    
    char request_file_path[512];
    snprintf(request_file_path, sizeof(request_file_path), "%s/%s.requests", requests_dir, filename);
    
    FILE* request_file = fopen(request_file_path, "r");
    if (!request_file) {
        write_log("WARN", "No access requests found for file %s", filename);
        return 0; // No requests file
    }
    
    // Read all requests
    char lines[100][512];
    int line_count = 0;
    int request_found = 0;
    
    char line[512];
    while (fgets(line, sizeof(line), request_file) && line_count < 100) {
        long timestamp;
        char user[128], perm[8], status[16];
        if (sscanf(line, "%ld|%127[^|]|%7[^|]|%15s", &timestamp, user, perm, status) == 4) {
            if (strcmp(user, requester_username) == 0 && strcmp(perm, permission) == 0 && strcmp(status, "PENDING") == 0) {
                // Mark this request as approved
                snprintf(lines[line_count], sizeof(lines[line_count]), "%ld|%s|%s|APPROVED\n", timestamp, user, perm);
                request_found = 1;
            } else {
                strcpy(lines[line_count], line);
            }
        } else {
            strcpy(lines[line_count], line);
        }
        line_count++;
    }
    fclose(request_file);
    
    if (!request_found) {
        write_log("WARN", "Access request not found: %s requesting %s access to %s", requester_username, permission, filename);
        return 0; // Request not found
    }
    
    // Rewrite the file
    request_file = fopen(request_file_path, "w");
    if (!request_file) {
        write_log("ERROR", "Failed to update access requests file for %s", filename);
        return -1;
    }
    
    for (int i = 0; i < line_count; i++) {
        fprintf(request_file, "%s", lines[i]);
    }
    fclose(request_file);
    
    // Add the user to ACL
    char meta_dir[256];
    snprintf(meta_dir, sizeof(meta_dir), "data/ss_%d/metadata", server_port);
    
    int perm_flag = (strcmp(permission, "-W") == 0) ? PERM_WRITE : PERM_READ;
    persist_set_acl(meta_dir, filename, requester_username, perm_flag);
    
    write_log("INFO", "Access request approved: %s granted %s access to %s by owner %s", 
             requester_username, permission, filename, owner_username);
    return 1; // Success
}

static int deny_access_request(const char* filename, const char* requester_username, const char* owner_username, int server_port) {
    char requests_dir[256];
    snprintf(requests_dir, sizeof(requests_dir), "data/ss_%d/access_requests", server_port);
    
    char request_file_path[512];
    snprintf(request_file_path, sizeof(request_file_path), "%s/%s.requests", requests_dir, filename);
    
    FILE* request_file = fopen(request_file_path, "r");
    if (!request_file) {
        write_log("WARN", "No access requests found for file %s", filename);
        return 0; // No requests file
    }
    
    // Read all requests
    char lines[100][512];
    int line_count = 0;
    int request_found = 0;
    
    char line[512];
    while (fgets(line, sizeof(line), request_file) && line_count < 100) {
        long timestamp;
        char user[128], perm[8], status[16];
        if (sscanf(line, "%ld|%127[^|]|%7[^|]|%15s", &timestamp, user, perm, status) == 4) {
            if (strcmp(user, requester_username) == 0 && strcmp(status, "PENDING") == 0) {
                // Mark this request as denied
                snprintf(lines[line_count], sizeof(lines[line_count]), "%ld|%s|%s|DENIED\n", timestamp, user, perm);
                request_found = 1;
            } else {
                strcpy(lines[line_count], line);
            }
        } else {
            strcpy(lines[line_count], line);
        }
        line_count++;
    }
    fclose(request_file);
    
    if (!request_found) {
        write_log("WARN", "Access request not found for user %s on file %s", requester_username, filename);
        return 0; // Request not found
    }
    
    // Rewrite the file
    request_file = fopen(request_file_path, "w");
    if (!request_file) {
        write_log("ERROR", "Failed to update access requests file for %s", filename);
        return -1;
    }
    
    for (int i = 0; i < line_count; i++) {
        fprintf(request_file, "%s", lines[i]);
    }
    fclose(request_file);
    
    write_log("INFO", "Access request denied: %s denied access to %s by owner %s", 
             requester_username, filename, owner_username);
    return 1; // Success
}

