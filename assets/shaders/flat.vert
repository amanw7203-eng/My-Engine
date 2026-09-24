#version 460 core
layout (location = 0) in vec3 aPosition;

// Position only: used by passes that don't need lighting or textures
// (object picking, the selection outline).
uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main()
{
    gl_Position = uProjection * uView * uModel * vec4(aPosition, 1.0);
}
