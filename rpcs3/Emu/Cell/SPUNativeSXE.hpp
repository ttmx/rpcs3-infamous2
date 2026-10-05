#pragma once
#include <immintrin.h>
#include <cstdint>
#include <cstring>
#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx512f,avx512vl,avx512dq,avx512bw,fma"))), apply_to=function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("avx512f,avx512vl,avx512dq,avx512bw,fma")
#endif
namespace rpcs3::experimental_sxe {
enum Outcome : unsigned {SPU_NATIVE_UNSUPPORTED=0,SPU_NATIVE_COMPLETED=1,SPU_NATIVE_INTERRUPTED=2};
using R=__m128i;
inline R splat(uint32_t x){return _mm_set1_epi32(x);}
inline __m128 fp(R x){return _mm_castsi128_ps(x);}
inline R bits(__m128 x){return _mm_castps_si128(x);}
inline __m128 clamp(__m128 x){return _mm_range_ps(x,fp(splat(0x7f7fffff)),2);}
inline R reverse(R x){return _mm_shuffle_epi8(x,_mm_setr_epi8(15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0));}
inline uint32_t pref(R x){return uint32_t(_mm_extract_epi32(x,3));}
inline R lq(const uint8_t* ls,uint32_t address){return reverse(_mm_load_si128(reinterpret_cast<const R*>(ls+(address&0x3fff0))));}
inline void sq(uint8_t* ls,uint32_t address,R value){_mm_store_si128(reinterpret_cast<R*>(ls+(address&0x3fff0)),reverse(value));}
inline R shuf(R a,R b,R c){
 const R idx=_mm_xor_si128(c,_mm_set1_epi8(15));
 const auto pick_b=_mm_cmp_epi8_mask(_mm_and_si128(c,_mm_set1_epi8(16)),_mm_setzero_si128(),_MM_CMPINT_NE);
 const R shuffled=_mm_mask_blend_epi8(pick_b,_mm_shuffle_epi8(a,idx),_mm_shuffle_epi8(b,idx));
 const auto ff=_mm_cmp_epu8_mask(c,_mm_set1_epi8(char(0xc0)),_MM_CMPINT_GE);
 const auto high=_mm_cmp_epu8_mask(c,_mm_set1_epi8(char(0xe0)),_MM_CMPINT_GE);
 const R constants=_mm_mask_blend_epi8(high,_mm_maskz_set1_epi8(ff,char(0xff)),_mm_set1_epi8(char(0x80)));
 return _mm_mask_mov_epi8(shuffled,_mm_movepi8_mask(c),constants);
}
alignas(64) inline constexpr uint32_t reciprocal_lut[32]={
 8387552,8357798,7401330,7375680,6523666,6503146,5739204,5720736,5032574,5018210,4393540,4380202,3812882,3800570,3282404,3273170,
 2795966,2786732,2348442,2342286,1934710,1934710,1551706,1551706,1197379,1197379,866605,866605,558362,558362,269575,269575};
inline R frest(R a){
 const R index=_mm_and_si128(_mm_srli_epi32(a,18),splat(31));
 const R fraction=_mm512_castsi512_si128(_mm512_permutex2var_epi32(_mm512_load_si512(reciprocal_lut),_mm512_zextsi128_si512(index),_mm512_load_si512(reciprocal_lut+16)));
 const R exponent=_mm_and_si128(a,splat(0x7f800000));
 const R sub=_mm_subs_epu16(splat(0x7e800000),exponent);
 const R exp=_mm_mask_blend_epi32(_mm_cmpeq_epi32_mask(exponent,_mm_setzero_si128()),sub,splat(0x7f800000));
 return _mm_or_si128(fraction,_mm_or_si128(exp,_mm_and_si128(a,splat(0x80000000))));
}
inline R fi(R a,R b){
 const R base=_mm_slli_epi32(_mm_and_si128(b,splat(0x7ffc00)),9);
 const R product=_mm_mullo_epi32(_mm_and_si128(b,splat(0x3ff)),_mm_and_si128(a,splat(0x7ffff)));
 const auto adjust=_mm_cmp_epu32_mask(product,base,_MM_CMPINT_GT);
 const R fraction=_mm_srlv_epi32(_mm_sub_epi32(base,product),_mm_mask_blend_epi32(adjust,splat(9),splat(8)));
 const R exp=_mm_sub_epi32(_mm_and_si128(b,splat(0xff800000)),_mm_maskz_set1_epi32(adjust,0x800000));
 return _mm_or_si128(bits(clamp(fp(exp))),_mm_and_si128(fraction,splat(0x7fffff)));
}
inline R fm(R a,R b){
 const auto nonzero=_mm_cmp_ps_mask(fp(a),_mm_setzero_ps(),_CMP_NEQ_UQ)&_mm_cmp_ps_mask(fp(b),_mm_setzero_ps(),_CMP_NEQ_UQ);
 return bits(_mm_maskz_mov_ps(nonzero,_mm_mul_ps(fp(a),fp(b))));
}
inline R fma(R a,R b,R c){
 return bits(_mm_fmadd_ps(_mm_fixupimm_ps(fp(a),fp(b),splat(0x800),0),_mm_fixupimm_ps(fp(b),fp(a),splat(0x800),0),fp(c)));
}
inline R fnms(R a,R b,R c){
 return bits(_mm_fmadd_ps(_mm_xor_ps(clamp(fp(a)),fp(splat(0x80000000))),clamp(fp(b)),fp(c)));
}
// This routine's compares use a constant zero first operand, matching emitted
// ordered compare against the clamped second operand.
inline R fcgt(R a,R b){return _mm_maskz_set1_epi32(_mm_cmp_ps_mask(fp(a),clamp(fp(b)),_CMP_GT_OQ),-1);}
struct WideReciprocal {__m512i estimate;__m512i refined;};
inline WideReciprocal reciprocal4(R a0,R a1,R a2,R a3){
 __m512i a=_mm512_zextsi128_si512(a0);a=_mm512_inserti32x4(a,a1,1);a=_mm512_inserti32x4(a,a2,2);a=_mm512_inserti32x4(a,a3,3);
 const auto i=[](uint32_t x){return _mm512_set1_epi32(x);};
 const auto index=_mm512_and_si512(_mm512_srli_epi32(a,18),i(31));
 const auto fraction=_mm512_permutex2var_epi32(_mm512_load_si512(reciprocal_lut),index,_mm512_load_si512(reciprocal_lut+16));
 const auto exponent=_mm512_and_si512(a,i(0x7f800000));
 const auto exp=_mm512_mask_blend_epi32(_mm512_cmpeq_epi32_mask(exponent,_mm512_setzero_si512()),_mm512_subs_epu16(i(0x7e800000),exponent),i(0x7f800000));
 const auto estimate=_mm512_or_si512(fraction,_mm512_or_si512(exp,_mm512_and_si512(a,i(0x80000000))));
 const auto base=_mm512_slli_epi32(_mm512_and_si512(estimate,i(0x7ffc00)),9);
 const auto product=_mm512_mullo_epi32(_mm512_and_si512(estimate,i(0x3ff)),_mm512_and_si512(a,i(0x7ffff)));
 const auto adjust=_mm512_cmp_epu32_mask(product,base,_MM_CMPINT_GT);
 const auto refined_fraction=_mm512_srlv_epi32(_mm512_sub_epi32(base,product),_mm512_mask_blend_epi32(adjust,i(9),i(8)));
 const auto adjusted_exp=_mm512_sub_epi32(_mm512_and_si512(estimate,i(0xff800000)),_mm512_maskz_set1_epi32(adjust,0x800000));
 const auto clamped_exp=_mm512_castps_si512(_mm512_range_ps(_mm512_castsi512_ps(adjusted_exp),_mm512_castsi512_ps(i(0x7f7fffff)),2));
 return {estimate,_mm512_or_si512(clamped_exp,_mm512_and_si512(refined_fraction,i(0x7fffff)))};
}
// Only four selectors in the decoded chunk; exact byte-pool equality is checked
// before using word permutations. Pool loads remain to preserve final GPRs.
inline R shuf_mask_15(R a,R){return _mm_shuffle_epi32(a,_MM_SHUFFLE(1,1,0,0));}
inline R shuf_mask_61(R,R b){return _mm_shuffle_epi32(b,_MM_SHUFFLE(3,3,2,2));}
inline R shuf_mask_16(R a,R b){return _mm_permutex2var_epi32(a,_mm_setr_epi32(7,1,0,2),b);}
inline R shuf_mask_14(R a,R b){return _mm_permutex2var_epi32(a,_mm_setr_epi32(5,7,6,0),b);}
inline bool masks_match_registers(R a,R b,R c,R d){
 const R ca=_mm_setr_epi32(0x0c0d0e0f,0x0c0d0e0f,0x08090a0b,0x08090a0b);
 const R cb=_mm_setr_epi32(0x10111213,0x08090a0b,0x0c0d0e0f,0x04050607);
 const R cc=_mm_setr_epi32(0x18191a1b,0x10111213,0x14151617,0x0c0d0e0f);
 const R cd=_mm_setr_epi32(0x14151617,0x14151617,0x10111213,0x10111213);
 return _mm_test_all_zeros(_mm_or_si128(_mm_or_si128(_mm_xor_si128(a,ca),_mm_xor_si128(b,cb)),_mm_or_si128(_mm_xor_si128(c,cc),_mm_xor_si128(d,cd))),_mm_set1_epi32(-1));
}

template<typename Check> unsigned spu_native_sxe_impl(void* context,void* local_store,uint32_t runtime_pc,Check interrupt){
 auto* ctx=static_cast<uint8_t*>(context);auto* ls=static_cast<uint8_t*>(local_store);
 uint32_t count;std::memcpy(&count,ctx+64+11*16+12,4);
 if ((runtime_pc&3) || runtime_pc>0x3f7a0 || !count || (count&3) || count>128 || (_mm_getcsr()&0xe040)!=0xe040 || interrupt(runtime_pc,false)) return SPU_NATIVE_UNSUPPORTED;
 alignas(16) R r[128];std::memcpy(r,ctx+64,sizeof(r));
 auto spill=[&](uint32_t pc){
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+2*16),r[2]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+3*16),r[3]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+4*16),r[4]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+5*16),r[5]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+6*16),r[6]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+7*16),r[7]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+8*16),r[8]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+9*16),r[9]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+10*16),r[10]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+11*16),r[11]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+13*16),r[13]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+14*16),r[14]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+15*16),r[15]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+16*16),r[16]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+17*16),r[17]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+18*16),r[18]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+19*16),r[19]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+20*16),r[20]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+21*16),r[21]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+22*16),r[22]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+23*16),r[23]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+24*16),r[24]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+25*16),r[25]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+26*16),r[26]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+27*16),r[27]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+28*16),r[28]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+29*16),r[29]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+30*16),r[30]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+31*16),r[31]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+32*16),r[32]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+33*16),r[33]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+34*16),r[34]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+35*16),r[35]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+36*16),r[36]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+37*16),r[37]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+38*16),r[38]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+39*16),r[39]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+40*16),r[40]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+41*16),r[41]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+42*16),r[42]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+43*16),r[43]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+44*16),r[44]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+45*16),r[45]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+46*16),r[46]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+47*16),r[47]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+48*16),r[48]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+49*16),r[49]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+50*16),r[50]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+51*16),r[51]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+52*16),r[52]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+53*16),r[53]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+54*16),r[54]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+55*16),r[55]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+56*16),r[56]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+57*16),r[57]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+58*16),r[58]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+59*16),r[59]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+60*16),r[60]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+61*16),r[61]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+62*16),r[62]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+63*16),r[63]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+64*16),r[64]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+65*16),r[65]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+66*16),r[66]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+67*16),r[67]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+68*16),r[68]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+69*16),r[69]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+70*16),r[70]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+71*16),r[71]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+72*16),r[72]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+73*16),r[73]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+74*16),r[74]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+75*16),r[75]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+76*16),r[76]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+77*16),r[77]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+78*16),r[78]);
 _mm_store_si128(reinterpret_cast<R*>(ctx+64+79*16),r[79]);
 std::memcpy(ctx+24,&pc,4);};
 r[77]=_mm_add_epi32(r[9],r[5]); // 042c0 A
 r[15]=lq(ls,runtime_pc+uint32_t(2112)); // 042c4 LQR
 r[18]=_mm_add_epi32(r[5],splat(uint32_t(32))); // 042c8 AI
 r[16]=lq(ls,runtime_pc+uint32_t(2128)); // 042cc LQR
 r[17]=_mm_add_epi32(r[77],splat(uint32_t(32))); // 042d0 AI
 r[14]=lq(ls,runtime_pc+uint32_t(2096)); // 042d4 LQR
 r[75]=_mm_sub_epi32(r[4],r[8]); // 042d8 SF
 r[48]=lq(ls,pref(r[18])+uint32_t(-32)); // 042dc LQD
 r[32]=_mm_add_epi32(r[8],r[4]); // 042e0 A
 r[44]=lq(ls,pref(r[17])+uint32_t(-32)); // 042e4 LQD
 r[19]=_mm_add_epi32(r[75],splat(uint32_t(16))); // 042e8 AI
 r[47]=lq(ls,pref(r[18])+uint32_t(-16)); // 042ec LQD
 r[20]=_mm_add_epi32(r[4],splat(uint32_t(16))); // 042f0 AI
 r[50]=lq(ls,pref(r[17])+uint32_t(-16)); // 042f4 LQD
 r[24]=splat(0x3a6b0000u); // 042f8 ILHU
 r[41]=lq(ls,pref(r[19])+uint32_t(0)); // 042fc LQD
 r[23]=splat(0x399d0000u); // 04300 ILHU
 r[78]=lq(ls,pref(r[19])+uint32_t(-16)); // 04304 LQD
 r[22]=splat(0x38d10000u); // 04308 ILHU
 r[30]=lq(ls,pref(r[19])+uint32_t(-32)); // 0430c LQD
 r[21]=_mm_add_epi32(r[32],splat(uint32_t(16))); // 04310 AI
 r[73]=frest(r[44]); // 04314 FREST
 r[24]=_mm_or_si128(r[24],splat(0xedfau)); // 04318 IOHL
 r[66]=frest(r[48]); // 0431c FREST
 r[23]=_mm_or_si128(r[23],splat(0x4952u)); // 04320 IOHL
 r[71]=frest(r[47]); // 04324 FREST
 r[22]=_mm_or_si128(r[22],splat(0xb717u)); // 04328 IOHL
 r[29]=lq(ls,pref(r[20])+uint32_t(-32)); // 0432c LQD
 r[39]=fi(r[44],r[73]); // 04330 FI
 r[42]=shuf(r[78],r[41],r[16]); // 04334 SHUFB
 r[9]=fi(r[48],r[66]); // 04338 FI
 r[54]=shuf(r[78],r[41],r[15]); // 0433c SHUFB
 r[66]=splat(0x00000000u); // 04340 ILHU
 r[37]=shuf(r[30],r[78],r[14]); // 04344 SHUFB
 r[58]=fi(r[47],r[71]); // 04348 FI
 r[28]=lq(ls,pref(r[20])+uint32_t(-16)); // 0434c LQD
 r[25]=_mm_sub_epi32(r[3],r[7]); // 04350 SF
 r[33]=lq(ls,pref(r[20])+uint32_t(0)); // 04354 LQD
 r[26]=_mm_add_epi32(r[7],r[3]); // 04358 A
 r[62]=lq(ls,runtime_pc+uint32_t(2064)); // 0435c LQR
 r[27]=_mm_add_epi32(r[10],r[6]); // 04360 A
 r[61]=lq(ls,runtime_pc+uint32_t(2080)); // 04364 LQR
 r[41]=fm(r[12],r[39]); // 04368 FM
 r[67]=splat(0x3f800000u); // 0436c ILHU
 r[65]=splat(0x41100000u); // 04370 ILHU
 r[64]=splat(0x40400000u); // 04374 ILHU
 r[63]=splat(0x3f800000u); // 04378 ILHU
 const bool fixed_masks=masks_match_registers(r[15],r[16],r[14],r[61]);
 if (!fixed_masks) return SPU_NATIVE_UNSUPPORTED;
 auto run_loop=[&]()->unsigned {
 R denominators[4];bool progressed=false;
 do {
 if (interrupt(runtime_pc+0xc0,true)) {if(!progressed)return SPU_NATIVE_UNSUPPORTED;spill(runtime_pc+0xc0);return SPU_NATIVE_INTERRUPTED;}
 r[77]=bits(_mm_sub_ps(fp(r[42]),clamp(fp(r[47])))); // 04380 FS
 r[60]=lq(ls,pref(r[21])+uint32_t(0)); // 04384 LQD
 r[3]=_mm_add_epi32(r[3],splat(uint32_t(16))); // 04388 AI
 r[76]=lq(ls,pref(r[21])+uint32_t(-16)); // 0438c LQD
 r[59]=bits(_mm_sub_ps(fp(r[37]),clamp(fp(r[48])))); // 04390 FS
 r[78]=shuf_mask_61(r[30],r[78]); // 04394 SHUFB
 r[26]=_mm_add_epi32(r[26],splat(uint32_t(16))); // 04398 AI
 r[25]=_mm_add_epi32(r[25],splat(uint32_t(16))); // 043a0 AI
 r[70]=shuf_mask_16(r[28],r[33]); // 043a4 SHUFB
 r[69]=bits(_mm_sub_ps(fp(r[54]),clamp(fp(r[47])))); // 043a8 FS
 r[71]=shuf_mask_61(r[29],r[28]); // 043ac SHUFB
 r[55]=bits(_mm_sub_ps(fp(r[78]),clamp(fp(r[48])))); // 043b0 FS
 r[68]=shuf_mask_14(r[29],r[28]); // 043b4 SHUFB
 r[51]=fm(r[12],r[9]); // 043b8 FM
 r[54]=shuf_mask_16(r[28],r[33]); // 043bc SHUFB
 r[57]=bits(_mm_sub_ps(fp(r[70]),clamp(fp(r[50])))); // 043c0 FS
 r[52]=bits(_mm_sub_ps(fp(r[71]),clamp(fp(r[48])))); // 043c8 FS
 r[31]=shuf_mask_16(r[76],r[60]); // 043cc SHUFB
 r[53]=bits(_mm_sub_ps(fp(r[68]),clamp(fp(r[44])))); // 043d0 FS
 r[37]=shuf_mask_15(r[28],r[33]); // 043d4 SHUFB
 r[35]=_mm_and_si128(r[77],r[62]); // 043d8 AND
 r[75]=lq(ls,pref(r[21])+uint32_t(-32)); // 043dc LQD
 r[56]=fm(r[12],r[58]); // 043e0 FM
 r[7]=shuf_mask_15(r[76],r[60]); // 043e4 SHUFB
 r[38]=_mm_and_si128(r[59],r[62]); // 043e8 AND
 r[34]=shuf_mask_61(r[29],r[28]); // 043ec SHUFB
 r[49]=_mm_and_si128(r[57],r[62]); // 043f0 AND
 r[10]=_mm_and_si128(r[55],r[62]); // 043f4 AND
 r[4]=bits(_mm_sub_ps(fp(r[54]),clamp(fp(r[47])))); // 043f8 FS
 r[36]=_mm_and_si128(r[53],r[62]); // 04400 AND
 r[32]=shuf_mask_61(r[75],r[76]); // 04404 SHUFB
 r[9]=fnms(r[38],r[51],r[67]); // 04408 FNMS
 r[72]=bits(_mm_sub_ps(fp(r[37]),clamp(fp(r[47])))); // 0440c FS
 r[78]=fnms(r[36],r[41],r[67]); // 04410 FNMS
 r[30]=shuf_mask_15(r[28],r[33]); // 04414 SHUFB
 r[5]=fnms(r[35],r[56],r[67]); // 04418 FNMS
 r[71]=bits(_mm_sub_ps(fp(r[34]),clamp(fp(r[44])))); // 04420 FS
 r[79]=shuf_mask_14(r[29],r[28]); // 04424 SHUFB
 r[68]=bits(_mm_sub_ps(fp(r[32]),clamp(fp(r[44])))); // 04428 FS
 r[77]=bits(_mm_sub_ps(fp(r[31]),clamp(fp(r[50])))); // 0442c FS
 r[73]=fcgt(r[66],r[9]); // 04430 FCGT
 r[70]=frest(r[50]); // 04434 FREST
 r[55]=bits(_mm_sub_ps(fp(r[30]),clamp(fp(r[50])))); // 04438 FS
 r[8]=_mm_or_si128(_mm_andnot_si128(r[73],r[9]),_mm_and_si128(r[73],r[66])); // 0443c SELB
 r[45]=fnms(r[10],r[51],r[67]); // 04440 FNMS
 r[60]=fma(r[8],r[63],r[22]); // 04444 FMA
 r[8]=fcgt(r[66],r[5]); // 04448 FCGT
 r[74]=bits(_mm_sub_ps(fp(r[7]),clamp(fp(r[50])))); // 0444c FS
 r[7]=_mm_or_si128(_mm_andnot_si128(r[8],r[5]),_mm_and_si128(r[8],r[66])); // 04450 SELB
 r[5]=_mm_and_si128(r[4],r[62]); // 04454 AND
 r[59]=fma(r[7],r[63],r[22]); // 04458 FMA
 r[4]=fcgt(r[66],r[78]); // 0445c FCGT
 r[73]=fnms(r[5],r[56],r[67]); // 04460 FNMS
 r[28]=_mm_or_si128(_mm_andnot_si128(r[4],r[78]),_mm_and_si128(r[4],r[66])); // 04468 SELB
 r[10]=shuf_mask_14(r[75],r[76]); // 0446c SHUFB
 r[29]=bits(_mm_sub_ps(fp(r[79]),clamp(fp(r[48])))); // 04470 FS
 r[58]=fma(r[28],r[64],r[23]); // 04474 FMA
 r[54]=_mm_and_si128(r[77],r[62]); // 04478 AND
 r[53]=_mm_and_si128(r[74],r[62]); // 0447c AND
 r[39]=fi(r[50],r[70]); // 04480 FI
 r[32]=_mm_and_si128(r[69],r[62]); // 04484 AND
 r[48]=fcgt(r[66],r[73]); // 04488 FCGT
 r[40]=_mm_and_si128(r[29],r[62]); // 0448c AND
 r[50]=_mm_and_si128(r[72],r[62]); // 04490 AND
 r[44]=bits(_mm_sub_ps(fp(r[10]),clamp(fp(r[44])))); // 04494 FS
 r[74]=_mm_or_si128(_mm_andnot_si128(r[48],r[73]),_mm_and_si128(r[48],r[66])); // 04498 SELB
 r[47]=_mm_and_si128(r[71],r[62]); // 0449c AND
 r[57]=fma(r[74],r[64],r[23]); // 044a0 FMA
 r[43]=fnms(r[50],r[56],r[67]); // 044a4 FNMS
 r[31]=fm(r[12],r[39]); // 044a8 FM
 r[46]=fnms(r[40],r[51],r[67]); // 044ac FNMS
 r[42]=_mm_and_si128(r[68],r[62]); // 044b0 AND
 r[68]=fnms(r[32],r[56],r[67]); // 044b4 FNMS
 r[30]=_mm_and_si128(r[44],r[62]); // 044b8 AND
 r[70]=_mm_and_si128(r[55],r[62]); // 044bc AND
 r[56]=fnms(r[47],r[41],r[67]); // 044c0 FNMS
 r[72]=fnms(r[54],r[31],r[67]); // 044c4 FNMS
 r[75]=_mm_and_si128(r[52],r[62]); // 044c8 AND
 r[69]=fcgt(r[66],r[45]); // 044cc FCGT
 r[71]=fnms(r[75],r[51],r[67]); // 044d0 FNMS
 r[55]=fnms(r[53],r[31],r[67]); // 044d4 FNMS
 r[54]=fnms(r[70],r[31],r[67]); // 044d8 FNMS
 r[73]=fcgt(r[66],r[72]); // 044dc FCGT
 r[4]=fnms(r[49],r[31],r[67]); // 044e0 FNMS
 r[79]=_mm_or_si128(_mm_andnot_si128(r[73],r[72]),_mm_and_si128(r[73],r[66])); // 044e4 SELB
 r[72]=fcgt(r[66],r[46]); // 044e8 FCGT
 r[53]=fma(r[79],r[63],r[22]); // 044f0 FMA
 r[52]=lq(ls,pref(r[3])+uint32_t(-32)); // 044f4 LQD
 r[32]=fnms(r[30],r[41],r[67]); // 044f8 FNMS
 r[51]=lq(ls,pref(r[3])+uint32_t(0)); // 044fc LQD
 r[74]=_mm_or_si128(_mm_andnot_si128(r[72],r[46]),_mm_and_si128(r[72],r[66])); // 04500 SELB
 r[50]=lq(ls,pref(r[25])+uint32_t(0)); // 04504 LQD
 r[72]=fcgt(r[66],r[43]); // 04508 FCGT
 r[49]=lq(ls,pref(r[3])+uint32_t(-16)); // 0450c LQD
 r[70]=_mm_or_si128(_mm_andnot_si128(r[69],r[45]),_mm_and_si128(r[69],r[66])); // 04510 SELB
 r[48]=lq(ls,pref(r[25])+uint32_t(-16)); // 04514 LQD
 r[47]=fma(r[74],r[64],r[23]); // 04518 FMA
 r[46]=lq(ls,pref(r[25])+uint32_t(-32)); // 0451c LQD
 r[74]=_mm_or_si128(_mm_andnot_si128(r[72],r[43]),_mm_and_si128(r[72],r[66])); // 04520 SELB
 r[45]=lq(ls,pref(r[26])+uint32_t(-32)); // 04524 LQD
 r[44]=fnms(r[42],r[41],r[67]); // 04528 FNMS
 r[43]=lq(ls,pref(r[26])+uint32_t(-16)); // 0452c LQD
 r[33]=fcgt(r[66],r[32]); // 04530 FCGT
 r[42]=lq(ls,pref(r[26])+uint32_t(0)); // 04534 LQD
 r[72]=fcgt(r[66],r[71]); // 04538 FCGT
 r[41]=shuf_mask_16(r[49],r[51]); // 0453c SHUFB
 r[5]=fcgt(r[66],r[4]); // 04540 FCGT
 r[40]=shuf_mask_15(r[49],r[51]); // 04544 SHUFB
 r[8]=_mm_or_si128(_mm_andnot_si128(r[33],r[32]),_mm_and_si128(r[33],r[66])); // 04548 SELB
 r[39]=shuf_mask_14(r[52],r[49]); // 0454c SHUFB
 r[69]=fcgt(r[66],r[68]); // 04550 FCGT
 r[38]=shuf_mask_61(r[52],r[49]); // 04554 SHUFB
 r[37]=fma(r[8],r[63],r[22]); // 04558 FMA
 r[36]=shuf_mask_61(r[52],r[49]); // 0455c SHUFB
 r[35]=fma(r[74],r[65],r[24]); // 04560 FMA
 r[34]=shuf_mask_16(r[49],r[51]); // 04564 SHUFB
 r[79]=_mm_or_si128(_mm_andnot_si128(r[69],r[68]),_mm_and_si128(r[69],r[66])); // 04568 SELB
 r[33]=shuf_mask_16(r[48],r[50]); // 0456c SHUFB
 r[73]=_mm_or_si128(_mm_andnot_si128(r[5],r[4]),_mm_and_si128(r[5],r[66])); // 04570 SELB
 r[32]=fma(r[79],r[64],r[23]); // 04574 FMA
 r[31]=fcgt(r[66],r[56]); // 04578 FCGT
 r[30]=fma(r[73],r[64],r[23]); // 0457c FMA
 r[29]=_mm_or_si128(_mm_andnot_si128(r[72],r[71]),_mm_and_si128(r[72],r[66])); // 04580 SELB
 r[28]=fma(r[70],r[64],r[23]); // 04584 FMA
 r[68]=fcgt(r[66],r[44]); // 04588 FCGT
 r[69]=fma(r[29],r[65],r[24]); // 04590 FMA
 r[74]=_mm_or_si128(_mm_andnot_si128(r[68],r[44]),_mm_and_si128(r[68],r[66])); // 04598 SELB
 r[75]=_mm_or_si128(_mm_andnot_si128(r[31],r[56]),_mm_and_si128(r[31],r[66])); // 0459c SELB
 r[76]=fcgt(r[66],r[54]); // 045a0 FCGT
 r[77]=fma(r[74],r[64],r[23]); // 045a4 FMA
 r[78]=fcgt(r[66],r[55]); // 045a8 FCGT
 r[7]=_mm_or_si128(_mm_andnot_si128(r[76],r[54]),_mm_and_si128(r[76],r[66])); // 045ac SELB
 r[8]=fma(r[75],r[65],r[24]); // 045b0 FMA
 r[9]=shuf_mask_15(r[49],r[51]); // 045b4 SHUFB
 r[79]=_mm_or_si128(_mm_andnot_si128(r[78],r[55]),_mm_and_si128(r[78],r[66])); // 045b8 SELB
 r[10]=fma(r[7],r[65],r[24]); // 045bc FMA
 r[29]=fma(r[79],r[64],r[23]); // 045c0 FMA
 r[31]=fm(r[35],r[9]); // 045c4 FM
 r[44]=bits(_mm_add_ps(fp(r[69]),fp(r[28]))); // 045c8 FA
 r[51]=bits(_mm_add_ps(fp(r[35]),fp(r[32]))); // 045cc FA
 r[54]=bits(_mm_add_ps(fp(r[8]),fp(r[77]))); // 045d0 FA
 r[70]=fm(r[69],r[36]); // 045d4 FM
 r[71]=fm(r[8],r[38]); // 045d8 FM
 r[72]=shuf_mask_15(r[48],r[50]); // 045dc SHUFB
 r[73]=bits(_mm_add_ps(fp(r[10]),fp(r[29]))); // 045e0 FA
 r[75]=fm(r[10],r[40]); // 045e8 FM
 r[76]=shuf_mask_61(r[45],r[43]); // 045ec SHUFB
 r[78]=bits(_mm_add_ps(fp(r[47]),fp(r[44]))); // 045f0 FA
 r[4]=fma(r[32],r[72],r[31]); // 045f8 FMA
 r[5]=shuf_mask_15(r[43],r[42]); // 045fc SHUFB
 r[7]=bits(_mm_add_ps(fp(r[58]),fp(r[54]))); // 04600 FA
 r[9]=fma(r[77],r[76],r[71]); // 04608 FMA
 r[10]=shuf_mask_61(r[46],r[48]); // 0460c SHUFB
 r[31]=bits(_mm_add_ps(fp(r[30]),fp(r[73]))); // 04610 FA
 r[35]=fma(r[29],r[5],r[75]); // 04614 FMA
 r[36]=bits(_mm_add_ps(fp(r[57]),fp(r[51]))); // 04618 FA
 r[38]=fma(r[28],r[10],r[70]); // 0461c FMA
 r[40]=bits(_mm_add_ps(fp(r[37]),fp(r[7]))); // 04620 FA
 r[17]=_mm_add_epi32(r[17],splat(uint32_t(32))); // 04624 AI
 r[39]=fma(r[58],r[39],r[9]); // 04628 FMA
 r[9]=shuf_mask_14(r[52],r[49]); // 0462c SHUFB
 r[49]=bits(_mm_add_ps(fp(r[53]),fp(r[31]))); // 04630 FA
 r[68]=fma(r[30],r[41],r[35]); // 04638 FMA
 r[73]=shuf_mask_14(r[45],r[43]); // 0463c SHUFB
 r[69]=bits(_mm_add_ps(fp(r[59]),fp(r[36]))); // 04640 FA
 r[74]=fma(r[47],r[9],r[38]); // 04648 FMA
 denominators[0]=r[40];
 r[75]=bits(_mm_add_ps(fp(r[60]),fp(r[78]))); // 04650 FA
 r[79]=shuf_mask_16(r[43],r[42]); // 04654 SHUFB
 r[5]=fma(r[37],r[73],r[39]); // 04658 FMA
 r[44]=lq(ls,pref(r[17])+uint32_t(-32)); // 0465c LQD
 r[18]=_mm_add_epi32(r[18],splat(uint32_t(32))); // 04660 AI
 denominators[1]=r[49];
 r[8]=fma(r[57],r[34],r[4]); // 04668 FMA
 r[32]=shuf_mask_14(r[46],r[48]); // 0466c SHUFB
 r[47]=lq(ls,pref(r[18])+uint32_t(-16)); // 04674 LQD
 r[19]=_mm_add_epi32(r[19],splat(uint32_t(16))); // 04678 AI
 denominators[2]=r[69];
 r[36]=fma(r[53],r[79],r[68]); // 04680 FMA
 r[48]=lq(ls,pref(r[18])+uint32_t(-32)); // 04684 LQD
 denominators[3]=r[75];
 r[2]=_mm_add_epi32(r[2],splat(uint32_t(32))); // 04690 AI
 r[31]=lq(ls,pref(r[19])+uint32_t(0)); // 04694 LQD
 r[39]=fma(r[60],r[32],r[74]); // 04698 FMA
 r[78]=lq(ls,pref(r[19])+uint32_t(-16)); // 0469c LQD
 r[43]=fma(r[59],r[33],r[8]); // 046a0 FMA
 r[50]=frest(r[44]); // 046a4 FREST
 const auto re=reciprocal4(denominators[0],denominators[1],denominators[2],denominators[3]);
 r[76]=_mm512_extracti32x4_epi32(re.estimate,0);
 r[7]=_mm512_extracti32x4_epi32(re.estimate,1);
 r[35]=_mm512_extracti32x4_epi32(re.estimate,2);
 r[40]=_mm512_extracti32x4_epi32(re.estimate,3);
 r[34]=_mm512_extracti32x4_epi32(re.refined,0);
 r[38]=_mm512_extracti32x4_epi32(re.refined,1);
 r[49]=_mm512_extracti32x4_epi32(re.refined,2);
 r[52]=_mm512_extracti32x4_epi32(re.refined,3);
 r[30]=lq(ls,pref(r[19])+uint32_t(-32)); // 046b4 LQD
 r[20]=_mm_add_epi32(r[20],splat(uint32_t(16))); // 046b8 AI
 r[45]=frest(r[47]); // 046bc FREST
 r[46]=fm(r[5],r[34]); // 046c0 FM
 r[51]=frest(r[48]); // 046c4 FREST
 r[55]=fm(r[36],r[38]); // 046c8 FM
 r[42]=shuf_mask_16(r[78],r[31]); // 046cc SHUFB
 r[53]=fi(r[44],r[50]); // 046d0 FI
 r[50]=lq(ls,pref(r[17])+uint32_t(-16)); // 046d4 LQD
 r[27]=_mm_add_epi32(r[27],splat(uint32_t(32))); // 046d8 AI
 r[29]=lq(ls,pref(r[20])+uint32_t(-32)); // 046dc LQD
 r[56]=fm(r[43],r[49]); // 046e0 FM
 r[28]=lq(ls,pref(r[20])+uint32_t(-16)); // 046e4 LQD
 r[57]=fm(r[39],r[52]); // 046e8 FM
 r[37]=shuf_mask_14(r[30],r[78]); // 046ec SHUFB
 r[21]=_mm_add_epi32(r[21],splat(uint32_t(16))); // 046f0 AI
 r[33]=lq(ls,pref(r[20])+uint32_t(0)); // 046f4 LQD
 r[6]=_mm_add_epi32(r[6],splat(uint32_t(32))); // 046f8 AI
 sq(ls,pref(r[27])+uint32_t(-16),r[55]); // 046fc STQD
 r[58]=fi(r[47],r[45]); // 04700 FI
 sq(ls,pref(r[27])+uint32_t(-32),r[46]); // 04704 STQD
 r[9]=fi(r[48],r[51]); // 04708 FI
 r[54]=shuf_mask_15(r[78],r[31]); // 0470c SHUFB
 r[11]=_mm_add_epi32(r[11],splat(uint32_t(-4))); // 04710 AI
 sq(ls,pref(r[6])+uint32_t(-16),r[56]); // 04714 STQD
 r[41]=fm(r[12],r[53]); // 04718 FM
 sq(ls,pref(r[6])+uint32_t(-32),r[57]); // 0471c STQD
 r[13]=_mm_add_epi32(r[13],splat(uint32_t(32))); // 04720 AI
 progressed=true; } while(pref(r[11]));
 spill(pref(r[0])&0x3fffc);return SPU_NATIVE_COMPLETED;
 };
 return run_loop();
}

} // namespace rpcs3::experimental_sxe
#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
