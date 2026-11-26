// Author: gre
// License: MIT
// Description: A fade through a solid color, black by default
// Tags: fade, color
vec4 transition (vec2 uv) {
  return mix(
    mix(u_color, getFromColor(uv), smoothstep(1.0-u_color_phase, 0.0, progress)),
    mix(u_color, getToColor(uv), smoothstep(    u_color_phase, 1.0, progress)),
    progress);
}
