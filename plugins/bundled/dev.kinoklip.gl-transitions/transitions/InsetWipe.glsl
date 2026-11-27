// Author: gre
// License: MIT
// Description: A rectangle anchored in a corner, on an edge or in the center grows until the incoming image fills the frame, revealing it in place without scaling it; in reverse, the outgoing image shrinks into the rectangle
// Tags: wipe, shape, box
// Preview: 0.5

const float AA = 0.001;

vec4 premultiplied(vec4 c) {
  return vec4(c.rgb * c.a, c.a);
}

vec4 transition(vec2 uv) {
  vec4 a = getFromColor(uv);
  vec4 b = getToColor(uv);

  vec2 an = clamp((vec2(u_anchorX, u_anchorY)), 0.0, 1.0);
  vec2 sq = vec2(ratio, 1.0);
  // the rectangle keeps the proportions that make it reach every far side of the frame
  // at once; measured in square pixels so its edge and border have an even width
  vec2 ext = max(an, 1.0 - an) * sq;
  vec2 d = abs(uv - an) * sq;
  float s = max(u_smoothness, AA);
  float bw = max(u_borderWidth, 0.0);

  // eased growth, from a rectangle whose border is not yet in the frame to one whose
  // soft edge has left it on every side
  float t = u_reverse ? 1.0 - progress : progress;
  float p = mix(t, t * t * (3.0 - 2.0 * t), 0.3);
  vec2 size = p * ext + mix(-(bw + s), s, p);
  // signed distance to the rectangle's edge, square-cornered, < 0 inside
  float e = max(d.x - size.x, d.y - size.y);

  vec4 inside = u_reverse ? a : b;
  vec4 c = premultiplied(u_reverse ? b : a);
  if (bw > 0.0) {
    // laid over the image outside the rectangle, so a translucent border tints it
    float m = 1.0 - smoothstep(bw - s, bw + s, e);
    c = premultiplied(u_borderColor) * m + c * (1.0 - u_borderColor.a * m);
  }
  c = mix(c, premultiplied(inside), 1.0 - smoothstep(-s, s, e));
  return c.a > 0.0 ? vec4(c.rgb / c.a, c.a) : vec4(0.0);
}
