#version 450
// Fragment shader arithmetic on an interpolated input, shaped like a lit
// surface: two normalizations, two dot products, a power, a few MADs. The
// result only feeds a discard that never happens, so the depth-only pass of
// depth_fill_probe keeps the code. Compare with d3d11_depth_fill_probe light.
layout(location = 0) in vec2 uv;
void main()
{
   vec3 n = normalize(vec3(uv * 2.0 - 1.0, 0.37));
   const vec3 l = vec3(0.3, 0.61, 0.73);
   float d = max(dot(n, l), 0.0);
   vec3 h = normalize(l + vec3(uv.y, uv.x, 1.0));
   float s = pow(max(dot(n, h), 0.0), 16.0);
   vec3 c = vec3(0.2, 0.3, 0.4) * d + s;
   c = c * (1.0 - uv.x * 0.1) + uv.y * 0.05;
   if (c.x + c.y + c.z < -1.0) discard;
}
