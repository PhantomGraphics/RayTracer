struct Triangle { vec4 a; vec4 b; vec4 c; vec4 normal; vec4 reflectance; };
struct Photon { vec4 position; vec4 direction; vec4 normal; vec4 power; };
layout(set=0,binding=0,std140) uniform Camera {
    vec4 eye; vec4 right; vec4 up; vec4 forward;
    vec4 projection; // tan(fov/2), aspect, depth A, depth B
    vec4 dimensions; // width, height, radius, near plane
} camera;
layout(set=0,binding=1,std430) readonly buffer Triangles { Triangle triangles[]; };
layout(set=0,binding=2,std430) readonly buffer Photons { Photon photons[]; };
