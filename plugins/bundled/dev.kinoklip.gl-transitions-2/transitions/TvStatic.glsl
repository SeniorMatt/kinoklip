// Author: Brandon Anzaldi
// License: MIT
// Description: Cuts to full-screen TV static, then to the incoming image
// Tags: static, noise, retro

// http://byteblacksmith.com/improvements-to-the-canonical-one-liner-glsl-rand-for-opengl-es-2-0/
highp float noise(vec2 co)
{
    highp float a = 12.9898;
    highp float b = 78.233;
    highp float c = 43758.5453;
    highp float dt= dot(co.xy * progress, vec2(a, b));
    highp float sn= mod(dt,3.14);
    return fract(sin(sn) * c);
}

vec4 transition(vec2 p) {
  if (progress < u_offset) {
    return getFromColor(p);
  } else if (progress > (1.0 - u_offset)) {
    return getToColor(p);
  } else {
    return vec4(vec3(noise(p)), max(getFromColor(p).a, getToColor(p).a)); // static where the images are
  }
}
