#pragma once
// Experimental complete inFamous2 SPU kernel. Runtime CPU gate is mandatory
// before entering any function in this header. Disabled by default in RPCS3.
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
namespace rpcs3::experimental_rwv {
// Outcomes0unsupported/no mutation;1complete;2synthetic offline interruption.
// Production checkpoint resumes in-place or g_escape, retaining C++ locals.
#include <immintrin.h>
#include <cstdint>
#include <cstring>
using bytes16 = unsigned char __attribute__((vector_size(16)));
inline uint32_t word(__m128i a){return _mm_extract_epi32(a,3);}
inline __m128i select(__m128i a,__m128i b,__m128i c){return _mm_or_si128(_mm_andnot_si128(c,a),_mm_and_si128(c,b));}
template<unsigned char... B> inline __m128i constant(){static_assert(sizeof...(B)==16);return _mm_setr_epi8(B...);}
inline __m128i reverse(__m128i v){return _mm_shuffle_epi8(v,_mm_setr_epi8(15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0));}
inline __m128i load_ls(unsigned char* ls,uint32_t address){return reverse(_mm_load_si128(reinterpret_cast<const __m128i*>(ls+(address&0x3fff0))));}
inline void store_ls(unsigned char* ls,uint32_t address,__m128i value){_mm_store_si128(reinterpret_cast<__m128i*>(ls+(address&0x3fff0)),reverse(value));}
template<unsigned char... B> inline __m128i shuffle_constant(__m128i a,__m128i b){
 const __m128i c=constant<B...>();
 const auto index=_mm_xor_si128(c,_mm_set1_epi8(15));
 const auto ab=_mm_blendv_epi8(_mm_shuffle_epi8(a,index),_mm_shuffle_epi8(b,index),_mm_slli_epi16(c,3));
 const auto special=_mm_cmpeq_epi8(_mm_and_si128(c,_mm_set1_epi8(0xe0)),_mm_set1_epi8(0xc0));
 const auto top=_mm_cmpeq_epi8(_mm_and_si128(c,_mm_set1_epi8(0xe0)),_mm_set1_epi8(0xe0));
 return _mm_or_si128(ab,_mm_or_si128(special,_mm_and_si128(top,_mm_set1_epi8(0x80))));
}
inline __m128i shuffle(__m128i a,__m128i b,__m128i c){
 const auto index=_mm_xor_si128(c,_mm_set1_epi8(15));
 const auto ab=_mm_blendv_epi8(_mm_shuffle_epi8(a,index),_mm_shuffle_epi8(b,index),_mm_slli_epi16(c,3));
 const auto special=_mm_cmpeq_epi8(_mm_and_si128(c,_mm_set1_epi8(0xe0)),_mm_set1_epi8(0xc0));
 const auto top=_mm_cmpeq_epi8(_mm_and_si128(c,_mm_set1_epi8(0xe0)),_mm_set1_epi8(0xe0));
 return _mm_or_si128(ab,_mm_or_si128(special,_mm_and_si128(top,_mm_set1_epi8(0x80))));
}
inline __m128 clamp(__m128 a){return _mm_range_ps(a,_mm_castsi128_ps(_mm_set1_epi32(0x7f7fffff)),2);}
inline __m128 multiply(__m128 a,__m128 b){return _mm_and_ps(_mm_mul_ps(a,b),_mm_and_ps(_mm_cmp_ps(a,_mm_setzero_ps(),_CMP_NEQ_UQ),_mm_cmp_ps(b,_mm_setzero_ps(),_CMP_NEQ_UQ)));}
inline __m128 fused(__m128 a,__m128 b,__m128 c){
 return _mm_mask_blend_ps(_mm_cmp_ps_mask(a,_mm_setzero_ps(),_CMP_EQ_OQ),_mm_fmadd_ps(a,b,c),c);
}
alignas(64) inline constexpr uint32_t reciprocal_lut[32]={8387552,8357798,7401330,7375680,6523666,6503146,5739204,5720736,5032574,5018210,4393540,4380202,3812882,3800570,3282404,3273170,2795966,2786732,2348442,2342286,1934710,1934710,1551706,1551706,1197379,1197379,866605,866605,558362,558362,269575,269575};
inline __m128i reciprocal_estimate(__m128i a){
 auto exponent=_mm_and_si128(a,_mm_set1_epi32(0x7f800000));
 auto re=_mm_subs_epu16(_mm_set1_epi32(0x7e800000),exponent);
 auto fixed=_mm_mask_blend_epi32(_mm_cmp_epu32_mask(exponent,_mm_setzero_si128(),_MM_CMPINT_GT),_mm_set1_epi32(0x7f800000),re);
 auto index=_mm_and_si128(_mm_srli_epi32(a,18),_mm_set1_epi32(31));
 auto fraction=_mm512_castsi512_si128(_mm512_permutex2var_epi32(_mm512_load_si512(reciprocal_lut),_mm512_castsi128_si512(index),_mm512_load_si512(reciprocal_lut+16)));
 return _mm_or_si128(fraction,_mm_or_si128(fixed,_mm_and_si128(a,_mm_set1_epi32(0x80000000u))));
}
inline __m128i interpolate(__m128i a,__m128i b){
 auto base=_mm_slli_epi32(_mm_and_si128(b,_mm_set1_epi32(0x007ffc00)),9);
 auto ymul=_mm_mullo_epi32(_mm_and_si128(b,_mm_set1_epi32(0x3ff)),_mm_and_si128(a,_mm_set1_epi32(0x7ffff)));
 auto comparison=_mm_maskz_set1_epi32(_mm_cmp_epu32_mask(ymul,base,_MM_CMPINT_GT),-1);
 auto mantissa=_mm_srlv_epi32(_mm_sub_epi32(base,ymul),_mm_add_epi32(comparison,_mm_set1_epi32(9)));
 auto exponent=_mm_sub_epi32(_mm_and_si128(b,_mm_set1_epi32(0xff800000u)),_mm_and_si128(comparison,_mm_set1_epi32(0x00800000)));
 return _mm_or_si128(_mm_castps_si128(clamp(_mm_castsi128_ps(exponent))),_mm_and_si128(mantissa,_mm_set1_epi32(0x007fffff)));
}

inline __m128i greater(__m128i a,__m128i b){
 auto either_positive=_mm_cmpge_epi32_mask(_mm_and_si128(a,b),_mm_setzero_si128());
 auto order=_mm_mask_blend_epi32(either_positive,_mm_cmplt_epi32(a,b),_mm_cmpgt_epi32(a,b));
 return _mm_and_si128(order,_mm_castps_si128(_mm_cmp_ps(_mm_castsi128_ps(a),_mm_castsi128_ps(b),_CMP_NEQ_UQ)));
}

// Requires finite point operand; bounds may contain PS3 extended-range sentinels.
inline __m128i greater_finite_point(__m128i a,__m128i b){
 auto ordered=_mm_cmp_ps_mask(_mm_castsi128_ps(a),_mm_castsi128_ps(b),_CMP_GT_OQ);
 auto negative_extended=_mm_cmp_epu32_mask(b,_mm_set1_epi32(0xff800000u),_MM_CMPINT_NLT);
 return _mm_maskz_set1_epi32(ordered|negative_extended,-1);
}
// Requires the refined reciprocal operand to be normal and nonzero.
inline __m128 multiply_refined(__m128 a,__m128 b){
 return _mm_maskz_mul_ps(_mm_cmp_ps_mask(a,_mm_setzero_ps(),_CMP_NEQ_UQ),a,b);
}

template<class Check> unsigned run(void* context, void* store, uint32_t pc, Check checkpoint) {
 auto* ls=static_cast<unsigned char*>(store);
 __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+0*16));
 __m128i r1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+1*16));
 __m128i r2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+2*16));
 __m128i r3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+3*16));
 __m128i r4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+4*16));
 __m128i r5 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+5*16));
 __m128i r6 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+6*16));
 __m128i r7 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+7*16));
 __m128i r8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+8*16));
 __m128i r9 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+9*16));
 __m128i r10 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+10*16));
 __m128i r11 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+11*16));
 __m128i r12 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+12*16));
 __m128i r13 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+13*16));
 __m128i r14 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+14*16));
 __m128i r15 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+15*16));
 __m128i r16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+16*16));
 __m128i r17 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+17*16));
 __m128i r18 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+18*16));
 __m128i r19 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+19*16));
 __m128i r20 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+20*16));
 __m128i r21 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+21*16));
 __m128i r22 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+22*16));
 __m128i r23 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+23*16));
 __m128i r24 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+24*16));
 __m128i r25 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+25*16));
 __m128i r26 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+26*16));
 __m128i r27 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+27*16));
 __m128i r28 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+28*16));
 __m128i r29 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+29*16));
 __m128i r30 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+30*16));
 __m128i r31 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+31*16));
 __m128i r32 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+32*16));
 __m128i r33 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+33*16));
 __m128i r34 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+34*16));
 __m128i r35 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+35*16));
 __m128i r36 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+36*16));
 __m128i r37 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+37*16));
 __m128i r38 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+38*16));
 __m128i r39 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+39*16));
 __m128i r40 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+40*16));
 __m128i r41 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+41*16));
 __m128i r42 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+42*16));
 __m128i r43 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+43*16));
 __m128i r44 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+44*16));
 __m128i r45 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+45*16));
 __m128i r46 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+46*16));
 __m128i r47 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+47*16));
 __m128i r48 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+48*16));
 __m128i r49 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+49*16));
 __m128i r50 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+50*16));
 __m128i r51 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+51*16));
 __m128i r52 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+52*16));
 __m128i r53 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+53*16));
 __m128i r54 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+54*16));
 __m128i r55 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+55*16));
 __m128i r56 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+56*16));
 __m128i r57 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+57*16));
 __m128i r58 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+58*16));
 __m128i r59 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+59*16));
 __m128i r60 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+60*16));
 __m128i r61 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+61*16));
 __m128i r62 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+62*16));
 __m128i r63 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+63*16));
 __m128i r64 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+64*16));
 __m128i r65 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+65*16));
 __m128i r66 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+66*16));
 __m128i r67 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+67*16));
 __m128i r68 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+68*16));
 __m128i r69 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+69*16));
 __m128i r70 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+70*16));
 __m128i r71 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+71*16));
 __m128i r72 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+72*16));
 __m128i r73 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+73*16));
 __m128i r74 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+74*16));
 __m128i r75 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+75*16));
 __m128i r76 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+76*16));
 __m128i r77 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+77*16));
 __m128i r78 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+78*16));
 __m128i r79 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(static_cast<unsigned char*>(context)+64+79*16));
 __m128i saved_mask_7574,saved_mask_7630,saved_mask_76cc;
 bool guest_stores_started=false;
 unsigned outcome=1;
 const uint32_t return_pc=word(r0)&0x3fffc;
