#version 450
// The least a fragment shader with a discard can do: one comparison of an
// interpolated input, never true. The fixed cost of a pixel that goes through
// the shader. Compare with d3d11_depth_fill_probe kill.
layout(location = 0) in vec2 uv;
void main()
{
   if (uv.x < -1.0) discard;
}
