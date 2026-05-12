#ifndef VELK_RUNTIME_ANDROID_NATIVE_ACTIVITY_MAIN_H
#define VELK_RUNTIME_ANDROID_NATIVE_ACTIVITY_MAIN_H

struct android_app;

#if defined(__GNUC__) || defined(__clang__)
#define VELK_RUNTIME_ANDROID_API __attribute__((visibility("default")))
#else
#define VELK_RUNTIME_ANDROID_API
#endif

namespace velk {

class Application;

/// Entry-point helper for NativeActivity-based sample apps.
///
/// Stores the android_app + AAssetManager pointers into the velk-runtime-android
/// plugin's static state (so the plugin's initialize() and lifecycle hooks see
/// them), installs the APP_CMD_* command handler that pumps ANativeWindow
/// lifecycle into the active IWindow, then invokes @p run_fn (the sample's
/// own entry, equivalent to its desktop `main` body). When run_fn returns,
/// the helper cleans up and android_main returns.
///
/// Sample apps declare their android_main as:
/// @code
///     #include <velk-runtime/plugins/android/native_activity_main.h>
///     #include "run.h"
///     extern "C" void android_main(struct android_app* app)
///     {
///         velk::android_native_activity_run(app, []{
///             return velk_simple::run_app(0, nullptr);
///         });
///     }
/// @endcode
VELK_RUNTIME_ANDROID_API
void android_native_activity_run(struct android_app* app, int (*run_fn)());

/// Framework-driven frame loop per docs/runtime/runtime.md.
///
/// Main thread (android_main): pumps NativeActivity events via the Looper
/// and runs `app.update()` + `app.prepare()` on each AChoreographer vsync
/// callback. A dedicated render thread drains prepared frames and calls
/// `app.submit(frame)`. The loop exits when the activity is destroyed.
///
/// Replaces the desktop `while (app.poll()) { app.update(); app.present(); }`
/// loop in the sample on Android.
VELK_RUNTIME_ANDROID_API
void android_run_frame_loop(Application app);

} // namespace velk

#endif // VELK_RUNTIME_ANDROID_NATIVE_ACTIVITY_MAIN_H
