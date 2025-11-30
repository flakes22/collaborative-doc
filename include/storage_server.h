#ifndef STORAGE_SERVER_H
#define STORAGE_SERVER_H

#include <sys/stat.h>
#include <time.h>

// Sentence caching structure
typedef struct sentence_cache_entry {
    int sentence_num;
    int start_word_idx;
    int end_word_idx;
    char delimiter;
    long file_modified_time;
} sentence_cache_entry_t;

// Enhanced sentence boundary tracking structure
typedef struct sentence_info {
    int start_word_idx;  // Starting word index of this sentence
    int end_word_idx;    // Ending word index of this sentence  
    char delimiter;      // Sentence ending delimiter (., !, ?)
} sentence_info_t;

// Function declarations
int init_storage_server(int port);
void close_storage_server(void);

#endif
