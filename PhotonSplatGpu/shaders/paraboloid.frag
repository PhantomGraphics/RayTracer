#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) flat in vec4 normalId;
layout(location=1) flat in float planeOffset;
layout(location=2) flat in vec3 vertexA;
layout(location=3) flat in vec3 vertexB;
layout(location=4) flat in vec3 vertexC;
layout(location=0) out vec4 outputNormalId;
void main() {
    vec2 uv=2*gl_FragCoord.xy/camera.dimensions.xy-1;
    float r2=dot(uv,uv);
    if(r2>1) discard;
    vec3 direction=(2*uv.x*camera.right.xyz-2*uv.y*camera.up.xyz+(1-r2)*camera.forward.xyz)/(1+r2);
    float denominator=dot(normalId.xyz,direction);
    if(abs(denominator)<1e-10) discard;
    // Plane depth of the rasterized primitive, not interpolated vertex range.
    float distance=planeOffset/denominator;
    if(distance<camera.projection.x || distance>=camera.projection.y) discard;
    // Reject chord approximation extending beyond the actual planar patch.
    vec3 e0=vertexB-vertexA,e1=vertexC-vertexA,p=direction*distance-vertexA;
    float a=dot(e0,e0),b=dot(e0,e1),c=dot(e1,e1),d=dot(p,e0),e=dot(p,e1);
    float determinant=a*c-b*b;
    if(determinant<=0) discard;
    vec2 bary=vec2(c*d-b*e,a*e-b*d)/determinant;
    if(bary.x< -1e-5 || bary.y< -1e-5 || bary.x+bary.y>1.00001) discard;
    gl_FragDepth=(distance-camera.projection.x)/(camera.projection.y-camera.projection.x);
    outputNormalId=normalId;
}
