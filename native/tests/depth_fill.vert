#version 450
// Full-screen triangle; each instance is one layer, drawn back to front so
// every layer passes the depth test (worst-case overdraw).
layout(location = 0) out vec2 uv;
void main()
{
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   uv = p;
   gl_Position = vec4(p * 2.0 - 1.0, 0.9 - float(gl_InstanceIndex) * 0.01, 1.0);
}
