#ifndef TEST_LOG_H
#define TEST_LOG_H

/* Appends one formatted line to a log file. Opens and closes the file on every call,
   so the log survives a crash. Creates ux0:data/umdplayer if needed. */
void test_log_open(const char *path);
void log_line(const char *fmt, ...);

#endif
