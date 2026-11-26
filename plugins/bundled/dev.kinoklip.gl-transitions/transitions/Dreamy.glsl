// Author: mikolalysenko
// License: MIT
// Description: A crossfade with a gentle wavy distortion
// Tags: wave, blend

vec2 offset(float gltProgress, float x, float theta) {
  float phase = gltProgress*gltProgress + gltProgress + theta;
  float shifty = 0.03*gltProgress*cos(10.0*(gltProgress+x));
  return vec2(0, shifty);
}
vec4 transition(vec2 p) {
  return mix(getFromColor(p + offset(gltProgress, p.x, 0.0)), getToColor(p + offset(1.0-gltProgress, p.x, 3.14)), gltProgress);
}
