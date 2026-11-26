// Author: Fernando Kuteken
// License: MIT
// Description: The outgoing image closes into a circle over a background color, then the incoming one opens from it
// Tags: circle, iris, shape
// Preview: 0.4

vec4 transition (vec2 uv) {
  
  float distance = length(uv - (vec2(u_center_x, u_center_y)));
  float radius = sqrt(8.0) * abs(progress - 0.5);
  
  if (distance > radius) {
    return u_back_color;
  }
  else {
    if (progress < 0.5) return getFromColor(uv);
    else return getToColor(uv);
  }
}
