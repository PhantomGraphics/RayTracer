#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) flat out vec4 normalId;
layout(location=1) flat out float planeOffset;
layout(location=2) flat out vec3 vertexA;
layout(location=3) flat out vec3 vertexB;
layout(location=4) flat out vec3 vertexC;
void main() {
    Triangle t=triangles[gl_VertexIndex/3];
    vec3 world=gl_VertexIndex%3==0?t.a.xyz:(gl_VertexIndex%3==1?t.b.xyz:t.c.xyz);
    vec3 delta=world-camera.eye.xyz;
    float denominator=length(delta)+dot(delta,camera.forward.xyz);
    if(denominator<=1e-12) gl_Position=vec4(2,2,0,1);
    else gl_Position=vec4(dot(delta,camera.right.xyz)/denominator,
        -dot(delta,camera.up.xyz)/denominator,0,1);
    normalId=vec4(t.normal.xyz,t.reflectance.w);
    planeOffset=dot(t.normal.xyz,t.a.xyz-camera.eye.xyz);
    vertexA=t.a.xyz-camera.eye.xyz; vertexB=t.b.xyz-camera.eye.xyz; vertexC=t.c.xyz-camera.eye.xyz;
}
