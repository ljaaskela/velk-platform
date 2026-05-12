// Android entry-point shim for the simple sample. The actual sample body
// lives in main.cpp's `velk_simple::run_app`; this file exists solely so
// NativeActivity finds an `android_main` symbol in libvelk_ui_simple.so.

#include "run.h"

#include <velk-runtime/plugins/android/native_activity_main.h>

extern "C" void android_main(struct android_app* app)
{
    velk::android_native_activity_run(app, []{
        return velk_simple::run_app(0, nullptr);
    });
}
