// Author: gre
// License: MIT
// Description: A crossfade in which each color channel changes at a different time
// Tags: fade, color

// Usage: fromStep and toStep must be in [0.0, 1.0] range 
// and all(fromStep) must be < all(toStep)

vec4 transition (vec2 uv) {
  vec4 a = getFromColor(uv);
  vec4 b = getToColor(uv);
  return mix(a, b, smoothstep((vec4(u_fromStepX, u_fromStepY, u_fromStepZ, u_fromStepW)), (vec4(u_toStepX, u_toStepY, u_toStepZ, u_toStepW)), vec4(progress)));
}
