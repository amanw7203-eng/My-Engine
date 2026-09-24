#version 330 core
in vec3 vWorldPos;
in vec3 vNormal;
in vec3 vColor;
in vec2 vTexCoord;

// Which texture unit to read from; set with shader.SetInt("uTexture", slot).
uniform sampler2D uTexture;
// Per-object color multiplier; white leaves the texture unchanged.
uniform vec3 uTint;

// Per-object material: how strong the highlight is, and how tight
// (higher shininess = smaller, sharper highlight).
uniform float uSpecularStrength;
uniform float uShininess;

// One directional light (like the sun): every surface sees it coming from
// the same direction. uLightDir points from the light towards the scene.
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbientColor;

// Camera position in world space, for the view direction.
uniform vec3 uViewPos;

// Shadows. uShadowMap holds, for each texel, the depth of the closest
// surface the light sees there; uLightSpace maps world space onto it.
uniform bool uShadowsEnabled;
uniform sampler2D uShadowMap;
uniform mat4 uLightSpace;
// How far to push the lookup point out along the normal (world units, about
// one shadow texel). Without it a surface can shadow itself in a striped
// pattern ("shadow acne"), because its own depth is stored with limited
// precision.
uniform float uShadowNormalOffset;
// Shadows fade out as they approach this distance from the camera, so
// there is no hard line where the shadow map stops.
uniform float uShadowDistance;

out vec4 FragColor;

// 1.0 = fully lit, 0.0 = fully in shadow.
float ShadowFactor(vec3 N, float NdotL)
{
    if (!uShadowsEnabled)
        return 1.0;

    // 0 near the camera, rising to 1 over the last 20% of the distance.
    float fade = smoothstep(0.8 * uShadowDistance, uShadowDistance, length(uViewPos - vWorldPos));
    if (fade >= 1.0)
        return 1.0;

    vec4 lightClip = uLightSpace * vec4(vWorldPos + N * uShadowNormalOffset, 1.0);
    // Clip space -1..1 -> texture coordinates and depth 0..1.
    vec3 p = lightClip.xyz / lightClip.w * 0.5 + 0.5;
    // Past the far end of the light's box: nothing there casts a shadow.
    if (p.z > 1.0)
        return 1.0;

    // A small extra bias, larger on surfaces at a grazing angle to the light.
    float bias = mix(0.0015, 0.0003, NdotL);

    // Percentage-closer filtering: test a 3x3 block of texels and average,
    // which softens the stair-stepped shadow edges.
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float lit = 0.0;
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float closest = texture(uShadowMap, p.xy + vec2(x, y) * texel).r;
            lit += p.z - bias > closest ? 0.0 : 1.0;
        }
    }
    return mix(lit / 9.0, 1.0, fade);
}

void main()
{
    vec3 albedo = texture(uTexture, vTexCoord).rgb * vColor * uTint;

    // Interpolation shortens normals between vertices, so re-normalize.
    // Back faces (e.g. the far side of a quad) flip the normal to face us.
    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing)
        N = -N;
    vec3 L = normalize(-uLightDir);             // surface -> light
    vec3 V = normalize(uViewPos - vWorldPos);   // surface -> camera
    vec3 H = normalize(L + V);                  // halfway vector (Blinn)

    // Ambient: a flat fill so faces turned away from the light aren't black.
    vec3 ambient = uAmbientColor * albedo;

    // Diffuse (Lambert): brightest when the surface faces the light.
    float NdotL = max(dot(N, L), 0.0);
    vec3 diffuse = NdotL * uLightColor * albedo;

    // Specular (Blinn-Phong): brightest when the halfway vector lines up with
    // the normal. Skipped on surfaces facing away so highlights don't leak.
    float spec = NdotL > 0.0 ? pow(max(dot(N, H), 0.0), uShininess) : 0.0;
    vec3 specular = uSpecularStrength * spec * uLightColor;

    // Shadow blocks the direct light only; ambient still fills it in.
    float shadow = NdotL > 0.0 ? ShadowFactor(N, NdotL) : 1.0;

    FragColor = vec4(ambient + shadow * (diffuse + specular), 1.0);
}
