// GLSL of the GPU replacement for the inFamous 2 deferred lighting job (VKNativeLighting.cpp).
// Every pass is "#version 450", common, io and its own text. The offline harness that compares the passes with a
// reference implementation of the job reads the texts from this file and supplies its own io.
#pragma once

namespace vk::native_lighting::shaders
{
inline constexpr const char* common = R"GLSL(
// Tiled deferred lighting of inFamous 2 (the game's SPU job P071), shared declarations.
// The frame is 40x18 tiles, 32x40 pixels each at the game's 1280x720. The images can have any size (resolution
// scaling), so pixel sizes come from the input words. Up to 256 point and spot lights.
#define W int(data[24])
#define H int(data[25])
const int TILES_X = 40, TILES_Y = 18, TILES = TILES_X * TILES_Y;
const uint MASK_WORDS = 8u;   // one bit per light
const uint TILE_STRIDE = 16u; // words per tile: the light mask, then
const uint TILE_ANY = 8u;     // not 0 if the mask has a light
const uint TILE_LIT = 9u;     // not 0 once a pixel of the tile is inside the range of a light
const uint SKY = 0xffffffu;   // depth of a pixel nothing was drawn to

// Words 0-15: matrix from (x/1280, y/720, depth/2^24, 1) to view space, by columns, x and y being pixels of the
// game's frame. 16: light colour scale. 17: light count. 22: offset of the view-to-clip matrix. 24, 25: image size.
// 26, 27: 1 / image size. 28, 29: added to a pixel of the image before that division, so that its centre lands
// where the game's pixel grid has it (0 at 1280x720). From word 32: 16 words per light
// (position, kind, colour, inner and outer radius, cone gain, cone cosine, near distance, direction).
layout(std430, binding=0) readonly buffer Inputs { uint data[]; };
// Per block of pixels, a fifth of a tile's height (32x8 at 1280x720): minimum and maximum view-space position
const uint BLOCKS_PER_TILE = 5u;
layout(std430, binding=1) buffer BlockBounds { vec4 block_bounds[]; };
layout(std430, binding=2) buffer Tiles { uint tile_data[]; };

struct light_t
{
    vec4 position_radius;  // view-space position, outer radius
    vec4 colour_magnitude; // scaled colour, its length
    vec4 direction_kind;   // spot direction, kind (1 point, 2 spot)
    vec4 falloff;          // 1 / (outer - inner radius), cone gain, cone cosine, near distance
};
layout(std430, binding=3) buffer Lights { light_t lights[]; };

float parameter(uint i) { return uintBitsToFloat(data[i]); }
vec4 parameter_column(uint i) { return vec4(parameter(i), parameter(i + 1u), parameter(i + 2u), parameter(i + 3u)); }
uint light_count() { return min(data[17], MASK_WORDS * 32u); }
mat4 position_matrix() { return mat4(parameter_column(0u), parameter_column(4u), parameter_column(8u), parameter_column(12u)); }

vec3 view_position(mat4 m, ivec2 pixel, uint depth)
{
    vec4 p = m * vec4((float(pixel.x) + parameter(28u)) * parameter(26u), (float(pixel.y) + parameter(29u)) * parameter(27u), float(depth) * exp2(-24.0), 1.0);
    return p.xyz / p.w;
}

// Pixel x is in tile x * TILES_X / W and pixel y in block y * TILES_Y * BLOCKS_PER_TILE / H: the first pixel of each
int tile_left(int tile_x) { return (tile_x * W + TILES_X - 1) / TILES_X; }
int block_top(int block_y) { const int blocks = TILES_Y * int(BLOCKS_PER_TILE); return (block_y * H + blocks - 1) / blocks; }

// A pixel is in the range of a light when it is inside the outer radius and, for a spot light, inside the cone
bool in_range(light_t light, vec3 to_light, float distance_squared, float inverse_distance)
{
    float radius = light.position_radius.w;
    if (distance_squared >= radius * radius) return false;
    if (light.direction_kind.w != 2.0) return true;
    return dot(to_light, -light.direction_kind.xyz) * inverse_distance > light.falloff.z;
}
)GLSL";

