// Author: gre
// License: MIT
// Description: Two hands sweep away from each other around the center, opening a widening wedge that reveals the incoming image
// Tags: wipe, radial, clock, wedge

#define PI 3.141592653589793

vec4 transition (vec2 uv) {
  // Measured in square pixels, so the angles are not stretched
  vec2 q = (uv - (vec2(u_centerX, u_centerY))) * vec2(ratio, 1.0);
  float a = radians(u_angle);
  // Local frame: y along the starting direction, x across it (the wedge is symmetric)
  vec2 p = vec2(abs(q.x * cos(a) - q.y * sin(a)), q.x * sin(a) + q.y * cos(a));
  float spread = PI * progress;
  // (float PI rounds above pi, so sin(spread) would turn slightly negative at the end)
  vec2 hand = vec2(max(sin(spread), 0.0), cos(spread));
  // Signed distance to the wedge: the nearest hand, negative inside
  float l = length(p);
  float d = length(p - hand * clamp(dot(p, hand), 0.0, l)) * sign(hand.y * p.x - hand.x * p.y);
  // The soft edge keeps an even width and slides from outside to inside the hands,
  // so nothing is revealed at the start and everything is at the end
  float s = max(u_smoothness, 0.001);
  float m = 1.0 - smoothstep(0.0, s, d + s * (1.0 - progress));
  return mix(getFromColor(uv), getToColor(uv), m);
}
