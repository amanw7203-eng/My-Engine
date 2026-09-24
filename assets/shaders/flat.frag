#version 460 core

// One solid color for the whole mesh (the selection outline).
uniform vec3 uColor;

out vec4 FragColor;

void main()
{
    FragColor = vec4(uColor, 1.0);
}