inline constexpr const char* io = R"GLSL(
// The two G-buffer images as the game blits them for the job, and the two images the game samples afterwards
layout(binding=4) uniform sampler2D normals_texture;
layout(binding=5) uniform sampler2D depth_texture;
layout(binding=6, rgba8) uniform writeonly image2D light_image;
layout(binding=7, rgba8) uniform writeonly image2D specular_image;

// 24-bit depth in the A, R and G channels
uint depth24(ivec2 p)
{
    uvec3 b = uvec3(texelFetch(depth_texture, p, 0).arg * 255.0 + 0.5);
    return (b.x << 16u) | (b.y << 8u) | b.z;
}
// Material byte, then the normal
uvec4 normal_bytes(ivec2 p) { return uvec4(texelFetch(normals_texture, p, 0).argb * 255.0 + 0.5); }
void store(ivec2 p, uvec4 light_argb, uint specular)
{
    imageStore(light_image, p, vec4(light_argb.yzwx) * (1.0 / 255.0));
    imageStore(specular_image, p, vec4(0.0, 0.0, 0.0, float(specular) * (1.0 / 255.0)));
}
// Pixels of a tile without lights: the images are cleared before the passes run
void store_empty(ivec2 p) {}
)GLSL";

inline constexpr const char* lights = R"GLSL(
// One thread per light: the values that do not depend on the pixel
layout(local_size_x=64) in;
void main()
{
    uint l = gl_GlobalInvocationID.x;
    if (l >= light_count()) return;
    uint o = 32u + 16u * l;
    vec3 colour = parameter_column(o + 4u).xyz * parameter(16u);
    float inner = parameter(o + 7u), outer = parameter(o + 8u);
    lights[l].position_radius = vec4(parameter_column(o).xyz, outer);
    lights[l].colour_magnitude = vec4(colour, length(colour));
    lights[l].direction_kind = vec4(parameter_column(o + 12u).xyz, float(data[o + 3u]));
    lights[l].falloff = vec4(1.0 / (outer - inner), parameter(o + 9u), parameter(o + 10u), parameter(o + 11u));
}
)GLSL";

inline constexpr const char* bounds = R"GLSL(
// One work group per block of pixels (five per tile), each thread a 16th of its width and a quarter of its height
// (2x2 pixels at 1280x720): bounds of the view-space positions in the block
layout(local_size_x=16, local_size_y=4) in;
shared vec3 block_lo[64], block_hi[64];
void main()
{
    mat4 m = position_matrix();
    vec3 lo = vec3(3.402823e38), hi = vec3(-3.402823e38);
    ivec2 group = ivec2(gl_WorkGroupID.xy), thread = ivec2(gl_LocalInvocationID.xy);
    ivec2 origin = ivec2(tile_left(group.x), block_top(group.y));
    ivec2 size = ivec2(tile_left(group.x + 1), block_top(group.y + 1)) - origin;
    ivec2 first = origin + thread * size / ivec2(16, 4), end = origin + (thread + 1) * size / ivec2(16, 4);
    for (int y = first.y; y < end.y; y++)
    {
        for (int x = first.x; x < end.x; x++)
        {
            ivec2 pixel = ivec2(x, y);
            uint depth = depth24(pixel);
            if (depth == SKY) continue;
            vec3 position = view_position(m, pixel, depth);
            lo = min(lo, position);
            hi = max(hi, position);
        }
    }
    uint i = gl_LocalInvocationIndex;
    block_lo[i] = lo;
    block_hi[i] = hi;
    for (uint step = 32u; step != 0u; step >>= 1u)
    {
        barrier();
        if (i < step)
        {
            block_lo[i] = min(block_lo[i], block_lo[i + step]);
            block_hi[i] = max(block_hi[i], block_hi[i + step]);
        }
    }
    if (i == 0u)
    {
        // Blocks in the order of the tiles: BLOCKS_PER_TILE consecutive entries per tile
        uvec2 block = gl_WorkGroupID.xy;
        uint entry = ((block.y / BLOCKS_PER_TILE) * uint(TILES_X) + block.x) * BLOCKS_PER_TILE + block.y % BLOCKS_PER_TILE;
        block_bounds[2u * entry] = vec4(block_lo[0], 0.0);
        block_bounds[2u * entry + 1u] = vec4(block_hi[0], 0.0);
    }
}
)GLSL";

