#include "user_manager.h"
#include "logger.h"
#include <string.h>

static char active_users[MAX_ACTIVE_USERS][MAX_USERNAME_LEN];
static int user_count = 0;
static pthread_mutex_t user_mutex;

void init_user_manager() {
    pthread_mutex_init(&user_mutex, NULL);
    user_count = 0;
    memset(active_users, 0, sizeof(active_users));
    write_log("INIT", "User Manager initialized.");
}

void user_manager_register(const char* username) {
    pthread_mutex_lock(&user_mutex);

    if (user_count >= MAX_ACTIVE_USERS) {
        write_log("ERROR", "[USER_MGR] Cannot register user '%s': List is full.", username);
        pthread_mutex_unlock(&user_mutex);
        return;
    }

    // Check if user is already in the list (e.g., duplicate login)
    for (int i = 0; i < user_count; i++) {
        if (strcmp(active_users[i], username) == 0) {
            // User is already here, no action needed.
            pthread_mutex_unlock(&user_mutex);
            return;
        }
    }

    // Add new user
    strncpy(active_users[user_count], username, MAX_USERNAME_LEN - 1);
    user_count++;
    write_log("USER_MGR", "User '%s' registered. Total active users: %d", username, user_count);
    
    pthread_mutex_unlock(&user_mutex);
}

void user_manager_deregister(const char* username) {
    pthread_mutex_lock(&user_mutex);

    int found_index = -1;
    for (int i = 0; i < user_count; i++) {
        if (strcmp(active_users[i], username) == 0) {
            found_index = i;
            break;
        }
    }

    if (found_index != -1) {
        // Remove user by swapping with the last one
        int last_index = user_count - 1;
        strncpy(active_users[found_index], active_users[last_index], MAX_USERNAME_LEN - 1);
        memset(active_users[last_index], 0, MAX_USERNAME_LEN); // Clear last slot
        user_count--;
        write_log("USER_MGR", "User '%s' deregistered. Total active users: %d", username, user_count);
    }
    
    pthread_mutex_unlock(&user_mutex);
}

int user_manager_get_list(char* out_buffer, int buffer_size) {
    pthread_mutex_lock(&user_mutex);
    
    memset(out_buffer, 0, buffer_size);
    int offset = 0; // Current position in the buffer

    for (int i = 0; i < user_count; i++) {
        // snprintf returns the number of chars *that would have been written*
        int chars_written = snprintf(out_buffer + offset, buffer_size - offset, "%s\n", active_users[i]);
        
        if (offset + chars_written >= buffer_size) {
            write_log("ERROR", "[USER_MGR] User list buffer too small.");
            break; // Stop to prevent overflow
        }
        offset += chars_written;
    }

    pthread_mutex_unlock(&user_mutex);
    return offset; // Total bytes written
}