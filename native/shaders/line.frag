#version 450
// a line in its own colour: metal.m's fs_line
layout(location = 0) in vec3 c;
layout(location = 1) in vec3 p;
layout(location = 0) out vec4 o;
void main() { o = vec4(c, 1.0); }
