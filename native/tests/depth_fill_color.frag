#version 450
// One texture sample written to the colour target (depth_fill_probe, depth mode 3).
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main()
{
   color = texture(tex, uv * 4.0);
}
