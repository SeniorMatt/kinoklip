// Author: Adrian Purser
// License: MIT
// Description: The outgoing image drops out of the frame with a few bounces, revealing the incoming one
// Tags: bounce, slide, vertical, shadow

const float PI = 3.14159265358;

vec4 transition (vec2 uv) {
  float time = progress;
  float stime = sin(time * PI / 2.);
  float phase = time * PI * u_bounces;
  float y = (abs(cos(phase))) * (1.0 - stime);
  float d = uv.y - y;
  vec4 to = getToColor(uv);
  return mix(
    mix(
      to,
      vec4(u_shadow_colour.rgb, 1.0), // shadow_colour.a is in the amount: the shadow darkens, it does not make the image transparent
      step(d, u_shadow_height) * (1. - mix(
        ((d / u_shadow_height) * u_shadow_colour.a) + (1.0 - u_shadow_colour.a),
        1.0,
        smoothstep(0.95, 1., progress)
      )) * to.a // cast on the incoming image, not on its transparent areas
    ),
    getFromColor(vec2(uv.x, uv.y + (1.0 - y))),
    step(d, 0.0)
  );
}
