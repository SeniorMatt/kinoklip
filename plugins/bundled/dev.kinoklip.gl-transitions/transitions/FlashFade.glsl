// Author: gre
// License: MIT
// Description: A crossfade that brightens toward a color in the middle
// Tags: fade, color, flash
vec4 transition (vec2 uv) {
  return mix(
    getFromColor(uv) + vec4(progress*u_color.rgb, 0.0),
    getToColor(uv) + vec4((1.0-progress)*u_color.rgb, 0.0),
    progress
  );
}