inline constexpr const char* cull = R"GLSL(
// One thread per tile: the lights whose volume can reach the tile
// (bounding box of the pixels, the four side planes of the tile's frustum, cone against bounding sphere)
vec3 tile_corner(mat4 clip, float x, float y)
{
    vec4 p = clip[0] * x + clip[1] * y + clip[3];
    return p.xyz / p.w;
}
bool cone_reaches_sphere(vec3 centre, float radius, vec3 apex, float range, vec3 direction, float cosine)
{
    vec3 delta = centre - apex;
    float d2 = dot(delta, delta), r2 = radius * radius;
    if (d2 > (radius + range) * (radius + range)) return false;
    if (r2 > d2) return true;
    float dist = sqrt(d2), s = radius / dist, tangent = sqrt(max(1.0 - s * s, 0.0));
    float u = tangent * dist > range ? (d2 + range * range - r2) / (2.0 * dist * range) : tangent;
    float threshold = u * cosine - sqrt(max((1.0 - u * u) * (1.0 - cosine * cosine), 0.0));
    return dot(direction, delta) / dist > threshold;
}
layout(local_size_x=64) in;
void main()
{
    uint tile = gl_GlobalInvocationID.x;
    if (tile >= uint(TILES)) return;
    vec3 lo = vec3(3.402823e38), hi = vec3(-3.402823e38);
    for (uint block = tile * BLOCKS_PER_TILE, end = block + BLOCKS_PER_TILE; block < end; block++)
    {
        lo = min(lo, block_bounds[2u * block].xyz);
        hi = max(hi, block_bounds[2u * block + 1u].xyz);
    }
    uint mask[MASK_WORDS];
    for (uint i = 0u; i < MASK_WORDS; i++) mask[i] = 0u;
    if (all(greaterThan(hi, lo)))
    {
        vec3 centre = (lo + hi) * 0.5;
        float sphere_radius = length(hi - lo) * 0.5;
        uint o = data[22];
        mat4 clip = mat4(parameter_column(o), parameter_column(o + 4u), parameter_column(o + 8u), parameter_column(o + 12u));
        float tx = float(tile % uint(TILES_X)), ty = float(tile / uint(TILES_X));
        float left = tx * (2.0 / 40.0) - 1.0, right = (tx + 1.0) * (2.0 / 40.0) - 1.0;
        float top = 1.0 - ty * (2.0 / 18.0), bottom = 1.0 - (ty + 1.0) * (2.0 / 18.0);
        vec3 corners[4] = vec3[4](tile_corner(clip, right, top), tile_corner(clip, left, top), tile_corner(clip, left, bottom), tile_corner(clip, right, bottom));
        vec3 planes[4];
        for (int i = 0; i < 4; i++) planes[i] = normalize(cross(corners[(i + 1) % 4], corners[i]));
        for (uint l = 0u, count = light_count(); l < count; l++)
        {
            light_t light = lights[l];
            vec3 position = light.position_radius.xyz;
            float radius = light.position_radius.w;
            vec3 nearest = position - clamp(position, lo, hi);
            if (dot(nearest, nearest) >= radius * radius) continue;
            if (any(lessThan(vec4(dot(planes[0], position), dot(planes[1], position), dot(planes[2], position), dot(planes[3], position)) + radius, vec4(0.0)))) continue;
            if (light.direction_kind.w == 2.0 && !cone_reaches_sphere(centre, sphere_radius, position, radius, light.direction_kind.xyz, light.falloff.z)) continue;
            mask[l / 32u] |= 1u << (l % 32u);
        }
    }
    for (uint i = 0u; i < MASK_WORDS; i++) tile_data[TILE_STRIDE * tile + i] = mask[i];
    uint any_light = 0u;
    for (uint i = 0u; i < MASK_WORDS; i++) any_light |= mask[i];
    tile_data[TILE_STRIDE * tile + TILE_ANY] = any_light;
    tile_data[TILE_STRIDE * tile + TILE_LIT] = 0u;
}
)GLSL";

