// Author: gre
// License: MIT
// Description: The frame is cut into interleaved strips that slide in opposite directions, each pushing the outgoing image out and the incoming one in
// Tags: slide, stripes, push, horizontal

vec4 transition (vec2 uv) {
  // a: the axis along which the strips slide, b: the axis across them
  float a = u_vertical ? uv.y : uv.x;
  float b = u_vertical ? uv.x : uv.y;
  float dir = mod(floor(b * float(u_strips)), 2.0) < 0.5 ? 1.0 : -1.0;
  // ease in and out, but keep moving from the first frame
  float t = mix(progress, progress * progress * (3.0 - 2.0 * progress), 0.6);
  float fa = a - dir * t;
  if (fa >= 0.0 && fa <= 1.0) {
    return getFromColor(u_vertical ? vec2(b, fa) : vec2(fa, b));
  }
  float ta = fa + dir;
  return getToColor(u_vertical ? vec2(b, ta) : vec2(ta, b));
}
