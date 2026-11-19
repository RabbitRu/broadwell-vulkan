#version 450
// Position only (4 inputs, no varyings): a smaller VS URB entry than the foliage VS.
layout(location = 0) in vec4 a0;
layout(location = 7) in vec4 r0;
layout(location = 8) in vec4 r1;
layout(location = 9) in vec4 r2;
layout(set = 0, binding = 0) uniform U { vec4 vp[4]; } u;
void main()
{
   vec3 p = a0.xyz;
   vec3 w = vec3(dot(r0.xyz, p) + r0.w, dot(r1.xyz, p) + r1.w, dot(r2.xyz, p) + r2.w);
   gl_Position = vec4(dot(u.vp[0].xyz, w) + u.vp[0].w, dot(u.vp[1].xyz, w) + u.vp[1].w,
                      dot(u.vp[2].xyz, w) + u.vp[2].w, dot(u.vp[3].xyz, w) + u.vp[3].w);
}