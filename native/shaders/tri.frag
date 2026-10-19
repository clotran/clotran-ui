#version 450
// a triangle lit as clotran-plot's SVG cells are: metal.m's fs_tri
layout(push_constant) uniform U { float az, el, kx, ky, lx, ly, lz, pad; } u;
layout(location = 0) in vec3 c;
layout(location = 1) in vec3 p;
layout(location = 0) out vec4 o;
void main() {
  vec3 n = cross(dFdx(p), dFdy(p));
  float l = length(n);
  float k = 1.0;
  if (l > 0.0) { n = n / l; if (n.z < 0.0) n = -n; k = max(0.0, dot(n, vec3(u.lx, u.ly, u.lz))); }
  o = vec4(c * (0.35 + 0.65 * k), 1.0);
}
