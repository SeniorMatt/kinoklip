// Author: Boundless <info@boundless-beta.com>
// License: MIT
// Description: The image breaks into a grid of tiles that swirl along Lissajous curves, in a Scanimate style, and flip one by one to the incoming image before settling back
// Tags: tiles, grid, motion, retro

const vec2 grid = vec2(10.,10.);
const float TAU = 6.283185307;

vec4 transition (vec2 uv) {
  // The tiles leave and come back with zero speed, so the image does not break apart in the first frame
  float p = 1.-pow(1.-smoothstep(0.,1.,1.-abs(1.-2.*progress)),3.);
  // Find the last tile that covers this pixel (looping backwards, the first hit), then sample the images once
  for (float jBig = grid.y-1.; jBig >= 0.; jBig--) {
    for (float iBig = grid.x-1.; iBig >= 0.; iBig--) {
      float i = iBig/grid.x;
      float j = jBig/grid.y;
      float k = i/grid.y+j; // order of the tile along the curve
      vec2 uv0 = uv + vec2(i,j) - 0.5 + 0.5/grid + vec2(cos(k*TAU*(vec2(u_freqX, u_freqY)).x+progress*6.*u_speed),sin(k*TAU*(vec2(u_freqX, u_freqY)).y+progress*6.*(1.+u_offset)*u_speed))*u_zoom/2.;
      uv0 = mix(uv, uv0, p);
      if (uv0.x >= i && uv0.x < i+1./grid.x && uv0.y >= j && uv0.y < j+1./grid.y) {
        float tileMix = clamp(progress*((1.+u_fade)*2.*progress)-u_fade+k*u_fade, 0., 1.);
        return mix(getFromColor(uv0), getToColor(uv0), tileMix);
      }
    }
  }
  return vec4(0.); // between the tiles: transparent
}
