#version 460 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aColor;
layout (location = 2) in vec2 aTexCoord;
layout (location = 3) in vec3 aNormal;

// model: object space -> world space (the object's position/rotation/scale)
// view: world space -> camera space (where the camera is and looks)
// projection: camera space -> clip space (perspective + aspect ratio)
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;
// Turns object-space normals into world space. It's the inverse-transpose of
// the model matrix, so non-uniform scale doesn't bend normals off the surface.
uniform mat3 uNormalMatrix;

out vec3 vWorldPos;
out vec3 vNormal;
out vec3 vColor;
out vec2 vTexCoord;

void main()
{
    vec4 worldPos = uModel * vec4(aPosition, 1.0);
    vWorldPos = worldPos.xyz;
    vNormal = uNormalMatrix * aNormal;
    vColor = aColor;
    vTexCoord = aTexCoord;
    gl_Position = uProjection * uView * worldPos;
}
