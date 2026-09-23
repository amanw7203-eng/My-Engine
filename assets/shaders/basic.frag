#version 330 core
in vec3 vColor;
in vec2 vTexCoord;

// Which texture unit to read from; set with shader.SetInt("uTexture", slot).
uniform sampler2D uTexture;
// Per-object color multiplier; white leaves the texture unchanged.
uniform vec3 uTint;

out vec4 FragColor;

void main()
{
    // Vertex color and uTint both act as tints on top of the texture.
    FragColor = texture(uTexture, vTexCoord) * vec4(vColor * uTint, 1.0);
}
