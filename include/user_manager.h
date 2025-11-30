#ifndef USER_MANAGER_H
#define USER_MANAGER_H

#include <pthread.h>

#define MAX_ACTIVE_USERS 50
#define MAX_USERNAME_LEN 64

/**
 * @brief Initializes the user manager.
 */
void init_user_manager();

/**
 * @brief Adds a user to the global list of active users.
 * This is called by client_handler on login.
 * @param username The user to register.
 */
void user_manager_register(const char* username);

/**
 * @brief Removes a user from the global list.
 * This is called by client_handler on disconnect.
 * @param username The user to deregister.
 */
void user_manager_deregister(const char* username);

/**
 * @brief Fills a buffer with a comma-separated list of all active users.
 * @param out_buffer The buffer to write the list into.
 * @param buffer_size The size of the output buffer.
 * @return The number of bytes written to the buffer.
 */
int user_manager_get_list(char* out_buffer, int buffer_size);

#endif // USER_MANAGER_H