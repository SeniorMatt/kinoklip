// Author: gre
// License: MIT
// Description: A straight wipe in any direction, diagonal and soft-edged by default; a smoothness of 0 gives a hard edge
// Tags: wipe, directional

 
const vec2 center = vec2(0.5, 0.5);
 
vec4 transition (vec2 uv) {
  vec2 v = normalize((vec2(u_direction_x, u_direction_y)));
  v /= abs(v.x)+abs(v.y);
  float d = v.x * center.x + v.y * center.y;
  float m =
    (1.0-step(progress, 0.0)) * // there is something wrong with our formula that makes m not equals 0.0 with progress is 0.0
    (1.0 - smoothstep(-u_smoothness, 0.0, v.x * uv.x + v.y * uv.y - (d-0.5+progress*(1.+u_smoothness))));
  return mix(getFromColor(uv), getToColor(uv), m);
}
