// Author: mandubian
// License: MIT
// Description: The outgoing image is pulled along a parametric curve into a psychedelic swirl
// Tags: distort, psychedelic

vec4 transition(vec2 uv) {
  vec2 p = uv.xy / vec2(1.0).xy;
  vec2 dir = p - vec2(.5);
  float dist = length(dir);
  float x = (u_a - u_b) * cos(progress) + u_b * cos(progress * ((u_a / u_b) - 1.) );
  float y = (u_a - u_b) * sin(progress) - u_b * sin(progress * ((u_a / u_b) - 1.));
  vec2 offset = dir * vec2(sin(progress  * dist * u_amplitude * x), sin(progress * dist * u_amplitude * y)) / u_smoothness;
  return mix(getFromColor(p + offset), getToColor(p), smoothstep(0.2, 1.0, progress));
}