r64 = _mm_mullo_epi32(_mm_and_si128(r3, _mm_set1_epi32(65535)),_mm_and_si128(r4,_mm_set1_epi32(65535))); //0x7448 MPYU
{auto loaded=load_ls(ls,pc+9720);auto mismatch=_mm_xor_si128(loaded,constant<3,2,1,0,3,2,1,0,3,2,1,0,3,2,1,0>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r13 = constant<3,2,1,0,3,2,1,0,3,2,1,0,3,2,1,0>(); //0x744c LQR
r65 = _mm_set1_epi32(3229614080u); //0x7450 ILHU
{auto loaded=load_ls(ls,pc+9656);auto mismatch=_mm_xor_si128(loaded,constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r14 = constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(); //0x7454 LQR
r51 = _mm_set1_epi32(-176); //0x7458 IL
{auto loaded=load_ls(ls,pc+9640);auto mismatch=_mm_xor_si128(loaded,constant<11,10,9,8,11,10,9,8,11,10,9,8,11,10,9,8>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r16 = constant<11,10,9,8,11,10,9,8,11,10,9,8,11,10,9,8>(); //0x745c LQR
r43 = _mm_castps_si128(_mm_mul_ps(_mm_castsi128_ps(r9),_mm_castsi128_ps(r65))); //0x7460 FM
r63 = load_ls(ls,word(r11)+(0)); //0x7464 LQD
{auto loaded=load_ls(ls,pc+9624);auto mismatch=_mm_xor_si128(loaded,constant<15,14,13,12,15,14,13,12,15,14,13,12,15,14,13,12>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r15 = constant<15,14,13,12,15,14,13,12,15,14,13,12,15,14,13,12>(); //0x7468 LQR
{auto loaded=load_ls(ls,pc+9704);auto mismatch=_mm_xor_si128(loaded,constant<128,128,128,128,128,128,128,128,19,18,17,16,3,2,1,0>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r17 = constant<128,128,128,128,128,128,128,128,19,18,17,16,3,2,1,0>(); //0x746c LQR
r57 = load_ls(ls,word(r12)+(0)); //0x7474 LQD
r62 = _mm_mullo_epi32(_mm_and_si128(r64,_mm_set1_epi32(65535)),_mm_set1_epi32(44)); //0x7478 MPYUI
r18 = shuffle_constant<3,2,1,0,3,2,1,0,3,2,1,0,3,2,1,0>(r4,r4); //0x747c SHUFB
r60 = _mm_mullo_epi32(_mm_and_si128(r64,_mm_set1_epi32(65535)),_mm_set1_epi32(4)); //0x7480 MPYUI
r28 = shuffle_constant<3,2,1,0,3,2,1,0,3,2,1,0,3,2,1,0>(r10,r10); //0x7484 SHUFB
r24 = shuffle_constant<3,2,1,0,3,2,1,0,3,2,1,0,3,2,1,0>(r63,r63); //0x7488 SHUFB
r23 = shuffle_constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(r63,r63); //0x748c SHUFB
r22 = shuffle_constant<11,10,9,8,11,10,9,8,11,10,9,8,11,10,9,8>(r63,r63); //0x7490 SHUFB
r33 = shuffle_constant<3,2,1,0,3,2,1,0,3,2,1,0,3,2,1,0>(r43,r43); //0x7494 SHUFB
r32 = shuffle_constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(r43,r43); //0x749c SHUFB
r58 = _mm_add_epi32(r6,r62); //0x74a0 A
r30 = shuffle_constant<11,10,9,8,11,10,9,8,11,10,9,8,11,10,9,8>(r43,r43); //0x74a4 SHUFB
r62 = _mm_set1_epi32(-16); //0x74a8 IL
r29 = shuffle_constant<15,14,13,12,15,14,13,12,15,14,13,12,15,14,13,12>(r43,r43); //0x74ac SHUFB
r61 = _mm_add_epi32(r5,r60); //0x74b0 A
r21 = shuffle_constant<3,2,1,0,3,2,1,0,3,2,1,0,3,2,1,0>(r57,r57); //0x74b4 SHUFB
r63 = shuffle_constant<128,128,128,128,128,128,128,128,19,18,17,16,3,2,1,0>(r62,r51); //0x74b8 SHUFB
r31 = shuffle_constant<128,128,128,128,128,128,128,128,19,18,17,16,3,2,1,0>(r61,r58); //0x74bc SHUFB
r25 = shuffle_constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(r57,r57); //0x74c0 SHUFB
r26 = shuffle_constant<11,10,9,8,11,10,9,8,11,10,9,8,11,10,9,8>(r57,r57); //0x74c4 SHUFB
r27 = shuffle_constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(r10,r10); //0x74c8 SHUFB
r34 = shuffle_constant<11,10,9,8,11,10,9,8,11,10,9,8,11,10,9,8>(r10,r10); //0x74cc SHUFB
r38 = shuffle_constant<15,14,13,12,15,14,13,12,15,14,13,12,15,14,13,12>(r10,r10); //0x74d0 SHUFB
{auto loaded=load_ls(ls,pc+9832);auto mismatch=_mm_xor_si128(loaded,constant<255,255,255,0,255,255,255,0,255,255,255,0,255,255,255,0>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r19 = constant<255,255,255,0,255,255,255,0,255,255,255,0,255,255,255,0>(); //0x74d4 LQR
{auto loaded=load_ls(ls,pc+9896);auto mismatch=_mm_xor_si128(loaded,constant<15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r20 = constant<15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0>(); //0x74d8 LQR
{auto loaded=load_ls(ls,pc+9768);auto mismatch=_mm_xor_si128(loaded,constant<31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r37 = constant<31,30,29,28,27,26,25,24,23,22,21,20,19,18,17,16>(); //0x74dc LQR
{auto loaded=load_ls(ls,pc+9752);auto mismatch=_mm_xor_si128(loaded,constant<0,0,128,62,0,0,0,63,0,0,64,63,0,0,128,63>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r36 = constant<0,0,128,62,0,0,0,63,0,0,64,63,0,0,128,63>(); //0x74e0 LQR
{auto loaded=load_ls(ls,pc+9736);auto mismatch=_mm_xor_si128(loaded,constant<14,13,12,128,10,9,8,128,6,5,4,128,2,1,0,128>());if(!_mm_testz_si128(mismatch,mismatch))return 0;}
r35 = constant<14,13,12,128,10,9,8,128,6,5,4,128,2,1,0,128>(); //0x74e4 LQR
L74e8:;
if (checkpoint(context,(pc+160)&0x3fffc)) { if(!guest_stores_started)return 0;const uint32_t resume=(pc+160)&0x3fffc; std::memcpy(static_cast<unsigned char*>(context)+24,&resume,4); outcome=2; goto spill; }
r18 = _mm_add_epi32(r18,_mm_set1_epi32(-1)); //0x74e8 AI
r48 = _mm_add_epi32(r31,r63); //0x74ec A
r47 = _mm_castps_si128(_mm_cvtepu32_ps(r18)); //0x74f0 CUFLT
r31 = _mm_add_epi32(r48,r63); //0x74f8 A
r46 = load_ls(ls,word(r48)+(0)); //0x74fc LQD
r44 = _mm_add_epi32(r3,_mm_set1_epi32(-8)); //0x7500 AI
r40 = shuffle_constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(r48,r48); //0x7504 SHUFB
r49 = load_ls(ls,word(r31)+(0)); //0x750c LQD
r42 = _mm_castps_si128(fused(_mm_castsi128_ps(r47),_mm_castsi128_ps(r8),_mm_castsi128_ps(r7))); //0x7510 FMA
r45 = shuffle_constant<14,13,12,128,10,9,8,128,6,5,4,128,2,1,0,128>(r46,r46); //0x7514 SHUFB
r4 = _mm_castps_si128(_mm_cvtepu32_ps(r45)); //0x7518 CUFLT
r56 = _mm_cmpeq_epi32(r45,r19); //0x7520 CEQ
r41 = shuffle_constant<15,14,13,12,15,14,13,12,15,14,13,12,15,14,13,12>(r42,r42); //0x7524 SHUFB
r39 = shuffle_constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(r42,r42); //0x7528 SHUFB
r17 = shuffle_constant<3,2,1,0,3,2,1,0,3,2,1,0,3,2,1,0>(r42,r42); //0x752c SHUFB
r10 = shuffle_constant<11,10,9,8,11,10,9,8,11,10,9,8,11,10,9,8>(r42,r42); //0x7530 SHUFB
r47 = _mm_castps_si128(fused(_mm_castsi128_ps(r36),_mm_castsi128_ps(r29),_mm_castsi128_ps(r41))); //0x7534 FMA
r6 = _mm_castps_si128(fused(_mm_castsi128_ps(r36),_mm_castsi128_ps(r32),_mm_castsi128_ps(r39))); //0x7538 FMA
r46 = _mm_castps_si128(fused(_mm_castsi128_ps(r36),_mm_castsi128_ps(r33),_mm_castsi128_ps(r17))); //0x753c FMA
r45 = _mm_castps_si128(fused(_mm_castsi128_ps(r36),_mm_castsi128_ps(r30),_mm_castsi128_ps(r10))); //0x7540 FMA
r48 = _mm_castps_si128(fused(_mm_castsi128_ps(r4),_mm_castsi128_ps(r38),_mm_castsi128_ps(r47))); //0x7544 FMA
r41 = _mm_castps_si128(fused(_mm_castsi128_ps(r4),_mm_castsi128_ps(r27),_mm_castsi128_ps(r6))); //0x7548 FMA
r9 = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r6),_mm_castsi128_ps(r32))); //0x754c FA
r43 = _mm_castps_si128(fused(_mm_castsi128_ps(r4),_mm_castsi128_ps(r28),_mm_castsi128_ps(r46))); //0x7550 FMA
r42 = _mm_castps_si128(fused(_mm_castsi128_ps(r4),_mm_castsi128_ps(r34),_mm_castsi128_ps(r45))); //0x7558 FMA
r50 = reciprocal_estimate(r48); //0x755c FREST
L7568:;
if (checkpoint(context,(pc+288)&0x3fffc)) { if(!guest_stores_started)return 0;const uint32_t resume=(pc+288)&0x3fffc; std::memcpy(static_cast<unsigned char*>(context)+24,&resume,4); outcome=2; goto spill; }
r47 = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r47),_mm_castsi128_ps(r29))); //0x7568 FA
r53 = shuffle_constant<14,13,12,128,10,9,8,128,6,5,4,128,2,1,0,128>(r49,r49); //0x756c SHUFB
r66 = interpolate(r48,r50); //0x7570 FI
saved_mask_7574 = r56;
r52 = select(r20,r37,r56); //0x7574 SELB
r51 = _mm_castps_si128(_mm_cvtepu32_ps(r53)); //0x7578 CUFLT
r44 = _mm_add_epi32(r44,_mm_set1_epi32(-4)); //0x757c AI
r46 = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r46),_mm_castsi128_ps(r33))); //0x7580 FA
r45 = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r45),_mm_castsi128_ps(r30))); //0x7584 FA
r54 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r43),_mm_castsi128_ps(r66))); //0x7588 FM
r75 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r42),_mm_castsi128_ps(r66))); //0x758c FM
r59 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r41),_mm_castsi128_ps(r66))); //0x7590 FM
r48 = _mm_castps_si128(fused(_mm_castsi128_ps(r51),_mm_castsi128_ps(r38),_mm_castsi128_ps(r47))); //0x7594 FMA
r43 = _mm_castps_si128(fused(_mm_castsi128_ps(r51),_mm_castsi128_ps(r28),_mm_castsi128_ps(r46))); //0x7598 FMA
r42 = _mm_castps_si128(fused(_mm_castsi128_ps(r51),_mm_castsi128_ps(r34),_mm_castsi128_ps(r45))); //0x759c FMA
r58 = greater_finite_point(r54,r21); //0x75a0 FCGT
guest_stores_started=true;
store_ls(ls,word(r40)+(0),r54); //0x75a4
r57 = greater_finite_point(r54,r24); //0x75a8 FCGT
store_ls(ls,word(r40)+(32),r75); //0x75ac
r17 = greater_finite_point(r75,r22); //0x75b0 FCGT
store_ls(ls,word(r40)+(16),r59); //0x75b4
r39 = greater_finite_point(r75,r26); //0x75b8 FCGT
r62 = greater_finite_point(r59,r23); //0x75bc FCGT
r64 = select(r75,r22,r17); //0x75c0 SELB
r56 = select(r26,r75,r39); //0x75c8 SELB
r40 = shuffle_constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(r31,r31); //0x75cc SHUFB
r31 = _mm_add_epi32(r31,r63); //0x75d0 A
r50 = reciprocal_estimate(r48); //0x75d4 FREST
r55 = greater_finite_point(r59,r25); //0x75d8 FCGT
r22 = select(r64,r22,saved_mask_7574); //0x75dc SHUFB
r60 = select(r59,r23,r62); //0x75e0 SELB
r49 = load_ls(ls,word(r31)+(0)); //0x75e4 LQD
r61 = select(r21,r54,r58); //0x75e8 SELB
r26 = select(r56,r26,saved_mask_7574); //0x75ec SHUFB
r66 = select(r54,r24,r57); //0x75f0 SELB
r23 = select(r60,r23,saved_mask_7574); //0x75f4 SHUFB
r54 = select(r25,r59,r55); //0x75f8 SELB
r21 = select(r61,r21,saved_mask_7574); //0x75fc SHUFB
r41 = _mm_castps_si128(fused(_mm_castsi128_ps(r51),_mm_castsi128_ps(r27),_mm_castsi128_ps(r9))); //0x7600 FMA
r24 = select(r66,r24,saved_mask_7574); //0x7604 SHUFB
r56 = _mm_cmpeq_epi32(r53,r19); //0x7608 CEQ
r25 = select(r54,r25,saved_mask_7574); //0x760c SHUFB
r9 = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r9),_mm_castsi128_ps(r32))); //0x7610 FA
if (word(r44) != 0) goto L7568;
r6 = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r47),_mm_castsi128_ps(r29))); //0x7620 FA
r62 = shuffle_constant<14,13,12,128,10,9,8,128,6,5,4,128,2,1,0,128>(r49,r49); //0x7624 SHUFB
r55 = interpolate(r48,r50); //0x7628 FI
r66 = shuffle_constant<7,6,5,4,7,6,5,4,7,6,5,4,7,6,5,4>(r31,r31); //0x762c SHUFB
saved_mask_7630 = r56;
r5 = select(r20,r37,r56); //0x7630 SELB
r4 = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r46),_mm_castsi128_ps(r33))); //0x7634 FA
r65 = _mm_castps_si128(_mm_cvtepu32_ps(r62)); //0x7638 CUFLT
r2 = _mm_cmpeq_epi32(r62,r19); //0x763c CEQ
r79 = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(r45),_mm_castsi128_ps(r30))); //0x7640 FA
r76 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r42),_mm_castsi128_ps(r55))); //0x7644 FM
r75 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r41),_mm_castsi128_ps(r55))); //0x7648 FM
r77 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r43),_mm_castsi128_ps(r55))); //0x764c FM
r74 = _mm_castps_si128(fused(_mm_castsi128_ps(r65),_mm_castsi128_ps(r38),_mm_castsi128_ps(r6))); //0x7650 FMA
r73 = _mm_castps_si128(fused(_mm_castsi128_ps(r65),_mm_castsi128_ps(r28),_mm_castsi128_ps(r4))); //0x7654 FMA
r72 = _mm_castps_si128(fused(_mm_castsi128_ps(r65),_mm_castsi128_ps(r34),_mm_castsi128_ps(r79))); //0x7658 FMA
r71 = _mm_castps_si128(fused(_mm_castsi128_ps(r65),_mm_castsi128_ps(r27),_mm_castsi128_ps(r9))); //0x7660 FMA
store_ls(ls,word(r40)+(32),r76); //0x7664
r67 = greater_finite_point(r76,r26); //0x7668 FCGT
store_ls(ls,word(r40)+(16),r75); //0x766c
r68 = greater_finite_point(r76,r22); //0x7670 FCGT
store_ls(ls,word(r40)+(0),r77); //0x7674
r67 = select(r26,r76,r67); //0x7678 SELB
r10 = reciprocal_estimate(r74); //0x767c FREST
r65 = greater_finite_point(r75,r23); //0x7680 FCGT
r39 = select(r76,r22,r68); //0x7684 SELB
r70 = greater_finite_point(r77,r21); //0x7688 FCGT
r17 = interpolate(r74,r10); //0x768c FI
r69 = greater_finite_point(r77,r24); //0x7690 FCGT
r10 = select(r67,r26,saved_mask_7630); //0x7694 SHUFB
r64 = greater_finite_point(r75,r25); //0x7698 FCGT
r74 = select(r39,r22,saved_mask_7630); //0x769c SHUFB
r68 = select(r75,r23,r65); //0x76a0 SELB
r76 = select(r21,r77,r70); //0x76a4 SELB
r41 = select(r77,r24,r69); //0x76a8 SELB
r40 = select(r68,r23,saved_mask_7630); //0x76ac SHUFB
r61 = select(r25,r75,r64); //0x76b0 SELB
r57 = select(r76,r21,saved_mask_7630); //0x76b4 SHUFB
r60 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r73),_mm_castsi128_ps(r17))); //0x76b8 FM
r58 = select(r41,r24,saved_mask_7630); //0x76bc SHUFB
r65 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r72),_mm_castsi128_ps(r17))); //0x76c0 FM
r59 = select(r61,r25,saved_mask_7630); //0x76c4 SHUFB
r67 = _mm_castps_si128(multiply_refined(_mm_castsi128_ps(r71),_mm_castsi128_ps(r17))); //0x76c8 FM
saved_mask_76cc = r2;
r64 = select(r20,r37,r2); //0x76cc SELB
r70 = greater_finite_point(r60,r57); //0x76d0 FCGT
store_ls(ls,word(r66)+(0),r60); //0x76d4
r68 = greater_finite_point(r60,r58); //0x76d8 FCGT
store_ls(ls,word(r66)+(32),r65); //0x76dc
r76 = greater_finite_point(r65,r74); //0x76e0 FCGT
store_ls(ls,word(r66)+(16),r67); //0x76e4
r77 = greater_finite_point(r65,r10); //0x76e8 FCGT
r75 = greater_finite_point(r67,r40); //0x76ec FCGT
r69 = greater_finite_point(r67,r59); //0x76f0 FCGT
r72 = select(r65,r74,r76); //0x76f4 SELB
r73 = select(r10,r65,r77); //0x76f8 SELB
r71 = select(r67,r40,r75); //0x7700 SELB
r22 = select(r72,r74,saved_mask_76cc); //0x7704 SHUFB
r2 = select(r57,r60,r70); //0x7708 SELB
r26 = select(r73,r10,saved_mask_76cc); //0x770c SHUFB
r5 = select(r60,r58,r68); //0x7710 SELB
r23 = select(r71,r40,saved_mask_76cc); //0x7714 SHUFB
r9 = select(r59,r67,r69); //0x7718 SELB
r21 = select(r2,r57,saved_mask_76cc); //0x771c SHUFB
r24 = select(r5,r58,saved_mask_76cc); //0x7720 SHUFB
r25 = select(r9,r59,saved_mask_76cc); //0x7724 SHUFB
if (word(r18) != 0) goto L74e8;
r74 = load_ls(ls,pc+9848); //0x772c LQR
r73 = load_ls(ls,pc+9816); //0x7730 LQR
r72 = load_ls(ls,pc+9880); //0x7734 LQR
r71 = load_ls(ls,pc+9800); //0x7738 LQR
r70 = load_ls(ls,pc+9864); //0x773c LQR
r69 = load_ls(ls,pc+9784); //0x7740 LQR
r68 = shuffle(r24,r23,r74); //0x7744 SHUFB
r75 = shuffle(r24,r23,r73); //0x7748 SHUFB
r76 = shuffle(r21,r25,r74); //0x774c SHUFB
r77 = shuffle(r21,r25,r73); //0x7750 SHUFB
r78 = shuffle(r68,r22,r72); //0x7754 SHUFB
r2 = shuffle(r68,r22,r71); //0x7758 SHUFB
r79 = shuffle(r75,r22,r70); //0x775c SHUFB
r3 = shuffle(r75,r22,r69); //0x7760 SHUFB
r5 = shuffle(r76,r26,r72); //0x7764 SHUFB
r4 = greater(r78,r2); //0x7768 FCGT
r6 = shuffle(r76,r26,r71); //0x776c SHUFB
r8 = shuffle(r77,r26,r70); //0x7774 SHUFB
r7 = greater(r79,r3); //0x7778 FCGT
r9 = shuffle(r77,r26,r69); //0x777c SHUFB
r13 = select(r78,r2,r4); //0x7780 SELB
r10 = greater(r5,r6); //0x7784 FCGT
r15 = select(r79,r3,r7); //0x7788 SELB
r14 = greater(r8,r9); //0x778c FCGT
r16 = select(r6,r5,r10); //0x7790 SELB
r18 = select(r9,r8,r14); //0x7794 SELB
r19 = greater(r13,r15); //0x7798 FCGT
r17 = greater(r16,r18); //0x779c FCGT
r20 = select(r13,r15,r19); //0x77a0 SELB
r21 = select(r18,r16,r17); //0x77a8 SELB
store_ls(ls,word(r11)+(0),r20); //0x77ac
store_ls(ls,word(r12)+(0),r21); //0x77b0
std::memcpy(static_cast<unsigned char*>(context)+24, &return_pc, 4);
spill:;
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+2*16),r2);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+3*16),r3);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+4*16),r4);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+5*16),r5);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+6*16),r6);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+7*16),r7);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+8*16),r8);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+9*16),r9);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+10*16),r10);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+13*16),r13);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+14*16),r14);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+15*16),r15);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+16*16),r16);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+17*16),r17);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+18*16),r18);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+19*16),r19);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+20*16),r20);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+21*16),r21);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+22*16),r22);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+23*16),r23);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+24*16),r24);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+25*16),r25);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+26*16),r26);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+27*16),r27);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+28*16),r28);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+29*16),r29);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+30*16),r30);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+31*16),r31);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+32*16),r32);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+33*16),r33);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+34*16),r34);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+35*16),r35);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+36*16),r36);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+37*16),r37);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+38*16),r38);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+39*16),r39);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+40*16),r40);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+41*16),r41);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+42*16),r42);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+43*16),r43);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+44*16),r44);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+45*16),r45);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+46*16),r46);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+47*16),r47);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+48*16),r48);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+49*16),r49);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+50*16),r50);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+51*16),r51);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+52*16),r52);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+53*16),r53);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+54*16),r54);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+55*16),r55);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+56*16),r56);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+57*16),r57);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+58*16),r58);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+59*16),r59);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+60*16),r60);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+61*16),r61);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+62*16),r62);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+63*16),r63);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+64*16),r64);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+65*16),r65);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+66*16),r66);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+67*16),r67);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+68*16),r68);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+69*16),r69);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+70*16),r70);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+71*16),r71);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+72*16),r72);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+73*16),r73);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+74*16),r74);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+75*16),r75);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+76*16),r76);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+77*16),r77);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+78*16),r78);
 _mm_storeu_si128(reinterpret_cast<__m128i*>(static_cast<unsigned char*>(context)+64+79*16),r79);
