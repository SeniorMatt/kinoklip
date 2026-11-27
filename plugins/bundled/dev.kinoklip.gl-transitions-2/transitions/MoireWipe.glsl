// Author: gre
// License: MIT
// Description: Two sets of fine concentric rings, centered on two points that drift toward each other, overlay the image and interfere into a shifting moire of wide bands; the bands open onto the incoming image and widen until it fills the frame, while the thin dark and light ring lines fade away
// Tags: moire, rings, interference, pattern, graphic, reveal

const float PI = 3.141592653589793;
const float AA = 0.0012;
// half width of a ring line, in ring spacings
const float LINE = 0.1;
// widest antialiased edge of the bands, in bands, which also sets when they start and finish
// opening
const float EDGE = 0.05;
const float START = 0.35;
// strength of the ring lines at lines = 1, so the image always shows through them
const float INK = 0.7;

// coverage of the ring lines at radius r, box-filtered over the antialiasing width, and
// faded to their mean coverage when the rings get too fine to resolve
float ringLines(float r, float f) {
  float q = abs(fract(r * f + 0.5) - 0.5);
  float aa = AA * f;
  float c = clamp((LINE - q) / (2.0 * aa) + 0.5, 0.0, 1.0);
  return mix(c, 2.0 * LINE, clamp(10.0 * aa - 0.5, 0.0, 1.0));
}

vec4 transition(vec2 uv) {
  vec4 a = getFromColor(uv);
  vec4 b = getToColor(uv);

  float t = progress * progress * (3.0 - 2.0 * progress);
  float f = max(u_density, 1.0);
  vec2 p = (uv - 0.5) * vec2(ratio, 1.0);

  float ang = 2.0 * PI * u_drift * t + START;
  float k = max(u_bands, 1.0) * (1.0 - 0.75 * t);
  vec2 axis = vec2(cos(ang), sin(ang)) * 0.5 * k / f;
  float r1 = length(p - axis);
  float r2 = length(p + axis);
  // the second set is slightly finer than the first, which bends the bands into curves
  float f2 = f + 1.2 * k;

  // the beat of the two ring sets: wide bands where their rings line up, at this distance
  // from the middle of the nearest band, in bands
  float beat = abs(fract(f2 * r2 - f * r1 + 0.5) - 0.5);
  vec2 grad = f2 * (p + axis) / max(r2, AA) - f * (p - axis) / max(r1, AA);
  float s = min(AA * length(grad) + 0.004, EDGE);
  float w = mix(-EDGE, 0.5 + EDGE, t);
  float m = 1.0 - smoothstep(w - s, w + s, beat);

  vec4 c = mix(vec4(a.rgb * a.a, a.a), vec4(b.rgb * b.a, b.a), m);

  float o = INK * clamp(u_lines, 0.0, 1.0) * sin(PI * t);
  float l = 1.0 - (1.0 - o * ringLines(r1, f)) * (1.0 - o * ringLines(r2, f2));
  c.rgb = mix(c.rgb, vec3(c.a * m), l);
  return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}
