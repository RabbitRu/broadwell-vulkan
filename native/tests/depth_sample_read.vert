#version 450
// One triangle covering the target.
void main()
{
   const vec2 p[3] = vec2[](vec2(-1, -1), vec2(3, -1), vec2(-1, 3));
   gl_Position = vec4(p[gl_VertexIndex], 0.0, 1.0);
}
