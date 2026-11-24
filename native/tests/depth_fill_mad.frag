#version 450
// Fragment shader multiply-adds only: four chains of 16 steps on an
// interpolated input (flops.comp for the fragment stage). The result only
// feeds a discard that never happens. Compare with d3d11_depth_fill_probe mad.
layout(location = 0) in vec2 uv;
void main()
{
   float a = uv.x, b = uv.y, c = uv.x * 0.37, d = uv.y * 0.61;
   for (int i = 0; i < 16; ++i) {
      a = a * 0.9999 + b; b = b * 0.9998 + c; c = c * 0.9997 + d; d = d * 0.9996 + a * 0.0001;
   }
   if (a + b + c + d < -1.0) discard;
}
