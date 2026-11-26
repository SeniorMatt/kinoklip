// Author: gre
// License: MIT
// Description: A soft-edged diamond grows from the center to reveal the incoming image
// Tags: shape, wipe, iris

vec4 transition (vec2 uv) {
  // Measured in square pixels, so the diamond is not stretched
  vec2 d = abs(uv - (vec2(u_center_x, u_center_y))) * vec2(ratio, 1.0);
  vec2 far = max((vec2(u_center_x, u_center_y)), 1.0 - (vec2(u_center_x, u_center_y))) * vec2(ratio, 1.0);
  float dist = (d.x + d.y) / (far.x + far.y);
  float s = max(u_smoothness, 0.001);
  float m = smoothstep(0.0, s, progress * (1.0 + s) - dist);
  return mix(getFromColor(uv), getToColor(uv), m);
}
