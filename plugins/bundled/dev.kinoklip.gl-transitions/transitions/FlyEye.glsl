// Author: gre
// License: MIT
// Description: The image breaks into a fly's-eye grid of lenses with color separation
// Tags: lens, distort, chromatic

vec4 transition(vec2 p) {
  float inv = 1. - progress;
  vec2 disp = u_size*vec2(cos(u_zoom*p.x), sin(u_zoom*p.y));
  vec4 texTo = getToColor(p + inv*disp);
  vec4 fromCenter = getFromColor(p + progress*disp);
  vec4 texFrom = vec4(
    getFromColor(p + progress*disp*(1.0 - u_colorSeparation)).r,
    fromCenter.g,
    getFromColor(p + progress*disp*(1.0 + u_colorSeparation)).b,
    fromCenter.a);
  return texTo*progress + texFrom*inv;
}
