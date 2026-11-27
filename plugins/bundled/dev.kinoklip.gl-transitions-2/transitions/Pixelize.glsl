// Author: gre
// License: MIT
// Description: Both images pixelate, crossfade, then sharpen again
// Tags: pixelate, blend
// forked from https://gist.github.com/benraziel/c528607361d90a072e98

vec4 transition(vec2 uv) {
  float d = min(progress, 1.0 - progress);
  float dist = u_steps>0 ? ceil(d * float(u_steps)) / float(u_steps) : d;
  vec2 squareSize = 2.0 * dist / vec2((ivec2(u_squaresMinX, u_squaresMinY)));
  vec2 p = dist>0.0 ? (floor(uv / squareSize) + 0.5) * squareSize : uv;
  return mix(getFromColor(p), getToColor(p), progress);
}
