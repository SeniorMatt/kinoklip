// Author: gre
// License: MIT
// Description: The frame is cut into 3D prisms that turn in a cascade, like a rotating billboard, from the outgoing image on one face to the incoming one on the next
// Tags: 3d, rotate, stripes

const float PI = 3.141592653589793;
const float EDGE = 0.002;

vec4 transition (vec2 uv) {
  float n = float(u_slats);
  // a: the axis across the prisms, b: the axis they turn around
  float a = u_horizontal ? uv.y : uv.x;
  float b = u_horizontal ? uv.x : uv.y;
  // the prisms' half width in frame heights, and the perspective from their depth
  float hw = 0.5 / n * (u_horizontal ? 1.0 : ratio);
  float k = min(u_perspective * hw, 0.5);
  float bScale = u_horizontal ? ratio : 1.0;
  // a turning prism is wider than its strip: keep the two nearest faces
  vec4 near1 = vec4(0.0), near2 = vec4(0.0);
  float cover1 = 0.0, cover2 = 0.0;
  float z1 = -2.0, z2 = -2.0;
  for (int d = -1; d <= 1; d++) {
    float i = floor(a * n) + float(d);
    if (i < 0.0 || i > n - 1.0) continue;
    float order = (u_horizontal ? n - 1.0 - i : i) / (n - 1.0);
    float t = clamp((progress - u_stagger * order) / (1.0 - u_stagger), 0.0, 1.0);
    t = mix(t, t * t * (3.0 - 2.0 * t), 0.7);
    float c = cos(0.5 * PI * t);
    float s = sin(0.5 * PI * t);
    // soft edges while turning (sharp at rest, to keep the first and last frames exact)
    float edge = EDGE * min(1.0, 20.0 * s * c) + 1e-6;
    // x: position across the strip, -1 to 1; the prism's square section has its faces at distance 1
    float x = 2.0 * (a * n - i) - 1.0;
    for (int f = 0; f < 2; f++) {
      // f = 0: the outgoing face turning away, f = 1: the incoming face turning in
      float cf = f == 0 ? c : s;
      if (cf < 1e-4) continue;
      // u: position on the face, z: depth (1 for a face at rest)
      float u = f == 0 ? (x + s) / c : (x - c) / s;
      float z = f == 0 ? u * s + c : s - u * c;
      if (abs(u) > 1.0 || z <= z2) continue;
      float scale = 1.0 - k * (z - 1.0);
      float y = (b - 0.5) * scale + 0.5;
      if (y < 0.0 || y > 1.0) continue;
      // soften the outer edges only: the faces meet at the prism's corner
      float outer = f == 0 ? 1.0 + u : 1.0 - u;
      float cover = clamp(outer * hw * cf / edge, 0.0, 1.0)
        * clamp(min(y, 1.0 - y) * bScale / scale / edge, 0.0, 1.0);
      float sa = (i + 0.5 + 0.5 * u) / n;
      vec2 p = u_horizontal ? vec2(y, sa) : vec2(sa, y);
      vec4 img = f == 0 ? getFromColor(p) : getToColor(p);
      float lambert = clamp(f == 0 ? c - 0.6 * s : s + 0.6 * c, 0.0, 1.0);
      vec4 color = vec4(img.rgb * mix(1.0 - u_shading, 1.0, lambert), img.a);
      if (z > z1) {
        near2 = near1; cover2 = cover1; z2 = z1;
        near1 = color; cover1 = cover; z1 = z;
      } else {
        near2 = color; cover2 = cover; z2 = z;
      }
    }
  }
  return mix(mix(u_backColor, near2, cover2), near1, cover1);
}
