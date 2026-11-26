// Author: gre
// License: MIT
// Description: The incoming image appears through vertical slices that widen
// Tags: slices, stripes, wipe

vec4 transition (vec2 p) {
  float pr = smoothstep(-u_smoothness, 0.0, p.x - progress * (1.0 + u_smoothness));
  float s = step(pr, fract(u_count * p.x));
  return mix(getFromColor(p), getToColor(p), s);
}
