// Author: gre
// License: MIT
// Description: The incoming image is printed like text on a terminal: line after line, the whole frame jolts up one row, pushing the outgoing image up, and a block cursor types the new row in cell by cell from left to right over a dimmed preview of the line
// Tags: text, terminal, typing, scroll, retro

const float FEED = 0.3;

vec4 transition(vec2 uv) {
  float n = float(u_rows);
  float u = progress * n;
  float k = min(floor(u), n - 1.0);
  float f = u - k;
  float s = clamp(f / FEED, 0.0, 1.0);
  // snaps a little past the line and settles back, like a platen jolting round
  float feed = s < 0.55 ? 1.06 * smoothstep(0.0, 0.55, s) : 1.06 - 0.06 * smoothstep(0.55, 1.0, s);
  float y = 1.0 - uv.y + (k + feed) / n;
  if (y < 1.0) return getFromColor(vec2(uv.x, 1.0 - y));

  // mirrored: the last feed's overshoot looks past the bottom of the image
  vec4 b = getToColor(vec2(uv.x, abs(2.0 - y)));
  float row = min(floor((y - 1.0) * n), n - 1.0);
  float c = float(u_cells);
  float typed = clamp((f - FEED) / (1.0 - FEED), 0.0, 1.0);
  float head = row < k ? c + 1.0 : (row > k ? -1.0 : floor(typed * (c + 1.0)));
  float cell = floor(uv.x * c);
  vec4 col = cell < head ? b : vec4(b.rgb * 0.16, b.a);
  float ly = fract((y - 1.0) * n);
  float lit = step(0.5, s);
  float box = (cell == head ? 1.0 : 0.0) * step(0.1, ly) * step(ly, 0.9) * lit;
  return mix(col, u_cursor, 0.9 * box * u_cursor.a);
}
