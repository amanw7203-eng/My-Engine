#version 330 core
layout (location = 0) in vec3 aPosition;

// Light's view + projection combined, and the object's model matrix.
uniform mat4 uLightSpace;
uniform mat4 uModel;

void main()
{
    gl_Position = uLightSpace * uModel * vec4(aPosition, 1.0);
}
