#version 440

// Fisheye dewarp for ceiling-mounted 180-degree lenses (equidistant projection, optical
// axis pointing straight down). One shader, several views chosen by `mode`:
//   0  raw fisheye (passthrough)
//   1  virtual PTZ: a perspective view looking at azimuth `pan`, `tilt` from straight down
//   2  panorama: a strip from azimuth phi0 over `span`, elevation eTop..eBot above horizon
// The maths was checked against real stills before being written here (see DESIGN.md).
//
// The lens circle is (cx, cy) with radius `rad`, as fractions of the frame side.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(binding = 1) uniform sampler2D source;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float mode;
    float pan;       // PTZ: azimuth around the vertical axis (radians)
    float tilt;      // PTZ: angle from straight down (radians)
    float tanHalf;   // PTZ: tan(fov / 2), horizontal
    float aspect;    // PTZ: tile height / width
    float phi0;      // panorama: azimuth at the left edge (radians)
    float span;      // panorama: azimuth covered (radians)
    float eTop;      // panorama: elevation above the horizon at the top edge (radians)
    float eBot;      // panorama: ... at the bottom edge
    float cx;
    float cy;
    float rad;
};

const float PI = 3.14159265359;

void main() {
    vec2 uv = qt_TexCoord0;
    if (mode < 0.5) {
        fragColor = texture(source, uv) * qt_Opacity;
        return;
    }

    vec3 ray;
    if (mode < 1.5) {
        vec3 f = vec3(sin(tilt) * cos(pan), sin(tilt) * sin(pan), cos(tilt));
        vec3 upWorld = vec3(0.0, 0.0, -1.0);           // towards the ceiling
        vec3 right = normalize(cross(f, upWorld));
        vec3 up = cross(right, f);
        float u = uv.x * 2.0 - 1.0;
        float v = uv.y * 2.0 - 1.0;
        ray = normalize(f + right * (u * tanHalf) - up * (v * tanHalf * aspect));
    } else {
        float a = phi0 + span * uv.x;
        float e = eTop + (eBot - eTop) * uv.y;
        ray = vec3(cos(e) * cos(a), cos(e) * sin(a), -sin(e));
    }

    float theta = acos(clamp(ray.z, -1.0, 1.0));      // angle from the optical axis
    if (theta > PI * 0.5) {                            // behind the lens's 180 degrees
        fragColor = vec4(0.0, 0.0, 0.0, 1.0) * qt_Opacity;
        return;
    }
    float phi = atan(ray.y, ray.x);
    float r = theta / (PI * 0.5) * rad;
    vec2 st = vec2(cx, cy) + r * vec2(cos(phi), sin(phi));
    fragColor = texture(source, st) * qt_Opacity;
}
