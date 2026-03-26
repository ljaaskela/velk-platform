#ifndef VELK_UI_DEFAULT_SHADERS_H
#define VELK_UI_DEFAULT_SHADERS_H

namespace velk_ui {

inline const char* rect_vertex_src = R"(
#version 330 core

const vec2 quad[4] = vec2[4](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

layout(location = 0) in vec4 inst_rect;
layout(location = 1) in vec4 inst_color;

uniform mat4 u_projection;

out vec4 v_color;

void main()
{
    vec2 pos = quad[gl_VertexID];
    vec2 world_pos = inst_rect.xy + pos * inst_rect.zw;
    gl_Position = u_projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst_color;
}
)";

inline const char* rect_fragment_src = R"(
#version 330 core

in vec4 v_color;
out vec4 frag_color;

void main()
{
    frag_color = v_color;
}
)";

inline const char* text_vertex_src = R"(
#version 330 core

const vec2 quad[4] = vec2[4](
    vec2(0.0, 0.0),
    vec2(1.0, 0.0),
    vec2(0.0, 1.0),
    vec2(1.0, 1.0)
);

layout(location = 0) in vec4 inst_rect;
layout(location = 1) in vec4 inst_color;
layout(location = 2) in vec4 inst_uv;

uniform mat4 u_projection;

out vec4 v_color;
out vec2 v_uv;

void main()
{
    vec2 pos = quad[gl_VertexID];
    vec2 world_pos = inst_rect.xy + pos * inst_rect.zw;
    gl_Position = u_projection * vec4(world_pos, 0.0, 1.0);
    v_color = inst_color;
    v_uv.x = mix(inst_uv.x, inst_uv.z, pos.x);
    v_uv.y = mix(inst_uv.y, inst_uv.w, pos.y);
}
)";

inline const char* text_fragment_src = R"(
#version 330 core

uniform sampler2D u_atlas;

in vec4 v_color;
in vec2 v_uv;
out vec4 frag_color;

void main()
{
    float alpha = texture(u_atlas, v_uv).r;
    frag_color = vec4(v_color.rgb, v_color.a * alpha);
}
)";

} // namespace velk_ui

#endif // VELK_UI_DEFAULT_SHADERS_H
