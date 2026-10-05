// Offline harness: the G-buffer words follow the parameters, the results go to a buffer
layout(std430, binding=4) buffer Outputs { uint output_data[]; };
uint depth24(ivec2 p) { return data[data[19] + uint(p.y * W + p.x)] >> 8u; }
// Material byte, then the normal
uvec4 normal_bytes(ivec2 p) { uint v = data[data[18] + uint(p.y * W + p.x)]; return uvec4(v >> 24u, (v >> 16u) & 255u, (v >> 8u) & 255u, v & 255u); }
void store(ivec2 p, uvec4 light_argb, uint specular)
{
    uint index = uint(p.y * W + p.x);
    output_data[index] = (light_argb.x << 24u) | (light_argb.y << 16u) | (light_argb.z << 8u) | light_argb.w;
    output_data[uint(W * H) + index] = specular << 24u;
}
// Pixels of a tile without lights. The live passes start from cleared images instead.
void store_empty(ivec2 p) { store(p, uvec4(0u), 0u); }
