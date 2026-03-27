#version 330
in vec2 fragTexCoord;
out vec4 finalColor;
uniform sampler2D texture0;
uniform float time;

void main() {
    float strength = 0.002;
    float noise = sin(time * 30.0) * cos(time * 20.0);
    vec2 uv = fragTexCoord;
    
    // Horizontal pixel vibration
    if (abs(noise) > 0.98) uv.x += noise * strength;

    // Red/Blue Anime shift
    float r = texture(texture0, uv + vec2(0.0015, 0.0)).r;
    float g = texture(texture0, uv).g;
    float b = texture(texture0, uv - vec2(0.0015, 0.0)).b;

    finalColor = vec4(r, g, b, 1.0);
}