// Author: gre
// License: MIT
// Description: The image twists like a ribbon around its horizontal center line, the twist travelling smoothly across its width, the outgoing image on its front and the incoming one on its back
// Tags: 3d, flip, rotate

const float PI = 3.141592653589793;
const float EDGE = 0.002;

vec4 transition (vec2 uv) {
  // q.x: along the ribbon, the way the twist travels; q.y: across the ribbon
  vec2 q = u_vertical ? vec2(1.0 - uv.y, uv.x) : uv;
  float halfSize = u_vertical ? 0.5 * ratio : 0.5;
  float t = smoothstep(0.0, 1.0, (progress * (1.0 + u_spread) - q.x) / u_spread);
  // the visible face (outgoing front, incoming back) and its angle from its rest position
  bool front = t < 0.5;
  float angle = PI * (front ? t : t - 1.0);
  float c = cos(angle);
  float s = sin(angle);
  // w: screen position across the ribbon, -1 to 1 at rest; v: the matching position on the face
  // a point v of the face goes to v * c across and v * s in depth, nearer points look bigger
  float w = 2.0 * q.y - 1.0;
  float den = c + u_perspective * w * s;
  if (den <= 0.0) return u_backColor;
  float v = w / den;
  if (abs(v) >= 1.0) return u_backColor;
  // soft edges while twisting (sharp at rest, to keep the first and last frames exact)
  float edge = max(EDGE * min(1.0, 20.0 * abs(s)), 1e-4);
  float top = c / (1.0 - u_perspective * s);
  float bottom = -c / (1.0 + u_perspective * s);
  float cover = clamp(min(top - w, w - bottom) * halfSize / edge, 0.0, 1.0);
  vec2 p = vec2(q.x, 0.5 + 0.5 * v);
  p = u_vertical ? vec2(p.y, 1.0 - p.x) : p;
  vec4 img = front ? getFromColor(p) : getToColor(p);
  float lambert = clamp(c - 0.25 * s, 0.0, 1.0);
  vec4 color = vec4(img.rgb * mix(1.0 - u_shading, 1.0, lambert), img.a);
  return mix(u_backColor, color, cover);
}
