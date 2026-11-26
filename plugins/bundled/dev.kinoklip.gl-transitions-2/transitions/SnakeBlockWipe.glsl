// Author: gre
// License: MIT
// Description: Blocks switch to the incoming image one at a time, row after row from the top (or column after column from the left), each line running the opposite way to the one before, like a snake winding across the frame
// Tags: wipe, blocks, grid, zigzag

vec4 transition(vec2 uv) {
  vec4 a = getFromColor(uv);
  vec4 b = getToColor(uv);

  vec2 g = max(vec2((ivec2(u_gridX, u_gridY))), 1.0);
  vec2 cell = min(floor(vec2(uv.x, 1.0 - uv.y) * g), g - 1.0);
  float lines = u_vertical ? g.x : g.y;
  float len = u_vertical ? g.y : g.x;
  float line = u_vertical ? cell.x : cell.y;
  float pos = u_vertical ? cell.y : cell.x;
  if (mod(line, 2.0) > 0.5) pos = len - 1.0 - pos;
  float order = line * len + pos;

  // block k fades in from k to k + w, on a timeline from 0 to n - 1 + w
  float n = lines * len;
  float w = max(u_smoothness, 0.001);
  float m = clamp((progress * (n - 1.0 + w) - order) / w, 0.0, 1.0);

  // mixed premultiplied, so transparent images do not bleed their color
  vec4 c = mix(vec4(a.rgb * a.a, a.a), vec4(b.rgb * b.a, b.a), m);
  return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}
