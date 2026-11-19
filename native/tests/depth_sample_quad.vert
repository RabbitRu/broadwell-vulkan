#version 450
// Left half of the target at depth 0.5 (two triangles).
void main()
{
   const vec2 p[6] = vec2[](vec2(-1, -1), vec2(0, -1), vec2(-1, 1), vec2(0, -1), vec2(0, 1), vec2(-1, 1));
   gl_Position = vec4(p[gl_VertexIndex], 0.5, 1.0);
}
