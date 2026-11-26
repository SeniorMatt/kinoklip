// Author: mandubian
// License: MIT
// Description: Wavy, color-separated ripples scramble the outgoing image into the incoming one
// Tags: wave, distort, chromatic
const float PI = 3.14159265358979323846264;
float compute(vec2 p, float gltProgress, vec2 center) {
vec2 o = p*sin(gltProgress * u_amplitude)-center;
vec2 h = vec2(1., 0.);
// butterfly polar function (don't ask me why this one :))
float theta = acos(dot(o, h)) * u_waves;
float s = sin((2.*theta - PI) / 24.);
float s2 = s * s;
return (exp(cos(theta)) - 2.*cos(4.*theta) + s2 * s2 * s) / 10.;
}
vec4 transition(vec2 uv) {
  vec2 p = uv;
  float inv = 1. - gltProgress;
  float disp = compute(p, gltProgress, vec2(0.5, 0.5));
  vec4 texTo = getToColor(p + inv*disp);
  vec4 fromCenter = getFromColor(p + gltProgress*disp);
  vec4 texFrom = vec4(
    getFromColor(p + gltProgress*disp*(1.0 - u_colorSeparation)).r,
    fromCenter.g,
    getFromColor(p + gltProgress*disp*(1.0 + u_colorSeparation)).b,
    fromCenter.a);
  return texTo*gltProgress + texFrom*inv;
}
