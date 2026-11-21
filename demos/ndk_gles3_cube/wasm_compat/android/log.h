#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { ANDROID_LOG_INFO = 4, ANDROID_LOG_ERROR = 6 };
int __android_log_write(int priority, const char* tag, const char* text);
int __android_log_print(int priority, const char* tag, const char* format, ...)
    __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif
