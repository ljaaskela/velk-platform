#ifndef VELK_RENDER_GPU_DATA_H
#define VELK_RENDER_GPU_DATA_H

#include <cstdint>

namespace velk {

// Shared CPU/GPU data structures.
//
// These structs define the memory layout that both CPU code and shaders read.
// The CPU writes them into mapped GPU buffers; shaders access them via
// buffer device address (pointer) dereference.
//
// Keep layouts compatible with std430 packing (natural alignment, no padding
// surprises). All structs are tightly packed with explicit padding where needed.

struct FrameGlobals
{
    float projection[16];
    float viewport[4]; // width, height, 1/width, 1/height
};

struct RectInstance
{
    float pos[2];
    float size[2];
    float color[4];
};

struct TextInstance
{
    float pos[2];
    float size[2];
    float color[4];
    float uv_min[2];
    float uv_max[2];
};

struct GradientParams
{
    float start_color[4];
    float end_color[4];
    float angle;
    float _pad[3];
};

// Standard draw data header written by the renderer into the staging buffer.
// The shader receives a pointer to this via push constants.
// Material-specific data (e.g. GradientParams) follows immediately after.
// Padded to 32 bytes so material data that follows (e.g. vec4) meets
// std430 16-byte alignment requirements in shaders.
struct DrawDataHeader
{
    uint64_t globals_address;
    uint64_t instances_address;
    uint32_t texture_id;
    uint32_t instance_count;
    uint32_t _pad[2];
};

} // namespace velk

#endif // VELK_RENDER_GPU_DATA_H
