#version 450
// Vertex shader cost with texel-buffer loads in a loop, shaped like The Talos
// Principle's skinning/vegetation shader: <count> iterations of 3 rows of a
// 3x4 matrix. Every vertex is placed outside the clip volume, so only the VS
// runs (nothing is rasterized). Mode 1 replaces the loads by ALU only; mode 2:
// the address depends only on the loop counter (uniform across vertices, as in
// Talos' vegetation shader); mode 3: the same uniform data from a UBO array.
layout(binding = 0) uniform samplerBuffer rows;
layout(binding = 1) uniform Rows { vec4 urows[192]; };
layout(push_constant) uniform Push { int count; int mode; } push;
void main()
{
   vec4 p = vec4(float(gl_VertexIndex & 1023) * 1e-3, float(gl_VertexIndex >> 10) * 1e-3, 0.5, 1.0);
   vec4 acc = vec4(0);
   const int base = (gl_VertexIndex * 7) & 1023;
   for (int i = 0; i < push.count; ++i) {
      if (push.mode == 0) {
         vec4 r0 = texelFetch(rows, (base + i * 3 + 0) & 4095);
         vec4 r1 = texelFetch(rows, (base + i * 3 + 1) & 4095);
         vec4 r2 = texelFetch(rows, (base + i * 3 + 2) & 4095);
         acc += vec4(dot(r0, p), dot(r1, p), dot(r2, p), 0);
      } else if (push.mode == 2) {
         vec4 r0 = texelFetch(rows, 50 + i * 3 + 0);
         vec4 r1 = texelFetch(rows, 50 + i * 3 + 1);
         vec4 r2 = texelFetch(rows, 50 + i * 3 + 2);
         acc += vec4(dot(r0, p), dot(r1, p), dot(r2, p), 0);
      } else if (push.mode == 3) {
         vec4 r0 = urows[(i * 3 + 0) & 191];
         vec4 r1 = urows[(i * 3 + 1) & 191];
         vec4 r2 = urows[(i * 3 + 2) & 191];
         acc += vec4(dot(r0, p), dot(r1, p), dot(r2, p), 0);
      } else {
         acc += vec4(p.x * float(i), p.y + float(i), p.z * 0.5, 0);
      }
   }
   gl_Position = vec4(3.0 + acc.x * 1e-6, 3.0 + acc.y * 1e-6, acc.z * 1e-6, 1.0);
}
