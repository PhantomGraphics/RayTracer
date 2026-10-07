#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location=0) flat out uint photonId;
void main() {
    photonId=gl_InstanceIndex;
    vec3 delta=photons[photonId].position.xyz-camera.eye.xyz;
    vec3 p=vec3(dot(delta,camera.right.xyz),-dot(delta,camera.up.xyz),dot(delta,camera.forward.xyz));
    float r=camera.dimensions.z;
    vec2 lo=vec2(-1),hi=vec2(1);
    if(p.z+r<=camera.dimensions.w) { gl_Position=vec4(2,2,0,1); return; }
    if(p.z-r>camera.dimensions.w) {
        vec4 xx=vec4((p.x-r)/(p.z-r),(p.x-r)/(p.z+r),(p.x+r)/(p.z-r),(p.x+r)/(p.z+r));
        vec4 yy=vec4((p.y-r)/(p.z-r),(p.y-r)/(p.z+r),(p.y+r)/(p.z-r),(p.y+r)/(p.z+r));
        lo=vec2(min(min(xx.x,xx.y),min(xx.z,xx.w)),min(min(yy.x,yy.y),min(yy.z,yy.w)));
        hi=vec2(max(max(xx.x,xx.y),max(xx.z,xx.w)),max(max(yy.x,yy.y),max(yy.z,yy.w)));
        vec2 scale=vec2(camera.projection.x*camera.projection.y,camera.projection.x);
        lo=clamp(lo/scale-2.0/camera.dimensions.xy,vec2(-1),vec2(1));
        hi=clamp(hi/scale+2.0/camera.dimensions.xy,vec2(-1),vec2(1));
    }
    const vec2 corners[6]=vec2[](vec2(0,0),vec2(1,0),vec2(1,1),vec2(0,0),vec2(1,1),vec2(0,1));
    gl_Position=vec4(mix(lo,hi,corners[gl_VertexIndex]),0,1);
}