return outcome;
}
struct Region {uint32_t b,e;};
static bool overlap(Region a,Region b){return a.b<b.e && b.b<a.e;}
static uint32_t preferred(const unsigned char* c,unsigned r){uint32_t x;std::memcpy(&x,c+64+r*16+12,4);return x;}
static bool region(uint32_t b,uint64_t length,Region& out){if((b&15)||b>=262144||length>262144-b)return false;out={b,b+uint32_t(length)};return true;}
inline bool eligible(void* context,void* store,uint32_t pc){
 auto* c=static_cast<unsigned char*>(context);
 if((_mm_getcsr()&0xe040)!=0xe040)return false; // RZ, DAZ, FTZ.
 if((pc&3)||pc>262144-9896-16)return false; // Keep every PC-relative LQR pool within LS.
 uint32_t width=preferred(c,3),height=preferred(c,4);
 if(width<12 || width>128 || width%4 || height<1 || height>128)return false;
 // CUFLT heights are at most24bits. These coefficients bound every pre-divide
 // numerator/denominator below2^35; FI therefore returns finite, nonzero-normal.
 for(unsigned r=7;r<=10;++r)for(unsigned lane=0;lane<4;++lane){float v;std::memcpy(&v,c+64+r*16+lane*4,4);if(!std::isfinite(v)||std::fabs(v)>1024.f)return false;}
 Region input,output,lower,upper;
 if(!region(preferred(c,5),uint64_t(width)*height*4,input)||!region(preferred(c,6),uint64_t(width)*height*44,output)||!region(preferred(c,11),16,lower)||!region(preferred(c,12),16,upper))return false;
 if(overlap(input,output)||overlap(input,lower)||overlap(input,upper)||overlap(output,lower)||overlap(output,upper)||overlap(lower,upper))return false;
 struct Mask {uint32_t offset;unsigned char bytes[16];};
 static constexpr Mask masks[]={
 {9720,{0,1,2,3,0,1,2,3,0,1,2,3,0,1,2,3}},
 {9656,{4,5,6,7,4,5,6,7,4,5,6,7,4,5,6,7}},
 {9640,{8,9,10,11,8,9,10,11,8,9,10,11,8,9,10,11}},
 {9624,{12,13,14,15,12,13,14,15,12,13,14,15,12,13,14,15}},
 {9704,{0,1,2,3,16,17,18,19,128,128,128,128,128,128,128,128}},
 {9832,{0,255,255,255,0,255,255,255,0,255,255,255,0,255,255,255}},
 {9896,{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}},
 {9768,{16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31}},
 {9752,{63,128,0,0,63,64,0,0,63,0,0,0,62,128,0,0}},
 {9736,{128,0,1,2,128,4,5,6,128,8,9,10,128,12,13,14}},
 {9848,{0,1,2,3,16,17,18,19,8,9,10,11,24,25,26,27}},
 {9816,{4,5,6,7,20,21,22,23,12,13,14,15,28,29,30,31}},
 {9880,{0,1,2,3,4,5,6,7,16,17,18,19,192,192,192,192}},
 {9800,{8,9,10,11,12,13,14,15,20,21,22,23,192,192,192,192}},
 {9864,{0,1,2,3,4,5,6,7,24,25,26,27,192,192,192,192}},
 {9784,{8,9,10,11,12,13,14,15,28,29,30,31,192,192,192,192}},
 };
 for(const auto& m:masks){uint32_t a=(pc+m.offset)&0x3fff0;Region pool{a,a+16};if(overlap(pool,output)||overlap(pool,lower)||overlap(pool,upper))return false;}
 return true;
}

} // namespace rpcs3::experimental_rwv
#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
