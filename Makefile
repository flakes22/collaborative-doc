# Compiler and flags
CC = gcc
# Added -pthread for thread support (both servers need it)
CFLAGS = -Wall -Wextra -Iinclude -g -pthread
LDFLAGS = -lm -pthread

# --- Source Directories ---
COMMON_SRC_DIR = src/common
NS_SRC_DIR = src/name_server
SS_SRC_DIR = src/storage_server
CLIENT_SRC_DIR = src/client
TEST_SRC_DIR = src/test

# --- Common Code (Shared) ---
# List of all common .o files
COMMON_OBJS = $(COMMON_SRC_DIR)/socket_utils.o \
              $(COMMON_SRC_DIR)/protocol.o \
              $(COMMON_SRC_DIR)/logger.o

# --- Final Executables (Targets) ---
TARGET_NS = name_server
TARGET_SS = storage_server
TARGET_CLIENT = client

# --- Name Server (Person A) ---
# List of all .c files for the Name Server
NS_SOURCES = $(NS_SRC_DIR)/main.c \
             $(NS_SRC_DIR)/init.c \
             $(NS_SRC_DIR)/storage_manager.c \
             $(NS_SRC_DIR)/client_handler.c \
             $(NS_SRC_DIR)/search.c \
             $(NS_SRC_DIR)/cache.c \
             $(NS_SRC_DIR)/executor.c \
             $(NS_SRC_DIR)/user_manager.c
NS_OBJS = $(NS_SOURCES:.c=.o)

# --- Storage Server (Person B) ---
# List of all .c files for the Storage Server
SS_SOURCES = $(SS_SRC_DIR)/main.c \
             $(SS_SRC_DIR)/init.c \
             $(SS_SRC_DIR)/persistence.c
SS_OBJS = $(SS_SOURCES:.c=.o)

# --- Client (Person B) ---
# List of all .c files for the Client
CLIENT_SOURCES = $(CLIENT_SRC_DIR)/main.c
CLIENT_OBJS = $(CLIENT_SOURCES:.c=.o)

# --- Test Program Targets ---
TEST_CLIENT = test_client
DUMMY_SERVER = dummy_server
FAKE_SS = fake_ss
TEST_CLIENT_LOOP = test_client_loop
TEST_CLIENT_READ = test_client_read
TEST_CLIENT_LOGIN = test_client_login
TEST_CLIENT_ACL = test_client_acl
TEST_CLIENT_EXEC = test_client_exec
TEST_CLIENT_DELETE = test_client_delete
TEST_CLIENT_UNDO = test_client_undo
TEST_CLIENT_INFO = test_client_info
TEST_CLIENT_LIST = test_client_list
TEST_CLIENT_VIEW = test_client_view
TEST_CLIENT_STAMPEDE = test_client_stampede

# =========================================================================
#  BUILD RULES
# =========================================================================

# Default rule: build the 3 final executables
all: $(TARGET_NS) $(TARGET_SS) $(TARGET_CLIENT)

# Rule to build all test programs
test: $(TEST_CLIENT) $(DUMMY_SERVER) $(FAKE_SS) $(TEST_CLIENT_LOOP) $(TEST_CLIENT_READ) $(TEST_CLIENT_LOGIN) $(TEST_CLIENT_ACL) $(TEST_CLIENT_EXEC) $(TEST_CLIENT_DELETE) $(TEST_CLIENT_UNDO) $(TEST_CLIENT_INFO) $(TEST_CLIENT_LIST) $(TEST_CLIENT_VIEW) $(TEST_CLIENT_STAMPEDE)

# --- Main Executable Linking Rules ---

# Rule to build the Name Server
$(TARGET_NS): $(NS_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Rule to build the Storage Server
$(TARGET_SS): $(SS_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# Rule to build the Client
$(TARGET_CLIENT): $(CLIENT_OBJS) $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Test Program Linking Rules ---

$(TEST_CLIENT): $(TEST_SRC_DIR)/test_client.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(DUMMY_SERVER): $(TEST_SRC_DIR)/dummy_server.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(FAKE_SS): $(TEST_SRC_DIR)/fake_ss.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_LOOP): $(TEST_SRC_DIR)/test_client_loop.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_READ): $(TEST_SRC_DIR)/test_client_read.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_LOGIN): $(TEST_SRC_DIR)/test_client_login.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_ACL): $(TEST_SRC_DIR)/test_client_acl.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_EXEC): $(TEST_SRC_DIR)/test_client_exec.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_DELETE): $(TEST_SRC_DIR)/test_client_delete.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_UNDO): $(TEST_SRC_DIR)/test_client_undo.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_INFO): $(TEST_SRC_DIR)/test_client_info.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_LIST): $(TEST_SRC_DIR)/test_client_list.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_CLIENT_VIEW): $(TEST_SRC_DIR)/test_client_view.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	
$(TEST_CLIENT_STAMPEDE): $(TEST_SRC_DIR)/test_client_stampede.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

# --- Generic Compilation Rules (.c -> .o) ---
# These rules tell 'make' how to build a .o file
# from a .c file for each of our source directories.

$(COMMON_SRC_DIR)/%.o: $(COMMON_SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(NS_SRC_DIR)/%.o: $(NS_SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(SS_SRC_DIR)/%.o: $(SS_SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(CLIENT_SRC_DIR)/%.o: $(CLIENT_SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# --- Clean Rule ---
clean:
	rm -f $(COMMON_OBJS) $(NS_OBJS) $(SS_OBJS) $(CLIENT_OBJS)
	rm -f $(TARGET_NS) $(TARGET_SS) $(TARGET_CLIENT)
	rm -f $(TEST_CLIENT) $(DUMMY_SERVER) $(FAKE_SS) $(TEST_CLIENT_LOOP) $(TEST_CLIENT_READ) $(TEST_CLIENT_LOGIN) $(TEST_CLIENT_ACL) $(TEST_CLIENT_EXEC) $(TEST_CLIENT_DELETE) $(TEST_CLIENT_UNDO) $(TEST_CLIENT_INFO) $(TEST_CLIENT_LIST) $(TEST_CLIENT_VIEW) $(TEST_CLIENT_STAMPEDE)
	rm -rf logs data