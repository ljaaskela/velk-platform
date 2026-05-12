#include "android_log_sink.h"

#include <android/log.h>

namespace velk::impl {

void AndroidLogSink::write(LogLevel level, const char* file, int line, const char* message)
{
    int prio = ANDROID_LOG_INFO;
    switch (level) {
        case LogLevel::Debug:   prio = ANDROID_LOG_DEBUG; break;
        case LogLevel::Info:    prio = ANDROID_LOG_INFO;  break;
        case LogLevel::Warning: prio = ANDROID_LOG_WARN;  break;
        case LogLevel::Error:   prio = ANDROID_LOG_ERROR; break;
    }
    __android_log_print(prio, "velk", "%s:%d: %s", file ? file : "?", line, message ? message : "");
}

} // namespace velk::impl
