#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int log_set; 

const char * log_level_strings [] = {
    "NONE", // 0
    "CRIT", // 1
    "WARN", // 2
    "NOTI", // 3
    " LOG", // 4
    "DEBG" // 5
};

enum {
    LOG_LVL_NONE, // 0
    LOG_LVL_CRITICAL, // 1
    LOG_LVL_WARNING, // 2
    LOG_LVL_NOTICE, // 3
    LOG_LVL_LOG, // 4
    LOG_LVL_DEBUG, // 5
    LOG_LVL_NEVER // 6
};

char * timestamp();

#define LOG_FP stdout

/*#define LL_DEBUG( fmt, ...) LOG( LOG_LVL_DEBUG, fmt, ##__VA_ARGS__)
#define LL_LOG( fmt, ...) LOG( LOG_LVL_LOG, fmt, ##__VA_ARGS__ )
#define LL_NOTICE( fmt, ...) LOG( LOG_LVL_NOTICE, fmt, ##__VA_ARGS__ )
#define LL_WARNING( fmt, ...) LOG( LOG_LVL_WARNING, fmt, ##__VA_ARGS__)
#define LL_CRITICAL( fmt, ...) LOG( LOG_LVL_CRITICAL, fmt, ##__VA_ARGS__)
*/
#define LOG_SHOULD_I( level ) ( level == log_set)

#define DEBUG_PRINTF(level, fmt, ...) do {	\
    if ( LOG_SHOULD_I(level) ) { \
        fprintf(LOG_FP, "[%s]:[%s] %s:%d: " fmt "\n", timestamp(),log_level_strings[level], __FUNCTION__,__LINE__, ##__VA_ARGS__); \
        fflush( LOG_FP ); \
    } \
} while(0)

