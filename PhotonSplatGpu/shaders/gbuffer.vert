#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) flat out vec4 normalId;
void main() {
    int id=gl_VertexIndex/3;
    Triangle t=triangles[id];
    vec3 world=gl_VertexIndex%3==0?t.a.xyz:(gl_VertexIndex%3==1?t.b.xyz:t.c.xyz);
    vec3 delta=world-camera.eye.xyz;
    float z=dot(delta,camera.forward.xyz);
    gl_Position=vec4(dot(delta,camera.right.xyz)/(camera.projection.x*camera.projection.y),
        -dot(delta,camera.up.xyz)/camera.projection.x,camera.projection.z*z+camera.projection.w,z);
    normalId=vec4(t.normal.xyz,float(id+1));
}
