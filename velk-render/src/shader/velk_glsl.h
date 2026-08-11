#ifndef VELK_RENDER_VELK_GLSL_H
#define VELK_RENDER_VELK_GLSL_H

#include <velk/string_view.h>

namespace velk {

/// Source for the framework-level "velk.glsl" virtual include. Registered
/// into the render context's shader compiler during init. Its length comes
/// from the literal, so no run-time scan is needed.
extern const string_view kVelkGlsl;

} // namespace velk

#endif // VELK_RENDER_VELK_GLSL_H
