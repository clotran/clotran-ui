#version 450
// the 3D view's vertices: metal.m's vs, in GLSL for Vulkan (g3.h's camera)
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 col;
layout(push_constant) uniform U { float az, el, kx, ky, lx, ly, lz, pad; } u;
layout(location = 0) out vec3 c;
layout(location = 1) out vec3 p;
void main() {
  float X = pos.x * cos(u.az) - pos.y * sin(u.az);
  float Y = pos.x * sin(u.az) + pos.y * cos(u.az);
  float up = Y * sin(u.el) + pos.z * cos(u.el);
  float to = pos.z * sin(u.el) - Y * cos(u.el);
  // Vulkan's clip space has y down, Metal's up
  gl_Position = vec4(X * u.kx, -up * u.ky, 0.5 - 0.4 * to, 1.0);
  c = col;
  p = pos;
}
