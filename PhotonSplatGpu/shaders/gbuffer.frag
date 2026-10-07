#version 450
layout(location=0) flat in vec4 normalId;
layout(location=0) out vec4 outputNormalId;
void main() { outputNormalId=normalId; }
