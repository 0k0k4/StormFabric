#pragma once
// Same native monitor projection/depth/composition as the shipped GLSL, with source-only FX.
// No ROM files are changed. Attribute bindings are copied from the native monitor program.
namespace swml {
inline constexpr const char* kVideoVertexShader = R"GLSL(#version 330 core
in vec3 vertexPosition_in;
in vec2 vertexCoord0_in;
in vec4 vertexColor_in;
out float log_z;
out vec4 vertexColor_out;
out vec2 vertexCoord0_out;
uniform mat4 mat_view_proj;
uniform mat4 mat_world;
void main() {
    gl_Position = mat_view_proj * mat_world * vec4(vertexPosition_in, 1.0);
    log_z = gl_Position.w * 0.01 + 1.0;
    vertexColor_out = vertexColor_in;
    vertexCoord0_out = vertexCoord0_in;
}
)GLSL";
inline constexpr const char* kVideoFragmentShader = R"GLSL(#version 330 core
in float log_z;
in vec4 vertexColor_out;
in vec2 vertexCoord0_out;
out vec4 color_out;
uniform sampler2D texture_video;
uniform sampler2D texture_overlay;
uniform float alpha_video;
uniform float alpha;
uniform vec4 multiply_color;
uniform vec4 fx_transform; // offset normalized to monitor size; scale from centre
uniform vec4 fx_tint;
uniform vec2 fx_filter;
void main() {
    gl_FragDepth = log2(log_z) / log2(20100.0 * 0.01 + 1.0);
    vec2 uv = (vertexCoord0_out - vec2(0.5) - fx_transform.xy) / fx_transform.zw + vec2(0.5);
    vec4 video_sample = vec4(0.0);
    if (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0))))
        video_sample = texture(texture_video, uv);
    float gray = dot(video_sample.rgb, vec3(0.2126, 0.7152, 0.0722));
    video_sample.rgb = mix(video_sample.rgb, vec3(gray), fx_filter.x);
    video_sample.rgb = mix(video_sample.rgb, vec3(1.0) - video_sample.rgb, fx_filter.y);
    video_sample *= fx_tint;
    vec4 overlay_sample = texture(texture_overlay, vertexCoord0_out);
    vec3 mix_sample = mix(video_sample.rgb * video_sample.a * alpha_video,
                          overlay_sample.rgb, overlay_sample.a);
    color_out = vec4(mix_sample, 1.0) * vertexColor_out * multiply_color;
    color_out.a *= alpha;
}
)GLSL";
}
