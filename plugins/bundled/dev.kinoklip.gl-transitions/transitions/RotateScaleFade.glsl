// Author: Fernando Kuteken
// License: MIT
// Description: The outgoing image spins and zooms while fading to the incoming one, over a background color
// Tags: rotate, zoom, fade
// Preview: 0.15

#define PI 3.14159265359

vec4 transition (vec2 uv) {
  
  vec2 difference = uv - (vec2(u_center_x, u_center_y));
  vec2 dir = normalize(difference);
  float dist = length(difference);
  
  float angle = 2.0 * PI * u_rotations * progress;
  
  float c = cos(angle);
  float s = sin(angle);
  
  float currentScale = mix(u_scale, 1.0, 2.0 * abs(progress - 0.5));
  
  vec2 rotatedDir = vec2(dir.x  * c - dir.y * s, dir.x * s + dir.y * c);
  vec2 rotatedUv = (vec2(u_center_x, u_center_y)) + rotatedDir * dist / currentScale;
  
  if (rotatedUv.x < 0.0 || rotatedUv.x > 1.0 ||
      rotatedUv.y < 0.0 || rotatedUv.y > 1.0)
    return u_back_color;
    
  return mix(getFromColor(rotatedUv), getToColor(rotatedUv), progress);
}
