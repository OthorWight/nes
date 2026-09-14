#ifndef NES_CRT_SHADER_H
#define NES_CRT_SHADER_H

/* Shared math for D3D11, Metal and GL, in source-pixel coordinates.
   Round a corner only when both adjoining pixels and the diagonal agree.
   This changes contour coverage locally, preserving solid outlines, flat
   colors and ambiguous diagonal details. Straight edges have a narrow blend;
   no full-pixel low-pass or linear-light lift is applied. */
#define CRT_COLOR_BODY \
    "vec2 p = crop.xy + uv * crop.zw;" \
    "vec2 centered = p - vec2(0.5, 0.5);" \
    "vec2 q = floor(centered) + vec2(0.5, 0.5)" \
    "       + smoothstep(vec2(0.38, 0.44), vec2(0.62, 0.56), fract(centered));" \
    "vec3 color = READ(q);" \
    "vec2 base = floor(p) + vec2(0.5, 0.5);" \
    "vec2 f = fract(p);" \
    "vec2 direction = vec2(f.x < 0.5 ? -1.0 : 1.0, f.y < 0.5 ? -1.0 : 1.0);" \
    "vec3 c = READ(base);" \
    "vec3 h = READ(base + vec2(direction.x, 0.0));" \
    "vec3 v = READ(base + vec2(0.0, direction.y));" \
    "vec3 d = READ(base + direction);" \
    "if (dot(h-v, h-v) < 0.0025 && dot(h-d, h-d) < 0.0025 && dot(c-h, c-h) > 0.01) {" \
    "    vec2 corner = max(abs(f - vec2(0.5, 0.5)) - vec2(0.18, 0.18), vec2(0.0, 0.0));" \
    "    float feather = clamp(0.5 / params.x, 0.025, 0.10);" \
    "    float coverage = smoothstep(-feather, feather, length(corner) - 0.32);" \
    "    color = c + (h - c) * coverage;" \
    "}" \
    "vec3 glow = (h * smoothstep(0.65, 1.0, max(h.r, max(h.g, h.b)))" \
    "           + v * smoothstep(0.65, 1.0, max(v.r, max(v.g, v.b)))) * 0.001;" \
    "float scan = 1.0 - 0.06 * smoothstep(1.0, 2.5, params.x)" \
    "           * (0.5 + 0.5 * cos(6.2831853 * p.y));" \
    "color = clamp(color + glow * (vec3(1.0, 1.0, 1.0) - color), 0.0, 1.0);" \
    "return vec4(color * scan, 1.0);"

/* Clamp every tap to the visible crop so hidden overscan cannot bleed in. */
#define CRT_SAMPLE_COORD "(clamp(q, crop.xy + vec2(0.5, 0.5), crop.xy + crop.zw - vec2(0.5, 0.5)) / vec2(256.0, 240.0))"

static sg_shader_desc crt_shader_desc(void) {
    sg_shader_desc d = {0};
    d.attrs[0].glsl_name = "position";
    d.attrs[0].hlsl_sem_name = "POSITION";
    d.uniform_blocks[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.uniform_blocks[0].size = 8 * sizeof(float);
    d.uniform_blocks[0].glsl_uniforms[0] = (sg_glsl_shader_uniform){.glsl_name = "crop", .type = SG_UNIFORMTYPE_FLOAT4};
    d.uniform_blocks[0].glsl_uniforms[1] = (sg_glsl_shader_uniform){.glsl_name = "params", .type = SG_UNIFORMTYPE_FLOAT4};
    d.views[0].texture.stage = SG_SHADERSTAGE_FRAGMENT;
    d.views[0].texture.image_type = SG_IMAGETYPE_2D;
    d.views[0].texture.sample_type = SG_IMAGESAMPLETYPE_FLOAT;
    d.samplers[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.samplers[0].sampler_type = SG_SAMPLERTYPE_FILTERING;
    d.texture_sampler_pairs[0].stage = SG_SHADERSTAGE_FRAGMENT;
    d.texture_sampler_pairs[0].glsl_name = "tex_smp";
    d.label = "crt-shader";
#if defined(SOKOL_D3D11)
    d.vertex_func.source =
        "struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
        "V main(float2 p : POSITION) { V o; o.pos = float4(p, 0, 1);"
        "o.uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); return o; }";
    d.fragment_func.source =
        "#define vec2 float2\n#define vec3 float3\n#define vec4 float4\n#define fract frac\n"
        "cbuffer P : register(b0) { float4 crop; float4 params; };"
        "Texture2D tex : register(t0); SamplerState smp : register(s0);"
        "vec3 read_pixel(vec2 q) { return tex.SampleLevel(smp, " CRT_SAMPLE_COORD ", 0).rgb; }\n"
        "#define READ(q) read_pixel(q)\n"
        "float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target0 {"
        CRT_COLOR_BODY "}";
#elif defined(SOKOL_METAL)
    d.vertex_func.entry = "crt_vs";
    d.fragment_func.entry = "crt_fs";
    d.vertex_func.source =
        "#include <metal_stdlib>\nusing namespace metal;"
        "struct V { float4 pos [[position]]; float2 uv; };"
        "struct In { float2 p [[attribute(0)]]; };"
        "vertex V crt_vs(In i [[stage_in]]) { V o; o.pos = float4(i.p, 0, 1);"
        "o.uv = float2(i.p.x * 0.5 + 0.5, 0.5 - i.p.y * 0.5); return o; }";
    d.fragment_func.source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "#define vec2 float2\n#define vec3 float3\n#define vec4 float4\n"
        "struct V { float4 pos [[position]]; float2 uv; };"
        "struct P { float4 crop; float4 params; };"
        "vec3 read_pixel(texture2d<float> tex, sampler smp, vec4 crop, vec2 q) {"
        "return tex.sample(smp, " CRT_SAMPLE_COORD ", level(0.0)).rgb; }\n"
        "#define READ(q) read_pixel(tex, smp, crop, q)\n"
        "fragment float4 crt_fs(V i [[stage_in]], constant P& u [[buffer(0)]],"
        "texture2d<float> tex [[texture(0)]], sampler smp [[sampler(0)]]) {"
        "float2 uv = i.uv; float4 crop = u.crop; float4 params = u.params;"
        CRT_COLOR_BODY "}";
#else
    d.vertex_func.source =
        "#version 410\nlayout(location=0) in vec2 position; out vec2 uv;"
        "void main() { gl_Position = vec4(position, 0.0, 1.0);"
        "uv = vec2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5); }";
    d.fragment_func.source =
        "#version 410\nuniform vec4 crop; uniform vec4 params; uniform sampler2D tex_smp;"
        "in vec2 uv; out vec4 frag_color;"
        "vec3 read_pixel(vec2 q) { return textureLod(tex_smp, " CRT_SAMPLE_COORD ", 0.0).rgb; }\n"
        "#define READ(q) read_pixel(q)\n"
        "vec4 crt_color() {" CRT_COLOR_BODY "}"
        "void main() { frag_color = crt_color(); }";
#endif
    return d;
}
#undef CRT_SAMPLE_COORD
#undef CRT_COLOR_BODY
#endif
