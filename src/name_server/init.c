#include "init.h"
#include "logger.h"
#include "storage_manager.h"
#include "search.h"
#include "cache.h"
#include "user_manager.h"
// #include "cache.h" // Add this when you create cache.c

void init_server() {
    write_log("INIT", "Initializing server subsystems...");
    
    // Initialize all components
    
    init_storage_manager();
    init_search_trie();
    init_cache();
    init_user_manager();
    
    write_log("INIT", "All subsystems initialized.");
}