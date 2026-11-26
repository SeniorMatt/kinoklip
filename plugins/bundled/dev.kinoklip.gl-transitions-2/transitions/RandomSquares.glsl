// Author: gre
// License: MIT
// Description: The incoming image appears in random squares
// Tags: squares, grid, dissolve

 
float rand (vec2 co) {
  return fract(sin(dot(co.xy ,vec2(12.9898,78.233))) * 43758.5453);
}

vec4 transition(vec2 p) {
  float r = rand(floor(vec2((ivec2(u_sizeX, u_sizeY))) * p));
  float m = smoothstep(0.0, -u_smoothness, r - (progress * (1.0 + u_smoothness)));
  return mix(getFromColor(p), getToColor(p), m);
}
