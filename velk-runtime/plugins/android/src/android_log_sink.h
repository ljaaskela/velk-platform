#ifndef VELK_RUNTIME_ANDROID_LOG_SINK_H
#define VELK_RUNTIME_ANDROID_LOG_SINK_H

#include <velk/ext/core_object.h>
#include <velk/interface/intf_log.h>

namespace velk::impl {

/// ILogSink that forwards VELK_LOG output to logcat via __android_log_print.
class AndroidLogSink final : public ext::ObjectCore<AndroidLogSink, ILogSink>
{
public:
    VELK_CLASS_UID(Uid{"7d3c1f08-2c91-4d54-83b9-f9c6b6bd33f1"}, "AndroidLogSink");

    void write(LogLevel level, const char* file, int line, const char* message) override;
};

} // namespace velk::impl

#endif // VELK_RUNTIME_ANDROID_LOG_SINK_H
