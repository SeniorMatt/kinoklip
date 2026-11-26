// Author: gre
// License: MIT
// Description: The stage lights go down on the outgoing image and a soft oval spotlight searches across it along a curved path, comes to rest and lights the incoming image inside it, then widens until the stage lights come back up on the incoming image
// Tags: light, spotlight, stage, iris, reveal
// Preview: 0.7

// height of the oval over its width: a cone of light hitting a stage seen at an angle
const float FLAT = 0.72;
// distance to the spotlight's center, in oval radii, inside which the incoming image is fully lit
const float LIT = 0.74;

// a point of the curved path the spotlight searches along, from the lower left to its resting point
vec2 path(float t, vec2 c) {
  vec2 a = vec2(0.08, 0.3);
  vec2 b = vec2(0.35, 1.15);
  vec2 d = vec2(1.05, 0.95);
  float s = 1.0 - t;
  return s * s * s * a + 3.0 * s * s * t * b + 3.0 * s * t * t * d + t * t * t * c;
}

vec4 transition(vec2 uv) {
  vec2 asp = vec2(ratio, 1.0);
  float p = progress;

  // the spotlight slides along its path and eases to rest, its last few steps overlapping the start
  // of its widening so the motion never stops
  float t = clamp(p / 0.5, 0.0, 1.0);
  t = t * t * (3.0 - 2.0 * t);
  vec2 c = path(t, (vec2(u_center_x, u_center_y)));

  vec2 far = max((vec2(u_center_x, u_center_y)), 1.0 - (vec2(u_center_x, u_center_y))) * asp;
  float full = length(vec2(far.x, far.y / FLAT)) / LIT;
  float g = smoothstep(0.44, 1.0, p);
  float radius = mix(0.5 * u_size, full, g * (0.3 + 0.7 * g));

  // distance to the spotlight's center, 1 on the oval's edge
  vec2 q = (uv - c) * asp;
  float r = length(vec2(q.x, q.y / FLAT)) / radius;

  float core = 1.0 - smoothstep(0.72, 1.08, r);
  float ring = (r - 0.9) / 0.09;
  float spot = max(core * (1.0 - 0.1 * r * r), 0.12 * (1.0 - smoothstep(0.9, 2.4, r))) + 0.16 * exp(-ring * ring);

  float s = u_darkness * smoothstep(0.0, 0.18, p) * (1.0 - smoothstep(0.72, 1.0, p));
  vec3 light = 1.0 - s + s * spot * u_warmth.rgb;

  float lit = smoothstep(0.42, 0.58, p) * (1.0 - smoothstep(LIT, 1.0, r));
  vec4 col = mix(getFromColor(uv), getToColor(uv), lit);
  return vec4(col.rgb * light, col.a);
}
