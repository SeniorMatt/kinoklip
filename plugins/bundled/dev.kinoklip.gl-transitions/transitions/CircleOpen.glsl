// Author: gre
// License: MIT
// Description: A soft-edged circle opens from the center to reveal the incoming image
// Tags: circle, iris, wipe

const vec2 center = vec2(0.5, 0.5);
const float SQRT_2 = 1.414213562373;

vec4 transition (vec2 uv) {
  float x = u_opening ? progress : 1.-progress;
  float m = smoothstep(-u_smoothness, 0.0, SQRT_2*distance(center, uv) - x*(1.+u_smoothness));
  return mix(getFromColor(uv), getToColor(uv), u_opening ? 1.-m : m);
}
