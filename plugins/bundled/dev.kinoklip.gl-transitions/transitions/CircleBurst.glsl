// Author: gre
// License: MIT
// Description: Discs of three solid colors burst out of a point one after another, each covering the last, then a hole bursts open from the same point in the last one to reveal the incoming image
// Tags: circle, burst, color, reveal, motion-graphics
// Preview: 0.6

const float AA = 0.0015;
const float SW = 0.03;
const float SO = 0.32;

float phase(float s, float d) {
  return clamp((progress - s) / d, 0.0, 1.0);
}

float radius(float t, float R) {
  float u = 1.0 - t;
  return mix(-SW - AA, R, mix(t, 1.0 - u * u * u, 0.8));
}

vec2 mirror(vec2 v) {
  return 1.0 - abs(1.0 - mod(abs(v), 2.0));
}

// a layer of color c and radius r laid over p (premultiplied), with its shadow around it, at distance x
vec4 layer(vec4 p, vec4 c, float r, float x) {
  float k = smoothstep(r - AA, r + AA, x);
  float s = SO * c.a * k * (1.0 - smoothstep(0.0, SW, x - r));
  p = p * (1.0 - s) + vec4(0.0, 0.0, 0.0, s);
  k = c.a * (1.0 - k);
  return p * (1.0 - k) + vec4(c.rgb, 1.0) * k;
}

vec4 transition(vec2 uv) {
  vec2 asp = vec2(ratio, 1.0);
  float x = length((uv - (vec2(u_center_x, u_center_y))) * asp);
  // past the farthest corner, with room for the shadows
  float R = length(max((vec2(u_center_x, u_center_y)), 1.0 - (vec2(u_center_x, u_center_y))) * asp) + SW + AA;
  float k = clamp(u_stagger, 0.0, 1.0);
  float d = 1.0 / (1.0 + 3.0 * k);

  float t = phase(0.0, d);
  vec4 a = getFromColor(mirror((vec2(u_center_x, u_center_y)) + (uv - (vec2(u_center_x, u_center_y))) / (1.0 + 0.06 * t * (2.0 - t))));
  vec4 p = vec4(a.rgb * a.a, a.a);
  p = layer(p, u_color1, radius(t, R), x);
  p = layer(p, u_color2, radius(phase(k * d, d), R), x);
  float r = radius(phase(2.0 * k * d, d), R);
  p = layer(p, u_color3, r, x);

  t = phase(3.0 * k * d, d);
  float h = radius(t, R);
  float u = 1.0 - t;
  vec4 b = getToColor(mirror((vec2(u_center_x, u_center_y)) + (uv - (vec2(u_center_x, u_center_y))) / (1.0 + 0.14 * u * u * (1.0 - 2.4 * t))));
  float s = SO * u_color3.a * clamp((r - h) / SW, 0.0, 1.0) * (1.0 - smoothstep(0.0, SW, h - x));
  vec4 q = vec4(b.rgb * b.a * (1.0 - s), b.a * (1.0 - s) + s);
  p = mix(p, q, 1.0 - smoothstep(h - AA, h + AA, x));
  return p.a > 0.0 ? vec4(p.rgb / p.a, p.a) : vec4(0.0);
}
