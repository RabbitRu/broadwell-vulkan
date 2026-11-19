#version 450
// Copies the depth texel under each pixel.
layout(set = 0, binding = 0) uniform sampler2D depth;
layout(location = 0) out float value;
void main()
{
   value = texelFetch(depth, ivec2(gl_FragCoord.xy), 0).r;
}
