#version 330 core
in vec3 vColor;
in vec2 vTexCoord;

// Which texture unit to read from; set with shader.SetInt("uTexture", slot).
uniform sampler2D uTexture;

out vec4 FragColor;

void main()
{
    // Vertex color acts as a tint: white leaves the texture unchanged.
    FragColor = texture(uTexture, vTexCoord) * vec4(vColor, 1.0);
}
