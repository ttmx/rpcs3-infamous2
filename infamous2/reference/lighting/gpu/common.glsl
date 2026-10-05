// Offline GPU prototype of the captured P071 lighting job. No RPCS3 hooks.
layout(std430, binding=0) readonly buffer Inputs { uint data[]; };
layout(std430, binding=1) buffer Outputs { uint output_data[]; };
layout(std430, binding=2) buffer Positions { vec4 position_data[]; };
layout(std430, binding=3) buffer Tiles { uint tile_data[]; };
const uint W=1280u, H=720u, PIXELS=W*H;
// One bit per light and tile: up to 256 lights.
const uint MASK_WORDS=8u;
float parameter(uint i) { return uintBitsToFloat(data[i]); }
vec4 matrix_column(uint i) { return vec4(parameter(4u*i),parameter(4u*i+1u),parameter(4u*i+2u),parameter(4u*i+3u)); }
float light_value(uint l,uint i) { return parameter(32u+16u*l+i); }
vec3 light_vector(uint l,uint i) { return vec3(light_value(l,i),light_value(l,i+1u),light_value(l,i+2u)); }
uint light_kind(uint l) { return data[32u+16u*l+3u]; }
float estimate(float v,bool rsqrt)
{
    uint a=floatBitsToUint(v), lut=data[20];
    uint f=rsqrt ? 288u : 0u, e=rsqrt ? 352u : 32u;
    uint b=data[lut+f+((a>>18u)&(rsqrt ? 63u : 31u))] | data[lut+e+((a>>23u)&255u)];
    if(!rsqrt) b |= a&0x80000000u;
    precise float base=uintBitsToFloat((b&0x007ffc00u)|0x3f800000u);
    precise float step=float(b&0x3ffu)*exp2(-13.0);
    precise float y=float(a&0x7ffffu)*exp2(-19.0);
    precise float res=base-step*y;
    return uintBitsToFloat((b&0xff800000u)|(floatBitsToUint(res)&0x007fffffu));
}
float dot3(vec3 a,vec3 b)
{
    precise float v=(a.x*b.x+a.y*b.y)+a.z*b.z;
    return v;
}
vec3 norm(vec3 a) { precise vec3 v=a*estimate(dot3(a,a),true); return v; }
uvec4 bytes(uint v) { return uvec4(v>>24u,(v>>16u)&255u,(v>>8u)&255u,v&255u); }
uint pack_bytes(uvec4 v) { return (v.x<<24u)|(v.y<<16u)|(v.z<<8u)|v.w; }
float cf(uint v) { return uintBitsToFloat(v); }
float gloss(float material)
{
    precise float v=material*(1.0/256.0)*cf(0x41008081u)+cf(0xbf6392e2u);
    float k=floor(v);
    precise float t=(k-v)*cf(0x3f317218u);
    precise float lo=t*cf(0xbe2aaa4fu)+cf(0x3efffffdu);
    precise float hi=t*cf(0xb9142e40u)+cf(0x3aae4f6fu);
    lo=t*lo-1.0; hi=t*hi+cf(0xbc08026du);
    lo=t*lo+1.0; hi=t*hi+cf(0x3d2aa0e5u);
    precise float t2=t*t;
    precise float outv=((t2*t2)*hi+lo)*exp2(k);
    return outv;
}
float square_root(float a)
{
    if(a<=0.0) return 0.0;
    return a*estimate(a,true);
}

// Native geometric rejection from the game's tile frustum and cone volumes.
float refined_reciprocal(float a)
{
    float r=estimate(a,false);
    precise float result=(1.0-a*r)*r+r;
    return result;
}
float refined_sqrt(float a)
{
    if(a<=cf(0x00800000u)) return 0.0;
    float r=estimate(a,true);
    precise float result=((1.0-a*(r*r))*(r*0.5)+r)*a;
    return result;
}
vec3 refined_normalize(vec3 a)
{
    float d=dot3(a,a),r=estimate(d,true);
    precise vec3 result=a*((1.0-d*(r*r))*(r*0.5)+r);
    return result;
}
vec4 clip_column(uint i)
{
    uint o=data[22]+4u*i;
    return vec4(parameter(o),parameter(o+1u),parameter(o+2u),parameter(o+3u));
}
vec3 tile_corner(float x,float y)
{
    precise vec4 p=clip_column(0u)*x+clip_column(1u)*y;
    p=p+clip_column(3u);
    return p.xyz*refined_reciprocal(p.w);
}
bool cone_sphere(vec3 centre,float radius,vec3 lp,float outer,vec3 direction,float cosine)
{
    vec3 delta=centre-lp;
    float d2=dot3(delta,delta),r2=radius*radius;
    if(d2>(radius+outer)*(radius+outer)) return false;
    if(r2>d2) return true;
    float dist=refined_sqrt(d2),inv=refined_reciprocal(dist);
    float s=radius*inv,tangent=refined_sqrt(1.0-s*s);
    float u=tangent*dist>outer ? (d2+outer*outer-r2)*refined_reciprocal((2.0*dist)*outer) : tangent;
    float threshold=u*cosine-refined_sqrt((1.0-u*u)*(1.0-cosine*cosine));
    return dot3(direction,delta*inv)>threshold;
}
