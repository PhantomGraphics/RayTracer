#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(set=0,binding=3) uniform sampler2D receiverDepth;
layout(set=0,binding=4) uniform sampler2D receiverNormalId;
layout(location=0) flat in uint photonId;
layout(location=0) out vec4 radiance;
void main() {
    ivec2 pixel=ivec2(gl_FragCoord.xy);
    vec4 normalId=texelFetch(receiverNormalId,pixel,0);
    if(normalId.w<0.5) discard;
    float depth=texelFetch(receiverDepth,pixel,0).r;
    float z=camera.projection.w/(depth-camera.projection.z);
    vec2 ndc=2.0*gl_FragCoord.xy/camera.dimensions.xy-1.0;
    vec3 position=camera.eye.xyz+camera.forward.xyz*z
        +camera.right.xyz*(ndc.x*z*camera.projection.x*camera.projection.y)
        -camera.up.xyz*(ndc.y*z*camera.projection.x);
    vec3 normal=normalize(normalId.xyz);
    if(dot(normal,camera.eye.xyz-position)<=0) discard;
    Photon p=photons[photonId]; vec3 delta=p.position.xyz-position;
    float radius=camera.dimensions.z;
    if(dot(delta,delta)>radius*radius || dot(p.normal.xyz,normal)<0.9
        || dot(p.direction.xyz,normal)>=0 || abs(dot(delta,normal))>radius*0.02) discard;
    const float pi=3.14159265358979323846;
    vec3 reflectance=triangles[int(normalId.w)-1].reflectance.xyz;
    radiance=vec4(reflectance*p.power.xyz/(pi*pi*radius*radius),0);
}
