// Author: gre
// License: MIT
// Description: A flowing plasma of sine waves, as in the old demos, draws moving bands through which the incoming image spreads like tendrils, with a glowing edge
// Tags: wipe, plasma, organic, glow

const float PI = 3.14159265;

vec4 transition(vec2 uv) {
  vec4 a = getFromColor(uv);
  vec4 b = getToColor(uv);

  // measured in square pixels, so the waves are not stretched
  vec2 p = (uv - 0.5) * vec2(ratio, 1.0) * max(u_scale, 0.0) * 2.0 * PI;
  // the phase of the waves; the folded plasma below then changes by at most 0.9 * flow per
  // unit of progress, slower than the threshold, so the reveal never goes back
  float t = clamp(u_flow, 0.0, 1.0) * progress * 0.5;
  float v = sin(p.x * 0.6 + t)
    + sin(p.y * 0.8 - t * 0.8)
    + sin((p.x * 0.5 + p.y * 0.7) * 0.7 + t * 0.6)
    + sin(length(p - vec2(0.15, -0.1) * max(u_scale, 0.0) * 2.0 * PI) * 0.9 - t * 1.2);
  v = abs(fract(v * 0.25 + 0.5) * 2.0 - 1.0);

  // the soft edge is fully below every band at the start and above every band at the end
  float s = max(u_smoothness, 0.001);
  float threshold = mix(-s, 1.0 + s, progress);
  float m = 1.0 - smoothstep(threshold - s, threshold + s, v);

  // mixed premultiplied, so transparent images do not bleed their color
  vec4 col = mix(vec4(a.rgb * a.a, a.a), vec4(b.rgb * b.a, b.a), m);
  if (col.a <= 0.0) return vec4(0.0);
  // a light along the edge, adding nothing to the alpha
  float glow = 4.0 * m * (1.0 - m) * max(u_edge_strength, 0.0);
  return vec4(col.rgb / col.a + u_edge_color.rgb * glow, col.a);
}
