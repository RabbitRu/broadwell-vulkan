#version 450
// Only the 4 per-vertex elements of the foliage VS (no per-instance elements):
// the vertex front end if instance data came from the shader instead of VF.
layout(location = 0) in vec4 a0;
layout(location = 1) in vec2 a1;
layout(location = 2) in vec4 a2;
layout(location = 3) in vec4 a3;
layout(location = 0) out vec4 v0;
layout(set = 0, binding = 0) uniform U { vec4 vp[4]; } u;
void main()
{
   vec4 p = vec4(a0.xyz + (a2.xyz + a3.xyz) * 1e-9, 1.0);
   gl_Position = vec4(dot(u.vp[0], p), dot(u.vp[1], p), dot(u.vp[2], p), dot(u.vp[3], p));
   v0 = vec4(a1, 0.0, 0.0);
}