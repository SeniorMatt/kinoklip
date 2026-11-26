// Author: gre
// License: MIT
// Description: A wipe whose edge is a row of sharp, regular triangular teeth sweeps across the frame, or two toothed edges that start interlocked part from the middle like jaws opening
// Tags: wipe, shape, directional, zigzag

const float AA = 0.0015;

// signed distance to a zigzag line of period w and peak-to-peak height h, centered on y = 0,
// its tips pointing up at x = 0; positive above it
float zigzag(vec2 q, float w, float h) {
  // fold into half a tooth: the slope from the bottom of a notch to the tip of a tooth
  float x = abs(fract(q.x / w) - 0.5) * w;
  vec2 a = vec2(0.0, -0.5 * h);
  vec2 ab = vec2(0.5 * w, h);
  vec2 aq = vec2(x, q.y) - a;
  float d = length(aq - ab * clamp(dot(aq, ab) / dot(ab, ab), 0.0, 1.0));
  return q.y > h * (2.0 * x / w - 0.5) ? d : -d;
}

vec4 transition(vec2 uv) {
  vec4 a = getFromColor(uv);
  vec4 b = getToColor(uv);

  vec2 dir = length((vec2(u_direction_x, u_direction_y))) > 0.0 ? normalize((vec2(u_direction_x, u_direction_y))) : vec2(1.0, 0.0);
  // measured in square pixels, so the teeth are not stretched
  vec2 p = (uv - 0.5) * vec2(ratio, 1.0);
  float e = 0.5 * (abs(dir.x) * ratio + abs(dir.y));
  float w = 0.5 * (abs(dir.y) * ratio + abs(dir.x));
  float period = 2.0 * w / float(u_teeth < 1 ? 1 : u_teeth);
  // position across the edge, with the notches on the sides of the frame so that it shows
  // whole teeth, and along the direction
  vec2 q = vec2(dot(p, vec2(-dir.y, dir.x)) + w + 0.5 * period, dot(p, dir));
  float h = max(u_depth, 0.0) * period;
  // the soft edge never gets wide enough to blur small teeth away
  float s = max(min(u_smoothness, 0.25 * period), AA);
  float t = mix(progress, progress * progress * (3.0 - 2.0 * progress), 0.35);

  float d;
  if (u_jaws) {
    // the two edges are the same zigzag, interlocked at the start (crossed by the soft
    // edge, measured across the slopes of the teeth), then moving apart until each one
    // has left the frame on its side
    float overlap = s * sqrt(1.0 + 4.0 * h * h / (period * period));
    float o = mix(-overlap, e + 0.5 * h + s, t);
    d = max(zigzag(q - vec2(0.0, o), period, h), -zigzag(q + vec2(0.0, o), period, h));
  } else {
    // the edge starts fully ahead of the frame and ends fully past it
    float c = mix(-e - 0.5 * h - s, e + 0.5 * h + s, t);
    d = zigzag(q - vec2(0.0, c), period, h);
  }
  // > 0 where the incoming image shows
  float m = 1.0 - smoothstep(-s, s, d);

  // mixed premultiplied, so transparent images do not bleed their color
  vec4 c = mix(vec4(a.rgb * a.a, a.a), vec4(b.rgb * b.a, b.a), m);
  return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}