inline constexpr const char* shade = R"GLSL(
// One thread per pixel: diffuse light (RGB with a shared scale in alpha, square-root encoded) and specular
// intensity from the lights of the pixel's tile
layout(local_size_x=8, local_size_y=8) in;
void main()
{
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    if (pixel.x >= W || pixel.y >= H) return;
    uint tile = uint((pixel.y * TILES_Y / H) * TILES_X + pixel.x * TILES_X / W);
    if (tile_data[TILE_STRIDE * tile + TILE_ANY] == 0u) { store_empty(pixel); return; }
    uint mask[MASK_WORDS];
    for (uint i = 0u; i < MASK_WORDS; i++) mask[i] = tile_data[TILE_STRIDE * tile + i];

    const float SCALE = 0.35355338; // 1 / (2 sqrt 2)
    uvec4 texel = normal_bytes(pixel);
    vec3 normal = normalize(vec3(texel.yzw) * (2.0 / 255.0) - 1.0);
    vec3 position = view_position(position_matrix(), pixel, depth24(pixel));
    vec3 view = normalize(position);
    float gloss = exp2(float(texel.x) * (8.0 / 255.0) - 0.88895999);
    float specular_gain = gloss * 0.92592591 + 1.0;
    vec3 diffuse = vec3(0.0);
    float specular = 0.0;
    bool lit = false;

    for (uint word = 0u; word < MASK_WORDS; word++)
    {
        for (uint bits = mask[word]; bits != 0u; bits &= bits - 1u)
        {
            light_t light = lights[word * 32u + uint(findLSB(bits))];
            vec3 to_light = light.position_radius.xyz - position;
            float distance_squared = dot(to_light, to_light), inverse_distance = inversesqrt(distance_squared);
            // Outside the range the terms below are zero
            if (!in_range(light, to_light, distance_squared, inverse_distance)) continue;
            lit = true;
            float dist = distance_squared * inverse_distance;
            float attenuation = clamp((light.position_radius.w - dist) * light.falloff.x, 0.0, 1.0);
            if (light.direction_kind.w == 2.0)
            {
                float angle = dot(to_light, -light.direction_kind.xyz) * inverse_distance;
                attenuation = dist > light.falloff.w ? attenuation * clamp(light.falloff.y * (light.falloff.z - angle), 0.0, 1.0) : 0.0;
            }
            vec3 direction = to_light * inverse_distance;
            float intensity = max(dot(normal, direction), 0.0) * attenuation * attenuation;
            diffuse += light.colour_magnitude.xyz * intensity;
            float n_dot_h = max(dot(normal, normalize(direction - view)), 0.0);
            float shape = (1.0 - n_dot_h) * gloss + 1.0;
            shape *= shape; shape *= shape; shape *= shape;
            specular += specular_gain * intensity * light.colour_magnitude.w / shape;
        }
    }

    if (lit && tile_data[TILE_STRIDE * tile + TILE_LIT] == 0u) atomicOr(tile_data[TILE_STRIDE * tile + TILE_LIT], 1u);
    vec3 v = sqrt(diffuse);
    float alpha = min(floor(max(max(max(v.x, v.y), v.z), 1.0) * (SCALE * 255.0)), 255.0);
    vec3 rgb = min(floor(v * (SCALE * 255.0 * 255.0) / alpha), vec3(255.0));
    store(pixel, uvec4(alpha, rgb), uint(min(floor(sqrt(specular) * (SCALE * 255.0)), 255.0)));
}
)GLSL";

inline constexpr const char* clear = R"GLSL(
// One thread per pixel row of a tile: the game leaves a tile empty when none of its pixels is inside the range
// of a light, even if a light volume reaches the tile
layout(local_size_x=64) in;
void main()
{
    uint id = gl_GlobalInvocationID.x;
    if (id >= uint(TILES_X * H)) return;
    int tile_x = int(id % uint(TILES_X)), y = int(id / uint(TILES_X));
    uint tile = uint((y * TILES_Y / H) * TILES_X + tile_x);
    if (tile_data[TILE_STRIDE * tile + TILE_LIT] != 0u || tile_data[TILE_STRIDE * tile + TILE_ANY] == 0u) return;
    for (int x = tile_left(tile_x), end = tile_left(tile_x + 1); x < end; x++) store(ivec2(x, y), uvec4(0u), 0u);
}
)GLSL";
}
