// Author: gre
// License: MIT
// Description: Solid color panels, slightly staggered, sweep across and cover the frame, then sweep off the far side and reveal the incoming image
// Tags: wipe, panels, color, slanted, motion-graphics
// Preview: 0.75

// the frame is fully covered from cut - hold to cut + hold, where the image switches
const float cut = 0.46;
const float hold = 0.01;
const float aa = 0.0015;

float ease(float t) {
  float s = clamp(t, 0.0, 1.0);
  float u = 1.0 - s * s * (3.0 - 2.0 * s);
  return 1.0 - u * u * u;
}

float cover(float x, float back, float front) {
  return smoothstep(back - aa, back + aa, x) * (1.0 - smoothstep(front - aa, front + aa, x));
}

vec4 transition(vec2 uv) {
  vec2 dir = length((vec2(u_directionX, u_directionY))) > 0.0 ? normalize((vec2(u_directionX, u_directionY))) : vec2(1.0, 0.0);
  vec2 p = (uv - 0.5) * vec2(ratio, 1.0);
  float k = clamp(u_skew, -1.0, 1.0);
  vec2 a = (dir + k * vec2(-dir.y, dir.x)) / sqrt(1.0 + k * k);
  float x = dot(p, a);
  float e = 0.5 * (abs(a.x) * ratio + abs(a.y));
  float start = -e - 2.0 * aa;
  float span = 2.0 * (e + 2.0 * aa);

  int n = u_panels < 1 ? 1 : u_panels > 3 ? 3 : u_panels;
  float m = float(n - 1);
  float st = clamp(u_stagger, 0.0, 1.0);
  vec2 d = vec2(cut - hold, 1.0 - cut - hold) / (1.0 + m * st);

  // the first panel enters first and leaves last; both images are sampled so no texture read sits in a divergent branch
  float front0 = start + span * ease(progress / d.x);
  float back0 = start + span * ease((progress - 1.0 + d.y) / d.y);
  vec4 c = mix(getToColor(uv), getFromColor(uv), step(0.5 * (front0 + back0), x));
  c = mix(c, u_color1, cover(x, back0, front0));

  for (int i = 1; i < 3; i++) {
    if (i < n) {
      vec2 t = float(i) * st * d;
      float front = start + span * ease((progress - t.x) / d.x);
      float back = start + span * ease((progress - 1.0 + d.y + t.y) / d.y);
      c = mix(c, i == 1 ? u_color2 : u_color3, cover(x, back, front));
    }
  }
  return c.a > 0.0 ? c : vec4(0.0);
}
