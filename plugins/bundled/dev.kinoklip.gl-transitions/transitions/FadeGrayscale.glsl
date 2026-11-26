// Author: gre
// License: MIT
// Description: A crossfade through grayscale
// Tags: fade, grayscale, color

 
vec3 grayscale (vec3 color) {
  return vec3(0.2126*color.r + 0.7152*color.g + 0.0722*color.b);
}
 
vec4 transition (vec2 uv) {
  vec4 fc = getFromColor(uv);
  vec4 tc = getToColor(uv);
  return mix(
    mix(vec4(grayscale(fc.rgb), fc.a), fc, smoothstep(1.0-u_intensity, 0.0, progress)),
    mix(vec4(grayscale(tc.rgb), tc.a), tc, smoothstep(    u_intensity, 1.0, progress)),
    progress);
}
