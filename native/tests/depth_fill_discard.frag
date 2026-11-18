#version 450
// Alpha-tested shadow caster: one texture sample, discard below 0.5.
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
void main()
{
   if (texture(tex, uv * 4.0).a < 0.5) discard;
}
