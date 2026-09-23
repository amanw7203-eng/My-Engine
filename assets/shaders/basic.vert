#version 330 core
layout (location = 0) in vec3 aPosition;
layout (location = 1) in vec3 aColor;

// model: object space -> world space (the object's position/rotation/scale)
// view: world space -> camera space (where the camera is and looks)
// projection: camera space -> clip space (perspective + aspect ratio)
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

out vec3 vColor;

void main()
{
    vColor = aColor;
    gl_Position = uProjection * uView * uModel * vec4(aPosition, 1.0);
}
