// Author: gre
// License: MIT
// Description: A band of intense light sweeps across the frame like a glint on glass, blooming the highlights of both images, and leaves the incoming image behind it
// Tags: wipe, light, glow, glint

const int TAPS = 12;

vec2 mirror(vec2 v) {
  return 1.0 - abs(1.0 - mod(v, 2.0));
}

// a gentle ease in and out, which does not linger at the ends
float ease(float t) {
  return 0.5 * (t + t * t * (3.0 - 2.0 * t));
}

// both images are always sampled, so that no texture read sits in a divergent branch
vec4 frame(vec2 st, float m) {
  return mix(getFromColor(st), getToColor(st), m);
}

vec3 highlight(vec4 c) {
  float lum = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
  return c.rgb * c.a * smoothstep(0.45, 1.0, lum);
}

vec4 transition(vec2 uv) {
  vec2 dir = length((vec2(u_direction_x, u_direction_y))) > 0.0 ? normalize((vec2(u_direction_x, u_direction_y))) : vec2(1.0, 0.0);
  vec2 p = (uv - 0.5) * vec2(ratio, 1.0);
  float e = 0.5 * (abs(dir.x) * ratio + abs(dir.y));
  float w = max(u_width, 0.001);
  float s = 0.35 * w;
  // the band starts and ends outside of the frame, with the switch out of sight
  float margin = s + 2.0 * w;
  float pos = -e - margin + 2.0 * (e + margin) * ease(progress);
  // signed distance to the middle of the band: < 0 has been swept
  float d = dot(p, dir) - pos;
  float m = 1.0 - smoothstep(-s, s, d);

  float env = smoothstep(0.0, 0.2, progress) * (1.0 - smoothstep(0.8, 1.0, progress));
  float core = exp(-d * d / (w * w));
  float g = (d + 2.2 * w) / (0.3 * w);
  float glint = 0.45 * exp(-g * g);
  float halo = 0.35 * exp(-abs(d) / (5.0 * w));
  float band = (core + glint + halo) * env * u_intensity;
  float glow = (core + glint) * env * u_intensity;

  vec4 c = frame(uv, m);

  vec3 bloom = vec3(0.0);
  float spread = min(3.0 * w, 0.12);
  for (int i = 0; i < TAPS; i++) {
    float t = (float(i) + 0.5) / float(TAPS) * 2.0 - 1.0;
    vec2 st = mirror((p + dir * (t * spread)) / vec2(ratio, 1.0) + 0.5);
    float dq = dot((st - 0.5) * vec2(ratio, 1.0), dir) - pos;
    bloom += (1.0 - abs(t)) * highlight(frame(st, 1.0 - smoothstep(-s, s, dq)));
  }
  bloom /= 0.5 * float(TAPS);

  vec3 hue = mix(u_color.rgb, vec3(1.0), core * core);
  float lum = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
  // the light lifts the image, the bright parts more: exposure that rolls off softly to white, never clipping
  vec3 light = hue * band * (0.3 + 0.9 * lum) + hue * bloom * glow * 4.0 + hue * core * env * u_intensity * 1.6;
  vec3 rgb = 1.0 - (1.0 - c.rgb) * exp(-light);
  // light adds to the image, not to its alpha
  return vec4(rgb, c.a);
}
