#pragma once
// Experimental exact coherent inFamous2 SPU kernel 07170, 181 guest instructions.
// Default-off; callers must apply ISA/config/state/MFC/debug/layout admission.
#include "SPUNativeRWV.hpp"
#include "SPUNativeSXE.hpp"
#include <immintrin.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx512f,avx512vl,avx512bw,avx512dq,fma,ssse3,sse4.1"))), apply_to=function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("avx512f,avx512vl,avx512bw,avx512dq,fma,ssse3,sse4.1")
#endif
namespace rpcs3::experimental_07170 {
// Outcome0 means unsupported before any mutation;1 means complete.
// No RAII is live: cold production state checks may abandon the native frame.

using R = __m128i;
alignas(64) inline constexpr uint32_t rsq_lut[64] = {
0x350160,0x34E954,0x2F993D,0x2F993D,0x2AA523,0x2AA523,0x26190D,0x26190D,0x21E4F9,0x21E4F9,0x1E00E9,0x1E00E9,0x1A5CD9,0x1A5CD9,0x16F8CB,0x16F8CB,0x13CCC0,0x13CCC0,0x10CCB3,0x10CCB3,0x0E00AA,0x0E00AA,0x0B58A1,0x0B58A1,0x08D498,0x08D498,0x067491,0x067491,0x043089,0x043089,0x020C83,0x020C83,0x7FFDF4,0x7FD1DE,0x7859C8,0x783DBA,0x71559C,0x71559C,0x6AE57C,0x6AE57C,0x64F561,0x64F561,0x5F7149,0x5F7149,0x5A4D33,0x5A4D33,0x55811F,0x55811F,0x51050F,0x51050F,0x4CC8FE,0x4CC8FE,0x48D0F0,0x48D0F0,0x4510E4,0x4510E4,0x4180D7,0x4180D7,0x3E24CC,0x3E24CC,0x3AF4C3,0x3AF4C3,0x37E8BA,0x37E8BA
};
struct Ops {
    using R = __m128i;
    static uint32_t word(R x) { return _mm_extract_epi32(x,3); }
    static R splat(uint32_t x) { return _mm_set1_epi32(x); }
    static R addi(R x, int32_t offset) { return _mm_add_epi32(x,splat(uint32_t(offset))); }
    static R ori(R x, uint32_t value) { return _mm_or_si128(x,splat(value)); }
    static R mpyi(R x, int32_t multiplier) { return _mm_mullo_epi32(_mm_srai_epi32(_mm_slli_epi32(x,16),16),splat(uint32_t(multiplier))); }
    static R load(const unsigned char* ls,uint32_t address) { return rpcs3::experimental_sxe::lq(ls,address); }
    static void store(unsigned char* ls,uint32_t address,R value) { rpcs3::experimental_sxe::sq(ls,address,value); }
    static R select(R a,R b,R mask) { return _mm_or_si128(_mm_andnot_si128(mask,a),_mm_and_si128(mask,b)); }
    static R signed_greater(R a,R b) { return _mm_cmpgt_epi32(a,b); }
    static R shuffle(R a,R b,R selector) { return rpcs3::experimental_sxe::shuf(a,b,selector); }
    static R frest(R a) { return rpcs3::experimental_sxe::frest(a); }
    static R fi(R a,R estimate) { return rpcs3::experimental_sxe::fi(a,estimate); }
    static R frsqest(R a) {
        const R index = _mm_and_si128(_mm_srli_epi32(a,18),splat(63));
        const R fraction = _mm_i32gather_epi32(reinterpret_cast<const int*>(rsq_lut),index,4);
        const R exponent = _mm_and_si128(a,splat(0x7f800000));
        const R half = _mm_srli_epi32(_mm_add_epi32(exponent,_mm_and_si128(exponent,splat(0x800000))),1);
        const R adjusted = _mm_sub_epi32(splat(190u<<23),half);
        const R final_exp = _mm_mask_blend_epi32(_mm_cmpeq_epi32_mask(exponent,_mm_setzero_si128()),adjusted,splat(0x7f800000));
        return _mm_or_si128(fraction,final_exp);
    }
    template<uint32_t PC> static R fm(R a,R b) { return rpcs3::experimental_sxe::fm(a,b); }
    template<uint32_t PC> static R fcgt(R a,R b) { return rpcs3::experimental_rwv::greater(a,b); }
    template<uint32_t PC> static R cfltu(R a,unsigned immediate) {
        const __m128 scaled = _mm_mul_ps(_mm_castsi128_ps(a),_mm_set1_ps(std::ldexp(1.f,173-int(immediate))));
        const R nonnegative = _mm_max_epi32(_mm_castps_si128(scaled),_mm_setzero_si128());
        return _mm_cvttps_epu32(_mm_castsi128_ps(nonnegative));
    }
    template<uint32_t PC> static R cuflt(R a,unsigned immediate) {
        return _mm_castps_si128(_mm_mul_ps(_mm_cvtepu32_ps(a),_mm_set1_ps(std::ldexp(1.f,int(immediate)-155))));
    }
};
inline __m512i frsqest4(R a0,R a1,R a2,R a3) {
    __m512i a = _mm512_zextsi128_si512(a0);
    a = _mm512_inserti32x4(a,a1,1); a = _mm512_inserti32x4(a,a2,2); a = _mm512_inserti32x4(a,a3,3);
    const auto i=[](uint32_t x){return _mm512_set1_epi32(x);};
    const __m512i index=_mm512_and_si512(_mm512_srli_epi32(a,18),i(63));
    const __m512i lower=_mm512_permutex2var_epi32(_mm512_load_si512(rsq_lut),index,_mm512_load_si512(rsq_lut+16));
    const __m512i upper=_mm512_permutex2var_epi32(_mm512_load_si512(rsq_lut+32),index,_mm512_load_si512(rsq_lut+48));
    const __m512i fraction=_mm512_mask_blend_epi32(_mm512_cmp_epu32_mask(index,i(32),_MM_CMPINT_LT),upper,lower);
    const __m512i exponent=_mm512_and_si512(a,i(0x7f800000));
    const __m512i half=_mm512_srli_epi32(_mm512_add_epi32(exponent,_mm512_and_si512(exponent,i(0x800000))),1);
    const __m512i adjusted=_mm512_sub_epi32(i(190u<<23),half);
    const __m512i final_exp=_mm512_mask_blend_epi32(_mm512_cmpeq_epi32_mask(exponent,_mm512_setzero_si512()),adjusted,i(0x7f800000));
    return _mm512_or_si512(fraction,final_exp);
}

// Performance selection only. Counts below128 lost in the matched width sweep.
// This prescan never proves any body's operands.
// Shortcut correctness still uses the body's actual four loads + fixedpoint.
inline bool eligible(const void*context,const void*store,uint32_t pc){
 const auto*ctx=static_cast<const unsigned char*>(context);const auto*ls=static_cast<const unsigned char*>(store);
 uint32_t width,pointer;std::memcpy(&width,ctx+64+6*16+12,4);std::memcpy(&pointer,ctx+64+5*16+12,4);width&=0xffff;
 if(width<128 || width>2048 || width%4 || (pc&3) || pc>0x3d740 || (_mm_getcsr()&0xe040)!=0xe040)return false;
 const uint32_t iterations=width/4,span=iterations-1;unsigned nonzero=0;
 for(unsigned i=0;i<32;++i){
  const uint32_t k=1+span*i/31;
  const uint32_t triple=44*width-176*(k+3),fourth=44*width-176*(k+1);
  const uint32_t offsets[4]={112+triple,144+triple,128+triple,160+fourth};
  for(uint32_t offset:offsets){
   const __m128i value=_mm_load_si128(reinterpret_cast<const __m128i*>(ls+((pointer+offset)&0x3fff0)));
   if(!_mm_testz_si128(value,value) && ++nonzero>12)return false;
  }
 }
 return true;
}

template<class Ops,class Check> unsigned run(void* context,void* store,uint32_t runtime_pc,Check checkpoint){
 using R=typename Ops::R;auto* ctx=static_cast<unsigned char*>(context);auto* ls=static_cast<unsigned char*>(store);
 alignas(16) R r[128];std::memcpy(r,ctx+64,sizeof(r));
 // Bounded validated count/relocation domain; unsupported entry is untouched.
 const uint32_t count = Ops::word(r[6]) & 0xffff;
 if(count<4 || count>2048 || count%4 || (runtime_pc&3) || runtime_pc>0x3d740 || (_mm_getcsr()&0xe040)!=0xe040) return 0;
 // Exact emitted IR entry checkpoint is safe; the loop checkpoint is unsavable.
 if(checkpoint(runtime_pc,false))return 0;
 const uint32_t return_pc=Ops::word(r[0])&0x3fffc;
 r[14]=Ops::mpyi(r[6],44); //07170 MPYI
 r[30]=Ops::addi(r[5],128); //07174 AI
 r[20]=Ops::addi(r[14],-176); //07178 AI
 r[29]=Ops::addi(r[5],112); //07180 AI
 r[76]=Ops::load(ls,Ops::word(r[30])+Ops::word(r[20])); //07184 LQX
 r[74]=Ops::load(ls,Ops::word(r[29])+Ops::word(r[20])); //07188 LQX
 r[28]=Ops::addi(r[5],144); //0718c AI
 r[71]=Ops::load(ls,Ops::word(r[28])+Ops::word(r[20])); //07190 LQX
 { const auto estimates=frsqest4(r[76],r[74],r[71],Ops::splat(0u));
 r[11]=_mm512_extracti32x4_epi32(estimates,0);
 r[8]=_mm512_extracti32x4_epi32(estimates,1);
 r[79]=_mm512_extracti32x4_epi32(estimates,2);
 }
 //0719c estimate already batched; exact bits retained.
 r[75]=Ops::fi(r[76],r[11]); //071a0 FI
 //071a4 estimate already batched; exact bits retained.
 r[73]=Ops::fi(r[74],r[8]); //071a8 FI
 r[72]=Ops::fi(r[71],r[79]); //071ac FI
 r[69]=Ops::template fm<0x071b0>(r[75],r[76]); //071b0 FM
 r[27]=Ops::load(ls,runtime_pc+10416); //071b4 LQR
 r[68]=Ops::template fm<0x071b8>(r[73],r[74]); //071b8 FM
 r[61]=Ops::template fm<0x071bc>(r[72],r[71]); //071bc FM
 r[26]=Ops::splat(0u); //071c0 ILHU
 r[70]=Ops::template fcgt<0x071c4>(r[69],r[27]); //071c4 FCGT
 r[67]=Ops::template fcgt<0x071c8>(r[68],r[27]); //071c8 FCGT
 r[7]=Ops::select(r[26],r[69],r[70]); //071cc SELB
 r[8]=Ops::select(r[26],r[68],r[67]); //071d0 SELB
 r[62]=Ops::template fcgt<0x071d4>(r[61],r[27]); //071d4 FCGT
 r[59]=Ops::template fcgt<0x071d8>(r[8],r[7]); //071d8 FCGT
 r[9]=Ops::select(r[26],r[61],r[62]); //071dc SELB
 r[49]=Ops::select(r[7],r[8],r[59]); //071e0 SELB
 r[21]=Ops::splat(1052049408u); //071e4 ILHU
 r[50]=Ops::template fcgt<0x071e8>(r[49],r[9]); //071e8 FCGT
 r[21]=Ops::ori(r[21],1267u); //071ec IOHL
 r[55]=Ops::select(r[9],r[49],r[50]); //071f0 SELB
 r[39]=Ops::template fm<0x071f4>(r[55],r[21]); //071f4 FM
 r[19]=Ops::addi(r[20],-176); //071f8 AI
 r[73]=Ops::load(ls,Ops::word(r[28])+Ops::word(r[19])); //071fc LQX
 r[74]=Ops::load(ls,Ops::word(r[29])+Ops::word(r[19])); //07200 LQX
 r[42]=Ops::template fcgt<0x07204>(r[39],r[21]); //07204 FCGT
 r[31]=Ops::splat(1065287680u); //07208 ILHU
 r[72]=Ops::load(ls,Ops::word(r[30])+Ops::word(r[19])); //0720c LQX
 r[40]=Ops::select(r[21],r[39],r[42]); //07210 SELB
 r[14]=Ops::template fm<0x07218>(r[40],r[31]); //07218 FM
 { const auto estimates=frsqest4(r[73],r[74],r[72],Ops::splat(0u));
 r[36]=_mm512_extracti32x4_epi32(estimates,0);
 r[16]=_mm512_extracti32x4_epi32(estimates,1);
 r[15]=_mm512_extracti32x4_epi32(estimates,2);
 }
 //07220 estimate already batched; exact bits retained.
 r[33]=Ops::addi(r[5],160); //07224 AI
 r[18]=Ops::addi(r[19],-176); //07228 AI
 //0722c estimate already batched; exact bits retained.
 r[77]=Ops::fi(r[73],r[36]); //07230 FI
 r[78]=Ops::fi(r[74],r[16]); //07234 FI
 r[75]=Ops::template cfltu<0x07238>(r[14],165); //07238 CFLTU
 r[37]=Ops::load(ls,Ops::word(r[30])+Ops::word(r[18])); //0723c LQX
 r[76]=Ops::fi(r[72],r[15]); //07240 FI
 r[32]=Ops::splat(1065353216u); //07248 ILHU
 r[35]=Ops::load(ls,Ops::word(r[33])+Ops::word(r[20])); //0724c LQX
 r[34]=Ops::splat(255u); //07250 ILA
 r[32]=Ops::ori(r[32],32897u); //07254 IOHL
 r[45]=Ops::template fm<0x07258>(r[77],r[73]); //07258 FM
 r[64]=Ops::template fm<0x07260>(r[78],r[74]); //07260 FM
 { const auto estimates=frsqest4(r[37],r[35],Ops::splat(0u),Ops::splat(0u));
 r[43]=_mm512_extracti32x4_epi32(estimates,0);
 r[79]=_mm512_extracti32x4_epi32(estimates,1);
 }
 r[10]=Ops::signed_greater(r[75],r[34]); //07268 CGT
 r[68]=Ops::template fm<0x07270>(r[76],r[72]); //07270 FM
 //07274 estimate already batched; exact bits retained.
 r[17]=Ops::select(r[75],r[34],r[10]); //07278 SELB
 r[57]=Ops::fi(r[37],r[43]); //0727c FI
 r[65]=Ops::template cuflt<0x07280>(r[17],147); //07280 CUFLT
 r[56]=Ops::fi(r[35],r[79]); //07284 FI
 r[48]=Ops::template fcgt<0x07288>(r[45],r[27]); //07288 FCGT
 r[69]=Ops::template fcgt<0x07290>(r[68],r[27]); //07290 FCGT
 r[38]=Ops::load(ls,Ops::word(r[29])+Ops::word(r[18])); //07294 LQX
 r[66]=Ops::template fcgt<0x07298>(r[64],r[27]); //07298 FCGT
 r[15]=Ops::select(r[26],r[68],r[69]); //0729c SELB
 r[14]=Ops::select(r[26],r[64],r[66]); //072a0 SELB
 r[12]=Ops::template fm<0x072a4>(r[65],r[32]); //072a4 FM
 r[47]=Ops::template fcgt<0x072a8>(r[14],r[15]); //072a8 FCGT
 r[2]=r[20]; //072ac ROTQBII
 r[13]=Ops::select(r[26],r[45],r[48]); //072b0 SELB
 //072b4 estimate already batched; exact bits retained.
 r[20]=Ops::select(r[15],r[14],r[47]); //072b8 SELB
 r[25]=Ops::load(ls,runtime_pc+10400); //072bc LQR
 r[36]=Ops::mpyi(r[6],4); //072c0 MPYI
 r[39]=Ops::load(ls,Ops::word(r[28])+Ops::word(r[18])); //072c4 LQX
 r[40]=Ops::template fcgt<0x072c8>(r[20],r[13]); //072c8 FCGT
 r[16]=r[19]; //072cc ROTQBII
 //072d0 FI deferred until its exact estimate is available; no intervening consumer.
 r[19]=Ops::frest(r[12]); //072d4 FREST
 r[59]=Ops::select(r[13],r[20],r[40]); //072d8 SELB
 r[24]=Ops::load(ls,runtime_pc+10336); //072dc LQR
 r[23]=Ops::load(ls,runtime_pc+10320); //072e4 LQR
 r[53]=Ops::template fm<0x072e8>(r[59],r[21]); //072e8 FM
 r[22]=Ops::load(ls,runtime_pc+10304); //072ec LQR
 r[58]=Ops::fi(r[12],r[19]); //072f0 FI
 { const auto estimates=frsqest4(r[38],r[39],Ops::splat(0u),Ops::splat(0u));
 r[46]=_mm512_extracti32x4_epi32(estimates,0);
 r[43]=_mm512_extracti32x4_epi32(estimates,1);
 }
 r[66]=Ops::fi(r[38],r[46]); //072d0 FI, before original loop checkpoint.
// Stock prologue commits before the first unsavable loop checkpoint.
 std::memcpy(ctx+64+30*16,&r[30],16);
 std::memcpy(ctx+64+29*16,&r[29],16);
 std::memcpy(ctx+64+28*16,&r[28],16);
 std::memcpy(ctx+64+27*16,&r[27],16);
 std::memcpy(ctx+64+26*16,&r[26],16);
 std::memcpy(ctx+64+67*16,&r[67],16);
 std::memcpy(ctx+64+21*16,&r[21],16);
 std::memcpy(ctx+64+31*16,&r[31],16);
 std::memcpy(ctx+64+33*16,&r[33],16);
 std::memcpy(ctx+64+34*16,&r[34],16);
 std::memcpy(ctx+64+32*16,&r[32],16);
 std::memcpy(ctx+64+20*16,&r[20],16);
 std::memcpy(ctx+64+25*16,&r[25],16);
 std::memcpy(ctx+64+19*16,&r[19],16);
 std::memcpy(ctx+64+24*16,&r[24],16);
 std::memcpy(ctx+64+23*16,&r[23],16);
 std::memcpy(ctx+64+22*16,&r[22],16);
bool zero_fixed=false;
 alignas(16) R fixedpoint_before[17];
L7300:;
 {
 // Production Check resumes these POD locals in place or escapes via g_escape.
 checkpoint((runtime_pc+0x190)&0x3fffc,true);
 // Only pure register arithmetic moves; memory issue sequence remains four
 // original LQX operations in original order followed by two original STQX.
 const R pre38=Ops::load(ls,Ops::word(r[29])+Ops::word(Ops::addi(r[18],-176)));
 const R pre39=Ops::load(ls,Ops::word(r[28])+Ops::word(Ops::addi(r[18],-176)));
 const R pre37=Ops::load(ls,Ops::word(r[30])+Ops::word(Ops::addi(r[18],-176)));
 const R pre35=Ops::load(ls,Ops::word(r[33])+Ops::word(r[16]));
 const R next_any=_mm_or_si128(_mm_or_si128(pre38,pre39),_mm_or_si128(pre37,pre35));
 const bool next_zero=_mm_testz_si128(next_any,next_any);
 if(zero_fixed && next_zero) {
  r[12]=r[16]; r[16]=r[18]; r[18]=Ops::addi(r[18],-176);
  r[10]=r[2]; r[2]=r[12]; r[36]=Ops::addi(r[36],-16);
  Ops::store(ls,Ops::word(r[3])+Ops::word(r[36]),r[52]);
  Ops::store(ls,Ops::word(r[4])+Ops::word(r[36]),r[54]);
  if(Ops::word(r[10])!=0)goto L7300;
  goto L7440;
 }
 zero_fixed=false;
 const R current_any=_mm_or_si128(_mm_or_si128(r[35],r[37]),_mm_or_si128(r[38],r[39]));
 const bool probe=next_zero && _mm_testz_si128(current_any,current_any);
 if(probe) {
 fixedpoint_before[0]=r[39];
 fixedpoint_before[1]=r[43];
 fixedpoint_before[2]=r[53];
 fixedpoint_before[3]=r[58];
 fixedpoint_before[4]=r[56];
 fixedpoint_before[5]=r[35];
 fixedpoint_before[6]=r[57];
 fixedpoint_before[7]=r[37];
 fixedpoint_before[8]=r[66];
 fixedpoint_before[9]=r[38];
 fixedpoint_before[10]=r[8];
 fixedpoint_before[11]=r[7];
 fixedpoint_before[12]=r[9];
 fixedpoint_before[13]=r[14];
 fixedpoint_before[14]=r[15];
 fixedpoint_before[15]=r[17];
 fixedpoint_before[16]=r[13];
 }

 r[5]=Ops::fi(r[39],r[43]); //07300 FI
 r[12]=r[16]; //07304 ROTQBII
 r[6]=Ops::template fcgt<0x07308>(r[53],r[21]); //07308 FCGT
 r[16]=r[18]; //0730c ROTQBII
 r[18]=Ops::addi(r[18],-176); //07310 AI
 r[79]=Ops::template fm<0x07314>(r[58],r[21]); //07314 FM
 r[41]=Ops::select(r[21],r[53],r[6]); //07318 SELB
 r[10]=Ops::template fm<0x0731c>(r[56],r[35]); //0731c FM
 r[11]=Ops::template fm<0x07320>(r[41],r[31]); //07320 FM
 r[73]=Ops::template fm<0x07324>(r[57],r[37]); //07324 FM
 r[61]=Ops::template fm<0x07328>(r[5],r[39]); //07328 FM
 r[78]=Ops::template fm<0x0732c>(r[66],r[38]); //0732c FM
 r[56]=Ops::template fm<0x07330>(r[8],r[79]); //07330 FM
 r[38]=pre38; //07334 LQX already issued once above
 r[57]=Ops::template fm<0x07338>(r[7],r[79]); //07338 FM
 r[55]=Ops::template fcgt<0x0733c>(r[10],r[27]); //0733c FCGT
 r[58]=Ops::template fm<0x07340>(r[9],r[79]); //07340 FM
 r[50]=Ops::template cfltu<0x07344>(r[11],165); //07344 CFLTU
 r[60]=Ops::template fcgt<0x07348>(r[61],r[27]); //07348 FCGT
 r[54]=Ops::template fm<0x0734c>(r[56],r[31]); //0734c FM
 r[53]=Ops::template fm<0x07350>(r[57],r[31]); //07350 FM
 r[63]=Ops::select(r[26],r[10],r[55]); //07354 SELB
 r[51]=Ops::template fm<0x07358>(r[58],r[31]); //07358 FM
 r[44]=Ops::template fcgt<0x0735c>(r[78],r[27]); //0735c FCGT
 r[49]=Ops::template fm<0x07360>(r[63],r[21]); //07360 FM
 r[52]=Ops::signed_greater(r[50],r[34]); //07364 CGT
 r[71]=Ops::template cfltu<0x07368>(r[54],165); //07368 CFLTU
 r[75]=Ops::template cfltu<0x07370>(r[53],165); //07370 CFLTU
 r[11]=r[17]; //07374 ROTQBII
 r[17]=Ops::select(r[50],r[34],r[52]); //07378 SELB
 r[39]=pre39; //0737c LQX already issued once above
 r[69]=Ops::template cfltu<0x07380>(r[51],165); //07380 CFLTU
 r[77]=Ops::template fcgt<0x07384>(r[73],r[27]); //07384 FCGT
 r[74]=Ops::template fm<0x07388>(r[49],r[31]); //07388 FM
 r[8]=r[14]; //0738c ROTQBII
 r[14]=Ops::select(r[26],r[78],r[44]); //07390 SELB
 r[37]=pre37; //07394 LQX already issued once above
 r[70]=Ops::template cuflt<0x07398>(r[17],147); //07398 CUFLT
 r[76]=Ops::signed_greater(r[75],r[34]); //073a0 CGT
 r[7]=r[15]; //073a4 ROTQBII
 r[15]=Ops::select(r[26],r[73],r[77]); //073a8 SELB
 r[72]=Ops::signed_greater(r[71],r[34]); //073ac CGT
 r[64]=Ops::select(r[75],r[34],r[76]); //073b0 SELB
 r[40]=Ops::template cfltu<0x073b4>(r[74],165); //073b4 CFLTU
 r[68]=Ops::signed_greater(r[69],r[34]); //073b8 CGT
 r[65]=Ops::select(r[71],r[34],r[72]); //073c0 SELB
 r[10]=r[2]; //073c4 ROTQBII
 r[42]=Ops::template fm<0x073c8>(r[70],r[32]); //073c8 FM
 r[9]=r[13]; //073cc ROTQBII
 r[63]=Ops::select(r[69],r[34],r[68]); //073d0 SELB
 r[35]=pre35; //073d4 LQX already issued once above
 r[62]=Ops::template fcgt<0x073d8>(r[14],r[15]); //073d8 FCGT
 r[41]=Ops::shuffle(r[65],r[64],r[22]); //073dc SHUFB
 r[13]=Ops::select(r[26],r[61],r[60]); //073e0 SELB
 r[48]=Ops::shuffle(r[63],r[11],r[23]); //073e4 SHUFB
 r[47]=Ops::select(r[15],r[14],r[62]); //073e8 SELB
 { const auto estimates=frsqest4(r[39],r[37],r[35],r[38]);
 r[43]=_mm512_extracti32x4_epi32(estimates,0);
 r[44]=_mm512_extracti32x4_epi32(estimates,1);
 r[49]=_mm512_extracti32x4_epi32(estimates,2);
 r[55]=_mm512_extracti32x4_epi32(estimates,3);
 }
 r[59]=Ops::signed_greater(r[40],r[34]); //073f0 CGT
 //073f4 estimate already batched; exact bits retained.
 r[46]=Ops::template fcgt<0x073f8>(r[47],r[13]); //073f8 FCGT
 r[45]=Ops::frest(r[42]); //073fc FREST
 r[50]=Ops::select(r[40],r[34],r[59]); //07400 SELB
 //07404 estimate already batched; exact bits retained.
 r[51]=Ops::select(r[13],r[47],r[46]); //07408 SELB
 r[52]=Ops::shuffle(r[41],r[48],r[24]); //0740c SHUFB
 r[36]=Ops::addi(r[36],-16); //07410 AI
 r[54]=Ops::shuffle(r[50],r[50],r[25]); //07414 SHUFB
 r[53]=Ops::template fm<0x07418>(r[51],r[21]); //07418 FM
 //0741c estimate already batched; exact bits retained.
 r[58]=Ops::fi(r[42],r[45]); //07420 FI
 r[2]=r[12]; //07424 ROTQBII
 r[57]=Ops::fi(r[37],r[44]); //07428 FI
 Ops::store(ls,Ops::word(r[3])+Ops::word(r[36]),r[52]); //0742c STQX
 r[56]=Ops::fi(r[35],r[49]); //07430 FI
 Ops::store(ls,Ops::word(r[4])+Ops::word(r[36]),r[54]); //07434 STQX
 r[66]=Ops::fi(r[38],r[55]); //07438 FI
 if(probe) {
 R changed=Ops::splat(0u);
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[0],r[39]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[1],r[43]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[2],r[53]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[3],r[58]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[4],r[56]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[5],r[35]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[6],r[57]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[7],r[37]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[8],r[66]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[9],r[38]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[10],r[8]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[11],r[7]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[12],r[9]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[13],r[14]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[14],r[15]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[15],r[17]));
 changed=_mm_or_si128(changed,_mm_xor_si128(fixedpoint_before[16],r[13]));
 zero_fixed=_mm_testz_si128(changed,changed);
 }
 if(Ops::word(r[10])!=0)goto L7300;
 }
