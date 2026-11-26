#version 450
#include "velk.glsl"

layout(local_size_x = 64) in;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D gStorageImagesF16[];

layout(push_constant) uniform PC {
    uint output_id;
    uint width;
    uint height;
    uint view_globals;
    uint chooser_globals;
    float reach;
} pc;

const uint kSteps = 4096u;

// The direction through a corner of the chooser's view, in world axes.
vec3 corner_direction(GlobalData chooser, vec2 ndc)
{
    vec4 p = chooser.inverse_view_projection * vec4(ndc, 0.5, 1.0);
    return normalize(p.xyz / p.w);
}

void main()
{
    uint edge = gl_GlobalInvocationID.x / kSteps;
    if (edge >= 8u) return;
    float t = float(gl_GlobalInvocationID.x % kSteps) / float(kSteps - 1u);

    GlobalData view = velk_globals.data[pc.view_globals];
    GlobalData chooser = velk_globals.data[pc.chooser_globals];
    const vec2 corners[4] = vec2[4](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
    // The chooser's position from this view's camera.
    vec3 eye = chooser.cam_pos.xyz - view.cam_pos.xyz;
    vec3 point;
    if (edge < 4u) {
        point = eye + corner_direction(chooser, corners[edge]) * (pc.reach * t);
    } else {
        vec3 a = eye + corner_direction(chooser, corners[edge - 4u]) * pc.reach;
        vec3 b = eye + corner_direction(chooser, corners[(edge - 3u) & 3u]) * pc.reach;
        point = mix(a, b, t);
    }
    vec4 clip = view.view_projection * vec4(point, 1.0);
    if (clip.w <= 1e-5) return;
    vec2 at = (clip.xy / clip.w * 0.5 + 0.5) * vec2(float(pc.width), float(pc.height));
    if (any(lessThan(at, vec2(0.0))) || at.x >= float(pc.width) || at.y >= float(pc.height)) return;
    imageStore(gStorageImagesF16[nonuniformEXT(pc.output_id)], ivec2(at), vec4(1.0, 1.0, 0.0, 1.0));
}
