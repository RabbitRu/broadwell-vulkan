#version 450
// Same vertex inputs as the captured Talos foliage VS (locations 0-3 per
// vertex, 7-10 per instance) and the same transform (per-instance 3x4 world
// rows, view-projection rows at UBO offset 0) without the wind animation, so
// the positions, clipping and rasterization stay close to the original. Used
// by replay_probe to measure the vertex front end without the shader's ALU work.
layout(location = 0) in vec4 a0;
layout(location = 1) in vec2 a1;
layout(location = 2) in vec4 a2;
layout(location = 3) in vec4 a3;
layout(location = 7) in vec4 r0;
layout(location = 8) in vec4 r1;
layout(location = 9) in vec4 r2;
layout(location = 10) in vec4 a10;
layout(location = 0) out vec4 v0;
layout(set = 0, binding = 0) uniform U { vec4 vp[4]; } u;
void main()
{
   vec3 p = a0.xyz + (a3.xyz + a2.xyz) * 1e-9;
   vec3 w = vec3(dot(r0.xyz, p) + r0.w * a0.w, dot(r1.xyz, p) + r1.w * a0.w, dot(r2.xyz, p) + r2.w * a0.w);
   gl_Position = vec4(dot(u.vp[0].xyz, w) + u.vp[0].w * a0.w, dot(u.vp[1].xyz, w) + u.vp[1].w * a0.w,
                      dot(u.vp[2].xyz, w) + u.vp[2].w * a0.w, dot(u.vp[3].xyz, w) + u.vp[3].w * a0.w);
   v0 = vec4(a1, a10.w, 0.0);
}