L7440:;
 std::memcpy(ctx+24,&return_pc,4);
 std::memcpy(ctx+64+2*16,&r[2],16);
 std::memcpy(ctx+64+5*16,&r[5],16);
 std::memcpy(ctx+64+6*16,&r[6],16);
 std::memcpy(ctx+64+7*16,&r[7],16);
 std::memcpy(ctx+64+8*16,&r[8],16);
 std::memcpy(ctx+64+9*16,&r[9],16);
 std::memcpy(ctx+64+10*16,&r[10],16);
 std::memcpy(ctx+64+11*16,&r[11],16);
 std::memcpy(ctx+64+12*16,&r[12],16);
 std::memcpy(ctx+64+13*16,&r[13],16);
 std::memcpy(ctx+64+14*16,&r[14],16);
 std::memcpy(ctx+64+15*16,&r[15],16);
 std::memcpy(ctx+64+16*16,&r[16],16);
 std::memcpy(ctx+64+17*16,&r[17],16);
 std::memcpy(ctx+64+18*16,&r[18],16);
 std::memcpy(ctx+64+19*16,&r[19],16);
 std::memcpy(ctx+64+20*16,&r[20],16);
 std::memcpy(ctx+64+21*16,&r[21],16);
 std::memcpy(ctx+64+22*16,&r[22],16);
 std::memcpy(ctx+64+23*16,&r[23],16);
 std::memcpy(ctx+64+24*16,&r[24],16);
 std::memcpy(ctx+64+25*16,&r[25],16);
 std::memcpy(ctx+64+26*16,&r[26],16);
 std::memcpy(ctx+64+27*16,&r[27],16);
 std::memcpy(ctx+64+28*16,&r[28],16);
 std::memcpy(ctx+64+29*16,&r[29],16);
 std::memcpy(ctx+64+30*16,&r[30],16);
 std::memcpy(ctx+64+31*16,&r[31],16);
 std::memcpy(ctx+64+32*16,&r[32],16);
 std::memcpy(ctx+64+33*16,&r[33],16);
 std::memcpy(ctx+64+34*16,&r[34],16);
 std::memcpy(ctx+64+35*16,&r[35],16);
 std::memcpy(ctx+64+36*16,&r[36],16);
 std::memcpy(ctx+64+37*16,&r[37],16);
 std::memcpy(ctx+64+38*16,&r[38],16);
 std::memcpy(ctx+64+39*16,&r[39],16);
 std::memcpy(ctx+64+40*16,&r[40],16);
 std::memcpy(ctx+64+41*16,&r[41],16);
 std::memcpy(ctx+64+42*16,&r[42],16);
 std::memcpy(ctx+64+43*16,&r[43],16);
 std::memcpy(ctx+64+44*16,&r[44],16);
 std::memcpy(ctx+64+45*16,&r[45],16);
 std::memcpy(ctx+64+46*16,&r[46],16);
 std::memcpy(ctx+64+47*16,&r[47],16);
 std::memcpy(ctx+64+48*16,&r[48],16);
 std::memcpy(ctx+64+49*16,&r[49],16);
 std::memcpy(ctx+64+50*16,&r[50],16);
 std::memcpy(ctx+64+51*16,&r[51],16);
 std::memcpy(ctx+64+52*16,&r[52],16);
 std::memcpy(ctx+64+53*16,&r[53],16);
 std::memcpy(ctx+64+54*16,&r[54],16);
 std::memcpy(ctx+64+55*16,&r[55],16);
 std::memcpy(ctx+64+56*16,&r[56],16);
 std::memcpy(ctx+64+57*16,&r[57],16);
 std::memcpy(ctx+64+58*16,&r[58],16);
 std::memcpy(ctx+64+59*16,&r[59],16);
 std::memcpy(ctx+64+60*16,&r[60],16);
 std::memcpy(ctx+64+61*16,&r[61],16);
 std::memcpy(ctx+64+62*16,&r[62],16);
 std::memcpy(ctx+64+63*16,&r[63],16);
 std::memcpy(ctx+64+64*16,&r[64],16);
 std::memcpy(ctx+64+65*16,&r[65],16);
 std::memcpy(ctx+64+66*16,&r[66],16);
 std::memcpy(ctx+64+67*16,&r[67],16);
 std::memcpy(ctx+64+68*16,&r[68],16);
 std::memcpy(ctx+64+69*16,&r[69],16);
 std::memcpy(ctx+64+70*16,&r[70],16);
 std::memcpy(ctx+64+71*16,&r[71],16);
 std::memcpy(ctx+64+72*16,&r[72],16);
 std::memcpy(ctx+64+73*16,&r[73],16);
 std::memcpy(ctx+64+74*16,&r[74],16);
 std::memcpy(ctx+64+75*16,&r[75],16);
 std::memcpy(ctx+64+76*16,&r[76],16);
 std::memcpy(ctx+64+77*16,&r[77],16);
 std::memcpy(ctx+64+78*16,&r[78],16);
 std::memcpy(ctx+64+79*16,&r[79],16);
 return 1;
}

} // namespace rpcs3::experimental_07170
#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
