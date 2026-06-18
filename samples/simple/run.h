#ifndef VELK_UI_SAMPLE_SIMPLE_RUN_H
#define VELK_UI_SAMPLE_SIMPLE_RUN_H

namespace velk_simple {

/// Sample entry point shared by desktop `main` and Android `android_main`.
/// Returns the process exit code.
int run_app(int argc, char* argv[]);

} // namespace velk_simple

#endif // VELK_UI_SAMPLE_SIMPLE_RUN_H
