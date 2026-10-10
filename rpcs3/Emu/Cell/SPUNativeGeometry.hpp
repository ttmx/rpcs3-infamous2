#pragma once
// Host versions of the hot loops of the God of War III geometry job (BCES00510 1.03, the SPURS job that skins, culls
// and packs every draw's vertices; see the gow3 notes of the fork), and of one function of its noise task.
// RPCS3_SPU_NATIVE_GEOMETRY=1.
//
// A kernel replaces a region of the job's SPU code: the recompiler calls it when the region's first instruction is
// reached, with every register in the thread context. If it returns 1 the SPU code continues at the region's exit
// address: the kernel has done the region's work on the local store and set the registers that code after the region
// reads (the others the region would have written keep their old values). If it returns 0 nothing was changed and
// the region's SPU code runs. Each was written against the SPU code and checked in an offline
// replay of captured jobs (profiling/gow3/geom/kern.py). Results differ from the recompiler's in the last bits only.
#include "Emu/Cell/SPUThread.h"
#include "Emu/RSX/VK/VKLiveCtl.hpp"
#include <immintrin.h>
#include <atomic>
#include <bit>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>

#if defined(__clang__)
#pragma clang attribute push(__attribute__((target("avx2,fma,ssse3,sse4.1"))), apply_to = function)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC target("avx2,fma,ssse3,sse4.1")
#endif

namespace spu_native_geometry
{
	inline bool enabled()
	{
		static const bool on = []
		{
			const char* v = std::getenv("RPCS3_SPU_NATIVE_GEOMETRY");
			return v && v[0] != '0';
		}();

		return on;
	}

	// Live control 16 for comparisons inside one session: 1 = the kernels decline
	inline bool active()
	{
		return vk::live_ctl::get(16) != 1;
	}

	// Live control 16, bit 2: only the kernels of the bounding sphere job decline
	inline bool active_spheres()
	{
		return active() && !(vk::live_ctl::get(16) & 2);
	}

	// Diagnostic for checking a built kernel (RPCS3_SPU_KERNEL_CHECK=<directory>,<pc hex>): the state before each of the
	// first 200 runs of the kernel at that address as region-NNNN.bin (the region capture's format) and the local store
	// and registers after it as after-NNNN.bin, to compare with the SPU code's result in the offline interpreter.
	// Kernels that support it call before() once their inputs are accepted and after() when they are done.
	struct kernel_check
	{
		std::string dir;
		u32 pc = 0;

		static const kernel_check& get()
		{
			static const kernel_check instance = []
			{
				kernel_check r;
				if (const char* v = std::getenv("RPCS3_SPU_KERNEL_CHECK"))
				{
					const std::string text = v;
					if (const auto a = text.find(','); a != std::string::npos)
					{
						r.dir = text.substr(0, a);
						r.pc = static_cast<u32>(std::strtoul(text.c_str() + a + 1, nullptr, 16));
					}
				}
				return r;
			}();
			return instance;
		}

		static void write(const spu_thread* spu, const u8* ls, u32 pc, const char* name, u32 n)
		{
			if (FILE* f = std::fopen((get().dir + "/" + name + "-" + std::to_string(n + 10000).substr(1) + ".bin").c_str(), "wb"))
			{
				std::fwrite("SPUR", 4, 1, f);
				std::fwrite(&pc, 4, 1, f);
				std::fwrite(spu->gpr.data(), 16, 128, f);
				std::fwrite(ls, 1, SPU_LS_SIZE, f);
				std::fclose(f);
			}
		}

		// Returns the number of this run, or -1 for none
		static s32 before(const spu_thread* spu, const u8* ls, u32 pc)
		{
			if (get().pc != pc) [[likely]] return -1;
			static std::atomic<u32> s_runs{0};
			const u32 n = s_runs++;
			if (n >= 200) return -1;
			write(spu, ls, pc, "region", n);
			return static_cast<s32>(n);
		}

		static void after(const spu_thread* spu, const u8* ls, u32 pc, s32 n)
		{
			if (n >= 0) [[unlikely]] write(spu, ls, pc, "after", static_cast<u32>(n));
		}
	};

	// Preferred word of a register
	inline u32 word(const spu_thread& spu, u32 reg)
	{
		return spu.gpr[reg]._u32[3];
	}

	// Four big-endian floats of the local store as x, y, z, w and back
	inline __m128 load_be(const u8* p)
	{
		return _mm_castsi128_ps(_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p)), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
	}

	inline void store_be(u8* p, __m128 v)
	{
		_mm_storeu_si128(reinterpret_cast<__m128i*>(p), _mm_shuffle_epi8(_mm_castps_si128(v), _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12)));
	}

	// 0x10d74..0x10ff8: skinning of positions with up to four bones per vertex.
	// r63 vertex count, r83 influence records (8 bytes per vertex: four times weight byte, bone index byte), r86
	// positions (vec4, transformed in place), r67 output of the blended matrix of every vertex (48 bytes), r89 bones
	// (48 bytes each, three vec4 rows), r69 weight scale.
	// A bone's rows hold the 3x3 part in a rotated order and the translation in the three w components:
	//   out.x = x*r0.x + y*r1.x + z*r2.x + r0.w,  out.y = y*r0.y + z*r1.y + x*r2.y + r1.w,  out.z = z*r0.z + x*r1.z + y*r2.z + r2.w
	inline u32 skin_positions_weighted(spu_thread* spu, u8* ls)
	{
		if (!active()) return 0;

		const u32 count = word(*spu, 63);
		const u8* rec = ls + (word(*spu, 83) & 0x3fff0);
		u8* pos = ls + (word(*spu, 86) & 0x3fff0);
		u8* out = ls + (word(*spu, 67) & 0x3fff0);
		const u32 bones = word(*spu, 89);
		const __m128 scale = _mm_set1_ps(spu->gpr[69]._f[3]);
		const __m128 xyz = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
		const __m128 one_w = _mm_setr_ps(0.f, 0.f, 0.f, 1.f);

		const __m256i swap = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12, 3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);

		for (u32 v = 0; v < count; v++, rec += 8, pos += 16, out += 48)
		{
			// Rows 0 and 1 together, row 2 apart. An influence with weight zero adds nothing and is left out
			__m256 r01 = _mm256_setzero_ps();
			__m128 r2 = _mm_setzero_ps();

			for (u32 k = 0; k < 4; k++)
			{
				if (!rec[k * 2]) continue;

				const __m128 w = _mm_mul_ps(_mm_set1_ps(static_cast<float>(rec[k * 2])), scale);
				const u8* bone = ls + ((bones + rec[k * 2 + 1] * 48) & 0x3fff0);
				const __m256 m01 = _mm256_castsi256_ps(_mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(bone)), swap));
				r01 = _mm256_fmadd_ps(_mm256_set_m128(w, w), m01, r01);
				r2 = _mm_fmadd_ps(w, load_be(bone + 32), r2);
			}

			__m128 r0 = _mm256_castps256_ps128(r01), r1 = _mm256_extractf128_ps(r01, 1);

			// Translation: the w components of the three rows
			const __m128 t01 = _mm_shuffle_ps(r0, r1, _MM_SHUFFLE(3, 3, 3, 3));                        // r0.w r0.w r1.w r1.w
			const __m128 t = _mm_and_ps(_mm_shuffle_ps(t01, r2, _MM_SHUFFLE(0, 3, 2, 0)), xyz);          // r0.w r1.w r2.w 0

			r0 = _mm_or_ps(_mm_and_ps(r0, xyz), one_w);
			r1 = _mm_and_ps(r1, xyz);
			r2 = _mm_and_ps(r2, xyz);

			const __m128 p = load_be(pos);
			__m128 o = _mm_fmadd_ps(p, r0, t);
			o = _mm_fmadd_ps(_mm_shuffle_ps(p, p, _MM_SHUFFLE(3, 0, 2, 1)), r1, o);
			o = _mm_fmadd_ps(_mm_shuffle_ps(p, p, _MM_SHUFFLE(3, 1, 0, 2)), r2, o);

			store_be(out, r0);
			store_be(out + 16, r1);
			store_be(out + 32, r2);
			store_be(pos, o);
		}

		for (u32 i = 0; i < 4; i++)
		{
			spu->gpr[73]._u32[i] = spu->gpr[83]._u32[i] + (spu->gpr[63]._u32[i] << 3);
			spu->gpr[86]._u32[i] += count * 16;
		}

		return 1;
	}

	// 0x111bc..0x1160c: after the positions, one to three more vec4 streams of the same vertices (normals, tangents)
	// are transformed by the blended matrix that the position kernel stored per vertex, without the translation
	// (the stored rows have w = 1, 0, 0, so w passes through). r66 number of streams, r62, r65, r68 the streams,
	// r39 the matrices (r74 = the same plus four, as the SPU code reads them), r63 vertex count.
	inline u32 skin_streams(spu_thread* spu, u8* ls)
	{
		const u32 streams = word(*spu, 66);
		const u32 count = word(*spu, 63);
		const u32 matrices = word(*spu, 39) & 0x3fff0;

		if (!active() || !count || matrices + count * 48 > SPU_LS_SIZE || word(*spu, 74) != word(*spu, 39) + 192) return 0;

		if (streams < 1 || streams > 3) return 1; // the SPU code does nothing either

		static constexpr u32 regs[3]{62, 65, 68};

		for (u32 s = 0; s < streams; s++)
		{
			const u32 address = word(*spu, regs[s]) & 0x3fff0;
			if (address + count * 16 > SPU_LS_SIZE) return 0;
		}

		for (u32 s = 0; s < streams; s++)
		{
			u8* p = ls + (word(*spu, regs[s]) & 0x3fff0);
			const u8* m = ls + matrices;

			for (u32 v = 0; v < count; v++, p += 16, m += 48)
			{
				const __m128 x = load_be(p);
				__m128 o = _mm_mul_ps(x, load_be(m));
				o = _mm_fmadd_ps(_mm_shuffle_ps(x, x, _MM_SHUFFLE(3, 0, 2, 1)), load_be(m + 16), o);
				o = _mm_fmadd_ps(_mm_shuffle_ps(x, x, _MM_SHUFFLE(3, 1, 0, 2)), load_be(m + 32), o);
				store_be(p, o);
			}

			for (u32 i = 0; i < 4; i++) spu->gpr[regs[s]]._u32[i] += count * 16;
		}

		return 1;
	}

	// 0xdd50, the whole function: normalises the xyz of r4 vectors (rounded up to 8) at r3, w stays. The SPU code uses
	// the reciprocal square root estimate alone (about 12 bits); this is exact to the last bits. A zero vector
	// stays zero, as it does there (the estimate of zero is a large number, not infinity).
	inline u32 normalize(spu_thread* spu, u8* ls)
	{
		const u32 count = (word(*spu, 4) + 7) & ~7u;
		const u32 address = word(*spu, 3) & 0x3fff0;

		if (!active() || !count || address + count * 16 > SPU_LS_SIZE) return 0;

		const __m128 w_lane = _mm_castsi128_ps(_mm_setr_epi32(0, 0, 0, -1));

		for (u8* p = ls + address, *end = p + count * 16; p < end; p += 16)
		{
			const __m128 v = load_be(p);
			const __m128 d = _mm_dp_ps(v, v, 0x7f);
			__m128 r = _mm_rsqrt_ps(d);
			r = _mm_mul_ps(r, _mm_fnmadd_ps(_mm_mul_ps(_mm_set1_ps(0.5f), d), _mm_mul_ps(r, r), _mm_set1_ps(1.5f)));
			r = _mm_and_ps(r, _mm_cmpgt_ps(d, _mm_set1_ps(1e-37f)));
			store_be(p, _mm_blendv_ps(_mm_mul_ps(v, r), v, w_lane));
		}

		spu->pc = word(*spu, 0) & 0x3fffc;
		return 1;
	}

	// 0x11740, the whole function: destination[i] = matrix * source[i] for r3 vec4 (rounded up to 8); r4 source,
	// r5 destination (may be the source), r6 the 4x4 matrix, whose rows are dotted with the vector.
	inline u32 transform_by_matrix(spu_thread* spu, u8* ls)
	{
		const u32 count = (word(*spu, 3) + 7) & ~7u;
		const u32 source = word(*spu, 4) & 0x3fff0;
		const u32 destination = word(*spu, 5) & 0x3fff0;
		const u32 matrix = word(*spu, 6) & 0x3fff0;

		if (!active() || !count || source + count * 16 > SPU_LS_SIZE || destination + count * 16 > SPU_LS_SIZE || matrix + 64 > SPU_LS_SIZE) return 0;
		if (destination != source && destination < source + count * 16 && source < destination + count * 16) return 0;

		// The sums are formed as the SPU code forms them: component i starts with its diagonal term
		alignas(16) float m[4][4];
		for (u32 i = 0; i < 4; i++) _mm_store_ps(m[i], load_be(ls + matrix + i * 16));

		const __m128 a = _mm_setr_ps(m[0][0], m[1][1], m[2][2], m[3][3]);
		const __m128 b = _mm_setr_ps(m[0][1], m[1][2], m[2][3], m[3][0]);
		const __m128 c = _mm_setr_ps(m[0][2], m[1][3], m[2][0], m[3][1]);
		const __m128 d = _mm_setr_ps(m[0][3], m[1][0], m[2][1], m[3][2]);

		const u8* src = ls + source;
		u8* dst = ls + destination;

		for (u32 i = 0; i < count; i++, src += 16, dst += 16)
		{
			const __m128 v = load_be(src);
			__m128 o = _mm_mul_ps(v, a);
			o = _mm_fmadd_ps(_mm_shuffle_ps(v, v, _MM_SHUFFLE(0, 3, 2, 1)), b, o);
			o = _mm_fmadd_ps(_mm_shuffle_ps(v, v, _MM_SHUFFLE(1, 0, 3, 2)), c, o);
			o = _mm_fmadd_ps(_mm_shuffle_ps(v, v, _MM_SHUFFLE(2, 1, 0, 3)), d, o);
			store_be(dst, o);
		}

		spu->pc = word(*spu, 0) & 0x3fffc;
		return 1;
	}

	// 0xc168..0xc520: the third basis vector of every vertex: destination = normalize(cross(B, A) * B.w), w = 0, for r83
	// vertices (rounded up to 8); r81 = A, r82 = B, r45 = destination. The cross product is formed in the SPU code's
	// order, so that vertices whose A and B are parallel get the same rounding residue to normalise as there.
	inline u32 binormals(spu_thread* spu, u8* ls)
	{
		const s32 vertices = static_cast<s32>(word(*spu, 83));
		const u32 count = (static_cast<u32>(vertices) + 7) & ~7u;
		const u32 a_address = word(*spu, 81) & 0x3fff0, b_address = word(*spu, 82) & 0x3fff0, destination = word(*spu, 45) & 0x3fff0;

		if (!active() || vertices > 0x4000 || a_address + count * 16 > SPU_LS_SIZE || b_address + count * 16 > SPU_LS_SIZE || destination + count * 16 > SPU_LS_SIZE) return 0;
		if (vertices <= 0) return 1;

		const __m128 xyz = _mm_castsi128_ps(_mm_setr_epi32(-1, -1, -1, 0));
		const u8* pa = ls + a_address;
		const u8* pb = ls + b_address;
		u8* out = ls + destination;

		for (u32 i = 0; i < count; i++, pa += 16, pb += 16, out += 16)
		{
			const __m128 a = load_be(pa), b = load_be(pb);
			const __m128 t = _mm_mul_ps(_mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 1, 0, 2)));
			__m128 c = _mm_fnmadd_ps(_mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 1, 0, 2)), _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 0, 2, 1)), t);
			c = _mm_and_ps(_mm_mul_ps(c, _mm_shuffle_ps(b, b, _MM_SHUFFLE(3, 3, 3, 3))), xyz);
			const __m128 d = _mm_dp_ps(c, c, 0x7f);
			__m128 r = _mm_rsqrt_ps(d);
			r = _mm_mul_ps(r, _mm_fnmadd_ps(_mm_mul_ps(_mm_set1_ps(0.5f), d), _mm_mul_ps(r, r), _mm_set1_ps(1.5f)));
			r = _mm_and_ps(r, _mm_cmpgt_ps(d, _mm_set1_ps(1e-37f)));
			store_be(out, _mm_mul_ps(c, r));
		}

		return 1;
	}

	// Another program of the game (a SPURS task that fills a field with noise; about 6% of all SPU time in a fight),
	// function 0x4980: Perlin's improved noise at four points. r3..r6 hold one point each (x, y, z in the first three
	// components); the result is r3 with one value per point. The program's permutation table (words at 0x8400,
	// Perlin's own) and gradient table (16 vec4 at 0x8300, Perlin's except entries 12 and 14) are compared with the
	// copies here once; the SPU code runs if they ever differ.
	inline u32 noise4(spu_thread* spu, u8* ls)
	{
		static constexpr u8 perm_table[256]
		{
			151, 160, 137, 91, 90, 15, 131, 13, 201, 95, 96, 53, 194, 233, 7, 225, 140, 36, 103, 30, 69, 142, 8, 99, 37, 240, 21, 10, 23, 190, 6, 148,
			247, 120, 234, 75, 0, 26, 197, 62, 94, 252, 219, 203, 117, 35, 11, 32, 57, 177, 33, 88, 237, 149, 56, 87, 174, 20, 125, 136, 171, 168, 68, 175,
			74, 165, 71, 134, 139, 48, 27, 166, 77, 146, 158, 231, 83, 111, 229, 122, 60, 211, 133, 230, 220, 105, 92, 41, 55, 46, 245, 40, 244, 102, 143, 54,
			65, 25, 63, 161, 1, 216, 80, 73, 209, 76, 132, 187, 208, 89, 18, 169, 200, 196, 135, 130, 116, 188, 159, 86, 164, 100, 109, 198, 173, 186, 3, 64,
			52, 217, 226, 250, 124, 123, 5, 202, 38, 147, 118, 126, 255, 82, 85, 212, 207, 206, 59, 227, 47, 16, 58, 17, 182, 189, 28, 42, 223, 183, 170, 213,
			119, 248, 152, 2, 44, 154, 163, 70, 221, 153, 101, 155, 167, 43, 172, 9, 129, 22, 39, 253, 19, 98, 108, 110, 79, 113, 224, 232, 178, 185, 112, 104,
			218, 246, 97, 228, 251, 34, 242, 193, 238, 210, 144, 12, 191, 179, 162, 241, 81, 51, 145, 235, 249, 14, 239, 107, 49, 192, 214, 31, 181, 199, 106, 157,
			184, 84, 204, 176, 115, 121, 50, 45, 127, 4, 150, 254, 138, 236, 205, 93, 222, 114, 67, 29, 24, 72, 243, 141, 128, 195, 78, 66, 215, 61, 156, 180,
		};

		static constexpr float gradients[16][3]{{1.f, 1.f, 0.f}, {-1.f, 1.f, 0.f}, {1.f, -1.f, 0.f}, {-1.f, -1.f, 0.f}, {1.f, 0.f, 1.f}, {-1.f, 0.f, 1.f}, {1.f, 0.f, -1.f}, {-1.f, 0.f, -1.f}, {0.f, 1.f, 1.f}, {0.f, -1.f, 1.f}, {0.f, 1.f, -1.f}, {0.f, -1.f, -1.f}, {0.f, 1.f, 0.f}, {0.f, -1.f, 1.f}, {0.f, 1.f, 0.f}, {0.f, -1.f, -1.f}};

		// perm[i] for i up to 511, the table repeated as in the program
		static const struct tables_t
		{
			u8 perm[512];
			bool same;

			explicit tables_t(const u8* ls)
			{
				same = true;

				for (u32 i = 0; i < 512; i++)
				{
					perm[i] = perm_table[i & 255];
					be_t<u32> v;
					std::memcpy(&v, ls + 0x8400 + i * 4, 4);
					same &= v == perm[i];
				}

				for (u32 i = 0; i < 16; i++)
				{
					be_t<float> g[3];
					std::memcpy(g, ls + 0x8300 + i * 16, 12);
					same &= g[0] == gradients[i][0] && g[1] == gradients[i][1] && g[2] == gradients[i][2];
				}
			}
		} tables(ls);

		if (!active() || !tables.same) return 0;

		vk::live_ctl::probe_delay(2);

		const u8* const perm = tables.perm;
		const s32 check = kernel_check::before(spu, ls, 0x4980);

		// Live control 16, bit 4: the scalar version below (comparisons)
		if (!(vk::live_ctl::get(16) & 4))
		{
			// The gradient components are -1, 0 or 1: one byte table per component, looked up for all 32 corners at once
			static const struct gradient_bytes_t
			{
				alignas(16) s8 x[16], y[16], z[16];

				gradient_bytes_t()
				{
					for (u32 i = 0; i < 16; i++) x[i] = static_cast<s8>(gradients[i][0]), y[i] = static_cast<s8>(gradients[i][1]), z[i] = static_cast<s8>(gradients[i][2]);
				}
			} bytes;

			const __m128 px = _mm_setr_ps(spu->gpr[3]._f[3], spu->gpr[4]._f[3], spu->gpr[5]._f[3], spu->gpr[6]._f[3]);
			const __m128 py = _mm_setr_ps(spu->gpr[3]._f[2], spu->gpr[4]._f[2], spu->gpr[5]._f[2], spu->gpr[6]._f[2]);
			const __m128 pz = _mm_setr_ps(spu->gpr[3]._f[1], spu->gpr[4]._f[1], spu->gpr[5]._f[1], spu->gpr[6]._f[1]);

			// Every coordinate has to be an ordinary number that fits an integer
			const __m128 limit = _mm_set1_ps(1e9f), sign = _mm_set1_ps(-0.f);
			if (_mm_movemask_ps(_mm_and_ps(_mm_and_ps(_mm_cmplt_ps(_mm_andnot_ps(sign, px), limit), _mm_cmplt_ps(_mm_andnot_ps(sign, py), limit)), _mm_cmplt_ps(_mm_andnot_ps(sign, pz), limit))) != 15) return 0;

			const __m128 fx = _mm_floor_ps(px), fy = _mm_floor_ps(py), fz = _mm_floor_ps(pz);
			const __m128 tx = _mm_sub_ps(px, fx), ty = _mm_sub_ps(py, fy), tz = _mm_sub_ps(pz, fz);

			alignas(16) u32 cx[4], cy[4], cz[4];
			const __m128i low_byte = _mm_set1_epi32(255);
			_mm_store_si128(reinterpret_cast<__m128i*>(cx), _mm_and_si128(_mm_cvttps_epi32(fx), low_byte));
			_mm_store_si128(reinterpret_cast<__m128i*>(cy), _mm_and_si128(_mm_cvttps_epi32(fy), low_byte));
			_mm_store_si128(reinterpret_cast<__m128i*>(cz), _mm_and_si128(_mm_cvttps_epi32(fz), low_byte));

			// The corners' hashes, x fastest; a point's eight in one 64-bit value
			u64 hashes[4];

			for (u32 i = 0; i < 4; i++)
			{
				const u32 xi = cx[i], yi = cy[i], zi = cz[i];
				const u32 a = perm[xi] + yi, aa = perm[a] + zi, ab = perm[a + 1] + zi;
				const u32 b = perm[xi + 1] + yi, ba = perm[b] + zi, bb = perm[b + 1] + zi;

				hashes[i] = u64{perm[aa]} | u64{perm[ba]} << 8 | u64{perm[ab]} << 16 | u64{perm[bb]} << 24 |
					u64{perm[aa + 1]} << 32 | u64{perm[ba + 1]} << 40 | u64{perm[ab + 1]} << 48 | u64{perm[bb + 1]} << 56;
			}

			const __m256i h = _mm256_and_si256(_mm256_set_epi64x(hashes[3], hashes[2], hashes[1], hashes[0]), _mm256_set1_epi8(15));
			const __m256i gxb = _mm256_shuffle_epi8(_mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(bytes.x))), h);
			const __m256i gyb = _mm256_shuffle_epi8(_mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(bytes.y))), h);
			const __m256i gzb = _mm256_shuffle_epi8(_mm256_broadcastsi128_si256(_mm_load_si128(reinterpret_cast<const __m128i*>(bytes.z))), h);

			// fade(t) = t^3 (t (6 t - 15) + 10)
			const auto fade4 = [](__m128 t)
			{
				return _mm_mul_ps(_mm_mul_ps(_mm_mul_ps(t, t), t), _mm_fmadd_ps(t, _mm_fmsub_ps(t, _mm_set1_ps(6.f), _mm_set1_ps(15.f)), _mm_set1_ps(10.f)));
			};

			alignas(16) float u[4], v[4], w[4], x[4], y[4], z[4];
			_mm_store_ps(u, fade4(tx)), _mm_store_ps(v, fade4(ty)), _mm_store_ps(w, fade4(tz));
			_mm_store_ps(x, tx), _mm_store_ps(y, ty), _mm_store_ps(z, tz);

			const __m256 corner_x = _mm256_setr_ps(0, 1, 0, 1, 0, 1, 0, 1), corner_y = _mm256_setr_ps(0, 0, 1, 1, 0, 0, 1, 1), corner_z = _mm256_setr_ps(0, 0, 0, 0, 1, 1, 1, 1);

			for (u32 i = 0; i < 4; i++)
			{
				const auto eight = [&](__m256i all)
				{
					const __m128i half = i < 2 ? _mm256_castsi256_si128(all) : _mm256_extracti128_si256(all, 1);
					return _mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(i & 1 ? _mm_unpackhi_epi64(half, half) : half));
				};

				__m256 d = _mm256_mul_ps(eight(gxb), _mm256_sub_ps(_mm256_set1_ps(x[i]), corner_x));
				d = _mm256_fmadd_ps(eight(gyb), _mm256_sub_ps(_mm256_set1_ps(y[i]), corner_y), d);
				d = _mm256_fmadd_ps(eight(gzb), _mm256_sub_ps(_mm256_set1_ps(z[i]), corner_z), d);

				// Along x inside both halves, then y and z across them
				const __m256 even = _mm256_shuffle_ps(d, d, _MM_SHUFFLE(2, 0, 2, 0)), odd = _mm256_shuffle_ps(d, d, _MM_SHUFFLE(3, 1, 3, 1));
				const __m256 along_x = _mm256_fmadd_ps(_mm256_set1_ps(u[i]), _mm256_sub_ps(odd, even), even);
				const __m128 t = _mm_unpacklo_ps(_mm256_castps256_ps128(along_x), _mm256_extractf128_ps(along_x, 1));
				const __m128 far_y = _mm_movehl_ps(t, t);
				const __m128 along_y = _mm_fmadd_ps(_mm_set1_ps(v[i]), _mm_sub_ps(far_y, t), t);
				const float near_z = _mm_cvtss_f32(along_y), far_z = _mm_cvtss_f32(_mm_shuffle_ps(along_y, along_y, 1));
				spu->gpr[3]._f[3 - i] = near_z + w[i] * (far_z - near_z);
			}

			spu->pc = word(*spu, 0) & 0x3fffc;
			kernel_check::after(spu, ls, 0x4980, check);
			return 1;
		}

		const auto fade = [](float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); };
		const auto lerp = [](float t, float a, float b) { return a + t * (b - a); };

		float result[4];

		for (u32 i = 0; i < 4; i++)
		{
			const v128& point = spu->gpr[3 + i];
			float x = point._f[3], y = point._f[2], z = point._f[1];

			if (!(std::fabs(x) < 1e9f && std::fabs(y) < 1e9f && std::fabs(z) < 1e9f)) return 0;

			const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
			const u32 xi = static_cast<u32>(static_cast<s32>(fx)) & 255, yi = static_cast<u32>(static_cast<s32>(fy)) & 255, zi = static_cast<u32>(static_cast<s32>(fz)) & 255;
			x -= fx, y -= fy, z -= fz;

			const u32 a = perm[xi] + yi, aa = perm[a] + zi, ab = perm[a + 1] + zi;
			const u32 b = perm[xi + 1] + yi, ba = perm[b] + zi, bb = perm[b + 1] + zi;

			// The eight corners of the cell, x fastest: gradient dot offset
			const u32 h[8]{perm[aa] & 15u, perm[ba] & 15u, perm[ab] & 15u, perm[bb] & 15u, perm[aa + 1] & 15u, perm[ba + 1] & 15u, perm[ab + 1] & 15u, perm[bb + 1] & 15u};
			const __m256 gx = _mm256_setr_ps(gradients[h[0]][0], gradients[h[1]][0], gradients[h[2]][0], gradients[h[3]][0], gradients[h[4]][0], gradients[h[5]][0], gradients[h[6]][0], gradients[h[7]][0]);
			const __m256 gy = _mm256_setr_ps(gradients[h[0]][1], gradients[h[1]][1], gradients[h[2]][1], gradients[h[3]][1], gradients[h[4]][1], gradients[h[5]][1], gradients[h[6]][1], gradients[h[7]][1]);
			const __m256 gz = _mm256_setr_ps(gradients[h[0]][2], gradients[h[1]][2], gradients[h[2]][2], gradients[h[3]][2], gradients[h[4]][2], gradients[h[5]][2], gradients[h[6]][2], gradients[h[7]][2]);
			const __m256 dx = _mm256_sub_ps(_mm256_set1_ps(x), _mm256_setr_ps(0, 1, 0, 1, 0, 1, 0, 1));
			const __m256 dy = _mm256_sub_ps(_mm256_set1_ps(y), _mm256_setr_ps(0, 0, 1, 1, 0, 0, 1, 1));
			const __m256 dz = _mm256_sub_ps(_mm256_set1_ps(z), _mm256_setr_ps(0, 0, 0, 0, 1, 1, 1, 1));

			alignas(32) float d[8];
			_mm256_store_ps(d, _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(gx, dx), _mm256_mul_ps(gy, dy)), _mm256_mul_ps(gz, dz)));

			const float u = fade(x), v = fade(y), w = fade(z);
			result[i] = lerp(w, lerp(v, lerp(u, d[0], d[1]), lerp(u, d[2], d[3])), lerp(v, lerp(u, d[4], d[5]), lerp(u, d[6], d[7])));
		}

		for (u32 i = 0; i < 4; i++) spu->gpr[3]._f[3 - i] = result[i];

		spu->pc = word(*spu, 0) & 0x3fffc;
		kernel_check::after(spu, ls, 0x4980, check);
		return 1;
	}

	// 0x5cb0..0x5de8: output format 0 of the vertex packer (function 0x5b38): the xyz of r60 positions (vec4 at the
	// second word of r5) as 12 bytes each at the first word of r5. The SPU loop leaves 16 bytes of leftovers after
	// them, outside of what is sent; zeros here.
	inline u32 pack_positions(spu_thread* spu, u8* ls)
	{
		const u32 count = word(*spu, 60);
		const u32 destination = word(*spu, 5) & 0x3ffff;
		const u32 source = spu->gpr[5]._u32[2] & 0x3fff0;

		if (!active() || !count || count > 0x2000 || destination + count * 12 + 16 > SPU_LS_SIZE || source + count * 16 > SPU_LS_SIZE) return 0;
		if (destination < source + count * 16 && source < destination + count * 12 + 16) return 0;

		const u8* src = ls + source;
		u8* dst = ls + destination;

		for (u32 i = 0; i < count; i++, src += 16, dst += 12)
		{
			std::memcpy(dst, src, 12);
		}

		std::memset(dst, 0, 16);
		return 1;
	}

	// The vertex packer's larger formats. The job context (r12) lists the vertex attribute ids at +352 and the address of
	// each attribute's vec4 stream as words from r39.
	inline const u8* attribute_stream(const spu_thread& spu, const u8* ls, u8 attribute, u32 count)
	{
		const u8* ids = ls + (word(spu, 12) & 0x3fff0) + 352;

		for (u32 i = 0; i < 16; i++)
		{
			if (ids[i] == attribute)
			{
				be_t<u32> address;
				std::memcpy(&address, ls + ((word(spu, 39) + i * 4) & 0x3fffc), 4);
				const u32 a = address & 0x3fff0;
				return a + count * 16 <= SPU_LS_SIZE ? ls + a : nullptr;
			}
		}

		return nullptr;
	}

	// A register's four floats as x, y, z, w
	inline __m128 lanes(const spu_thread& spu, u32 reg)
	{
		return _mm_castsi128_ps(_mm_shuffle_epi32(_mm_load_si128(reinterpret_cast<const __m128i*>(&spu.gpr[reg])), 0x1b));
	}

	// float to int as CFLTS does it for these value ranges: truncated, the largest int for anything above
	inline __m128i to_int(__m128 v)
	{
		return _mm_cvttps_epi32(_mm_min_ps(v, _mm_set1_ps(2147483520.f)));
	}

	struct packer
	{
		__m128 scale16, bias16; // r66, r76: 32767.5 and -0.5
		__m128 scale_bits, bias_bits; // r70, r69: (2^n - 1) / 2^n and -1 / 2^n for n = 11, 11, 10

		explicit packer(const spu_thread& spu)
			: scale16(lanes(spu, 66)), bias16(lanes(spu, 76)), scale_bits(lanes(spu, 70)), bias_bits(lanes(spu, 69))
		{
		}

		// Four signed 16-bit values: floor(v * 32767.5 - 0.5)
		void put16(u8* out, const u8* in) const
		{
			const __m128i i = _mm_srai_epi32(to_int(_mm_mul_ps(_mm_fmadd_ps(load_be(in), scale16, bias16), _mm_set1_ps(65536.f))), 16);
			const __m128i h = _mm_packs_epi32(i, i);
			_mm_storel_epi64(reinterpret_cast<__m128i*>(out), _mm_shuffle_epi8(h, _mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14)));
		}

		// xyz in 32 bits: x and y in 11 bits (low and middle), z in the top 10
		void put_11_11_10(u8* out, const u8* in) const
		{
			alignas(16) s32 i[4];
			_mm_store_si128(reinterpret_cast<__m128i*>(i), to_int(_mm_mul_ps(_mm_fmadd_ps(load_be(in), scale_bits, bias_bits), _mm_set1_ps(2147483648.f))));
			const be_t<u32> packed = (static_cast<u32>(i[2] >> 22) & 0x3ff) << 22 | (static_cast<u32>(i[1] >> 21) & 0x7ff) << 11 | (static_cast<u32>(i[0] >> 21) & 0x7ff);
			std::memcpy(out, &packed, 4);
		}

		// xyz as half floats the way the SPU code makes them: mantissa truncated, too small gives 0 without the sign,
		// too large the largest half
		static void put_halves(u8* out, const u8* in)
		{
			for (u32 c = 0; c < 3; c++)
			{
				be_t<u32> bits;
				std::memcpy(&bits, in + c * 4, 4);
				const u32 u = bits;
				const s32 e = static_cast<s32>((u >> 23) & 255) - 112;
				const u32 h = e < 0 ? 0 : ((u >> 16) & 0x8000) | (e >= 31 ? 0x7bff : (static_cast<u32>(e) << 10) | ((u >> 13) & 0x3ff));
				out[c * 2] = static_cast<u8>(h >> 8);
				out[c * 2 + 1] = static_cast<u8>(h);
			}
		}
	};

	// Formats 7 (0x5ea8..0x5de8, 36 bytes a vertex) and 4 (0x66f0..0x5de8, 54 bytes): position (attribute 1) as three
	// floats, attribute 3 (normal) as four 16-bit values, attribute 4 (tangent) in 11:11:10 bits, attribute 0x21
	// (format 7) or 0x1d (format 4) as three floats; format 4 then has attributes 0x1e, 0x1f and 0x20 as three half
	// floats each. r60 vertex count, first word of r5 the destination.
	template <bool Wide>
	inline u32 pack_vertices(spu_thread* spu, u8* ls)
	{
		constexpr u32 size = Wide ? 54 : 36;
		const u32 count = word(*spu, 60);
		const u32 destination = word(*spu, 5) & 0x3ffff;

		if (!active() || !count || count > 0x2000 || destination + count * size > SPU_LS_SIZE) return 0;

		const u8* position = attribute_stream(*spu, ls, 1, count);
		const u8* normal = attribute_stream(*spu, ls, 3, count);
		const u8* tangent = attribute_stream(*spu, ls, 4, count);
		const u8* second = attribute_stream(*spu, ls, Wide ? 0x1d : 0x21, count);
		const u8* extra[3]{};

		if (!position || !normal || !tangent || !second) return 0;

		if (Wide)
		{
			for (u32 i = 0; i < 3; i++)
			{
				if (!(extra[i] = attribute_stream(*spu, ls, static_cast<u8>(0x1e + i), count))) return 0;
			}
		}

		// The output must not lie over any input
		const u8* out_begin = ls + destination;
		const u8* out_end = out_begin + count * size;

		for (const u8* in : {position, normal, tangent, second, extra[0], extra[1], extra[2]})
		{
			if (in && in < out_end && out_begin < in + count * 16) return 0;
		}

		const packer pack(*spu);
		u8* out = ls + destination;

		for (u32 v = 0; v < count; v++, out += size)
		{
			alignas(16) u8 record[64];
			std::memcpy(record, position + v * 16, 12);
			pack.put16(record + 12, normal + v * 16);
			pack.put_11_11_10(record + 20, tangent + v * 16);
			std::memcpy(record + 24, second + v * 16, 12);

			if (Wide)
			{
				for (u32 i = 0; i < 3; i++) packer::put_halves(record + 36 + i * 6, extra[i] + v * 16);
			}

			std::memcpy(out, record, size);
		}

		return 1;
	}

	// 0x8fd8, the whole function: per-vertex light from a range of point lights. r3 destination (vec4 per vertex), r4
	// positions, r5 vertex count (done in eights), r6 light table: its first two words are the offsets of the light
	// positions and colours (vec4 arrays), words 4 and 5 add up to the first light, word 6 is the number of lights.
	// A vertex gets the sum of colour * (colour.w * f * f), f = smoothstep(1 - min(distance * position.w, 1)).
	inline u32 point_lights(spu_thread* spu, u8* ls)
	{
		const s32 vertices = static_cast<s32>(word(*spu, 5));
		const u32 count = (static_cast<u32>(vertices) + 7) & ~7u;
		const u32 destination = word(*spu, 3) & 0x3fff0, source = word(*spu, 4) & 0x3fff0, table = word(*spu, 6) & 0x3fff0;

		if (!active() || vertices > 0x4000 || table + 32 > SPU_LS_SIZE) return 0;

		be_t<u32> header[8];
		std::memcpy(header, ls + table, 32);
		const u32 first = header[4] + header[5], lights = header[6];
		const u32 positions = (table + header[0] + first * 16) & 0x3fff0, colours = (table + header[1] + first * 16) & 0x3fff0;

		if (lights > 64 || positions + lights * 16 > SPU_LS_SIZE || colours + lights * 16 > SPU_LS_SIZE) return 0;

		if (vertices > 0)
		{
			if (destination + count * 16 > SPU_LS_SIZE || source + count * 16 > SPU_LS_SIZE) return 0;
			if (destination != source && destination < source + count * 16 && source < destination + count * 16) return 0;

			__m128 at[64], colour[64], reach[64], strength[64];

			for (u32 i = 0; i < lights; i++)
			{
				at[i] = load_be(ls + positions + i * 16);
				colour[i] = load_be(ls + colours + i * 16);
				reach[i] = _mm_shuffle_ps(at[i], at[i], _MM_SHUFFLE(3, 3, 3, 3));
				strength[i] = _mm_shuffle_ps(colour[i], colour[i], _MM_SHUFFLE(3, 3, 3, 3));
			}

			const __m128 one = _mm_set1_ps(1.f);

			for (u32 v = 0; v < count; v++)
			{
				const __m128 p = load_be(ls + source + v * 16);
				__m128 sum = _mm_setzero_ps();

				for (u32 i = 0; i < lights; i++)
				{
					const __m128 d = _mm_sub_ps(p, at[i]);
					const __m128 distance = _mm_sqrt_ss(_mm_dp_ps(d, d, 0x71));
					const __m128 s = _mm_sub_ss(one, _mm_min_ss(_mm_mul_ss(distance, reach[i]), one));
					const __m128 f = _mm_mul_ss(_mm_mul_ss(s, s), _mm_fnmadd_ss(_mm_set_ss(2.f), s, _mm_set_ss(3.f)));
					const __m128 k = _mm_mul_ss(strength[i], _mm_mul_ss(f, f));
					sum = _mm_fmadd_ps(colour[i], _mm_shuffle_ps(k, k, 0), sum);
				}

				store_be(ls + destination + v * 16, sum);
			}
		}

		spu->pc = word(*spu, 0) & 0x3fffc;
		return 1;
	}

	// Job b245f318 (bounding spheres of the scene's objects; the main thread waits for it), function 0x59a0:
	// r3 groups, r4 one sphere out per group, r5 a byte per group: its number of spheres, r6 room for a group's moved
	// spheres, r7 the spheres of all groups in a row (16 bytes: centre, bone index byte, exponent byte, two index bits
	// and 14 mantissa bits of the radius), r8 bone matrices (64 bytes). Every sphere is moved by its bone and its
	// radius scaled by the longest of the matrix's three axes; a group's sphere sits in the middle of its spheres' bounds
	// and reaches the farthest of them. Square roots are exact here (the SPU code refines an estimate once).
	inline u32 group_spheres(spu_thread* spu, u8* ls)
	{
		const u32 groups = word(*spu, 3);
		const u32 out = word(*spu, 4) & 0x3ffff;
		const u32 counts = word(*spu, 5) & 0x3ffff;
		const u32 moved = word(*spu, 6) & 0x3ffff;
		const u32 first = word(*spu, 7) & 0x3ffff;
		const u32 matrices = word(*spu, 8) & 0x3ffff;

		if (!active_spheres() || !groups || groups > 0x1000 || ((out | moved | first | matrices) & 15)) return 0;
		if (counts + groups > SPU_LS_SIZE || out + groups * 16 > SPU_LS_SIZE || moved + 255 * 16 > SPU_LS_SIZE) return 0;

		// Nothing may be written before every input is known to be in range
		u32 total = 0;
		for (u32 g = 0; g < groups; g++) total += ls[counts + g];
		if (!ls[counts + groups - 1] || first + total * 16 > SPU_LS_SIZE) return 0;

		for (u32 i = 0; i < total; i++)
		{
			const u8* item = ls + first + i * 16;
			if (matrices + (item[12] | (item[14] >> 6) << 8) * 64 + 64 > SPU_LS_SIZE) return 0;
		}

		const s32 check = kernel_check::before(spu, ls, 0x59a0);
		const u8* item = ls + first;
		__m128 spheres[255];
		float far = 0.f;

		for (u32 g = 0; g < groups; g++)
		{
			const u32 n = ls[counts + g];
			__m128 low = _mm_set1_ps(3.40282347e+38f), high = _mm_set1_ps(-3.40282347e+38f);

			for (u32 i = 0; i < n; i++, item += 16)
			{
				const u8* m = ls + matrices + (item[12] | (item[14] >> 6) << 8) * 64;
				const __m128 r0 = load_be(m), r1 = load_be(m + 16), r2 = load_be(m + 32), r3 = load_be(m + 48);
				const __m128 p = load_be(item);

				__m128 c = _mm_mul_ps(r0, _mm_shuffle_ps(p, p, _MM_SHUFFLE(0, 0, 0, 0)));
				c = _mm_fmadd_ps(r1, _mm_shuffle_ps(p, p, _MM_SHUFFLE(1, 1, 1, 1)), c);
				c = _mm_fmadd_ps(r2, _mm_shuffle_ps(p, p, _MM_SHUFFLE(2, 2, 2, 2)), c);
				c = _mm_add_ps(c, r3);

				const __m128 axis = _mm_max_ss(_mm_max_ss(_mm_dp_ps(r0, r0, 0x71), _mm_dp_ps(r1, r1, 0x71)), _mm_dp_ps(r2, r2, 0x71));
				const __m128 radius = _mm_castsi128_ps(_mm_cvtsi32_si128(item[13] << 23 | ((item[14] << 8 | item[15]) & 0x3fff) << 9));
				const __m128 s = _mm_insert_ps(c, _mm_mul_ss(_mm_sqrt_ss(axis), radius), 0x30);

				spheres[i] = s;
				store_be(ls + moved + i * 16, s);
				low = _mm_min_ps(low, s);
				high = _mm_max_ps(high, s);
			}

			const __m128 centre = _mm_mul_ps(_mm_add_ps(low, high), _mm_set1_ps(0.5f));
			far = 0.f;

			for (u32 i = 0; i < n; i++)
			{
				const __m128 d = _mm_sub_ps(spheres[i], centre);
				far = std::max(far, _mm_cvtss_f32(_mm_sqrt_ss(_mm_dp_ps(d, d, 0x71))) + _mm_cvtss_f32(_mm_shuffle_ps(spheres[i], spheres[i], _MM_SHUFFLE(3, 3, 3, 3))));
			}

			store_be(ls + out + g * 16, _mm_insert_ps(centre, _mm_set_ss(far), 0x30));
		}

		spu->gpr[3] = v128::from32p(std::bit_cast<u32>(far));
		spu->pc = word(*spu, 0) & 0x3fffc;
		kernel_check::after(spu, ls, 0x59a0, check);
		return 1;
	}

	// The same job, function 0x4ef0: r3 and r4 the numbers of spheres in two lists (16 bits each), r5 and r7 the lists
	// (centre and radius), r6 a bit for every pair, row by row, the lowest bit of a byte first: set where the two
	// spheres overlap. The comparison is done in the SPU code's operations and order, so the bits are the same.
	inline u32 sphere_pairs(spu_thread* spu, u8* ls)
	{
		const u32 na = word(*spu, 3) & 0xffff;
		const u32 nb = word(*spu, 4) & 0xffff;
		const u32 a = word(*spu, 5) & 0x3ffff;
		const u32 bits = word(*spu, 6) & 0x3ffff;
		const u32 b = word(*spu, 7) & 0x3ffff;
		const u32 bytes = (na * nb + 7) >> 3;

		constexpr u32 max_b = 1024;

		if (!active_spheres() || nb > max_b || na > 0x1000 || ((a | b) & 15)) return 0;
		if (a + na * 16 > SPU_LS_SIZE || b + nb * 16 > SPU_LS_SIZE || bits + bytes + 1 > SPU_LS_SIZE) return 0;
		if ((bits < a + na * 16 && a < bits + bytes) || (bits < b + nb * 16 && b < bits + bytes)) return 0;

		const s32 check = kernel_check::before(spu, ls, 0x4ef0);
		u8* out = ls + bits;
		std::memset(out, 0, bytes);

		if (na && nb)
		{
			// The second list as one array per component, padded to whole groups of eight
			alignas(32) float bx[max_b + 8], by[max_b + 8], bz[max_b + 8], bw[max_b + 8];

			for (u32 j = 0; j < nb; j++)
			{
				alignas(16) float v[4];
				_mm_store_ps(v, load_be(ls + b + j * 16));
				bx[j] = v[0], by[j] = v[1], bz[j] = v[2], bw[j] = v[3];
			}

			for (u32 j = nb; j < ((nb + 7) & ~7u); j++) bx[j] = by[j] = bz[j] = bw[j] = 0.f;

			u32 bit = 0;

			for (u32 i = 0; i < na; i++)
			{
				alignas(16) float v[4];
				_mm_store_ps(v, load_be(ls + a + i * 16));
				const __m256 ax = _mm256_set1_ps(v[0]), ay = _mm256_set1_ps(v[1]), az = _mm256_set1_ps(v[2]), aw = _mm256_set1_ps(v[3]);

				for (u32 j = 0; j < nb; j += 8)
				{
					const __m256 dx = _mm256_sub_ps(ax, _mm256_load_ps(bx + j)), dy = _mm256_sub_ps(ay, _mm256_load_ps(by + j)), dz = _mm256_sub_ps(az, _mm256_load_ps(bz + j));
					const __m256 distance = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy)), _mm256_mul_ps(dz, dz));
					const __m256 reach = _mm256_add_ps(_mm256_load_ps(bw + j), aw);
					const u32 count = std::min<u32>(8, nb - j);
					const u32 hit = (_mm256_movemask_ps(_mm256_cmp_ps(_mm256_mul_ps(reach, reach), distance, _CMP_GT_OQ)) & ((1u << count) - 1)) << (bit & 7);

					out[bit >> 3] |= static_cast<u8>(hit);
					if (hit >> 8) out[(bit >> 3) + 1] |= static_cast<u8>(hit >> 8);
					bit += count;
				}
			}
		}

		spu->gpr[3]._u32[3] = na * nb;
		spu->pc = word(*spu, 0) & 0x3fffc;
		kernel_check::after(spu, ls, 0x4ef0, check);
		return 1;
	}

	struct kernel
	{
		u32 entry;    // first instruction of the region
		u32 exit;     // where the SPU code continues; 0 = the region is a whole function: the kernel sets the pc to the return address
		u32 words[4]; // the instructions at the entry, to recognise the program
		u32 (*run)(spu_thread*, u8*);
		const char* name;
	};

	inline constexpr kernel kernels[]
	{
		{0x10d74, 0x10ff8, {0x0f60df9e, 0x040004b2, 0x340029ba, 0x04002ca5}, &skin_positions_weighted, "spu_geometry_skin_positions_weighted"},
		{0x111bc, 0x1160c, {0x12008a20, 0x7c00a102, 0x3400df26, 0x04002528}, &skin_streams, "spu_geometry_skin_streams"},
		{0x11740, 0, {0x04000226, 0x3400c31a, 0x1c01c19b, 0x34004319}, &transform_by_matrix, "spu_geometry_transform_by_matrix"},
		{0x0c168, 0x0c520, {0x4c0029a7, 0x200076a7, 0x0400292a, 0x338ca1b7}, &binormals, "spu_geometry_binormals"},
		{0x04980, 0, {0x0f58419f, 0x3fe001c8, 0x0f584215, 0x24ffc0fe}, &noise4, "spu_noise4"},
		{0x05cb0, 0x05de8, {0x40800011, 0x34058625, 0x408000a9, 0x120017ac}, &pack_positions, "spu_geometry_pack_positions"},
		{0x05ea8, 0x05de8, {0x40800010, 0x34058603, 0x40800205, 0x339849f5}, &pack_vertices<false>, "spu_geometry_pack_vertices_36"},
		{0x066f0, 0x05de8, {0x4080001f, 0x34058636, 0x4080026f, 0x33973ec1}, &pack_vertices<true>, "spu_geometry_pack_vertices_54"},
		{0x08fd8, 0, {0x0400020b, 0x34004308, 0x0400018a, 0x34000309}, &point_lights, "spu_geometry_point_lights"},
		{0x0dd50, 0, {0x1c01c242, 0x3388679f, 0x34000198, 0x14fe2117}, &normalize, "spu_geometry_normalize"},
		{0x059a0, 0, {0x040001ac, 0x3fe00226, 0x04000297, 0x3fe0032b}, &group_spheres, "spu_native_group_spheres"},
		{0x04ef0, 0, {0x04000188, 0x12000792, 0x7980c203, 0x3fe0021a}, &sphere_pairs, "spu_native_sphere_pairs"},
	};

	// Diagnostic for writing a kernel (RPCS3_SPU_REGION_CAPTURE=<directory>,<pc hex>,<first instruction hex>[,<every>]):
	// registers and local store at up to 200 arrivals at that instruction (every n-th), as region-NNNN.bin
	// ("SPUR", pc, 128 registers, local store). The SPU code runs on as usual.
	struct region_capture
	{
		std::string dir;
		u32 pc = 0, word = 0, every = 1;

		static const region_capture& get()
		{
			static const region_capture instance = []
			{
				region_capture r;
				if (const char* v = std::getenv("RPCS3_SPU_REGION_CAPTURE"))
				{
					const std::string text = v;
					const auto a = text.find(','), b = text.find(',', a + 1), c = text.find(',', b + 1);
					if (a != std::string::npos && b != std::string::npos)
					{
						r.dir = text.substr(0, a);
						r.pc = static_cast<u32>(std::strtoul(text.c_str() + a + 1, nullptr, 16));
						r.word = static_cast<u32>(std::strtoul(text.c_str() + b + 1, nullptr, 16));
						if (c != std::string::npos) r.every = std::max<u32>(1, std::atoi(text.c_str() + c + 1));
					}
				}
				return r;
			}();
			return instance;
		}

		static u32 run(spu_thread* spu, u8* ls)
		{
			static std::atomic<u32> s_seen{0}, s_written{0};
			const region_capture& r = get();
			if (s_seen++ % r.every || s_written >= 200) return 0;
			const u32 n = s_written++;
			if (n >= 200) return 0;
			if (FILE* f = std::fopen(fmt::format("%s/region-%04u.bin", r.dir, n).c_str(), "wb"))
			{
				std::fwrite("SPUR", 4, 1, f);
				std::fwrite(&r.pc, 4, 1, f);
				std::fwrite(spu->gpr.data(), 16, 128, f);
				std::fwrite(ls, 1, SPU_LS_SIZE, f);
				std::fclose(f);
			}
			return 0;
		}
	};

	inline const kernel capture_kernel{0, 0, {}, &region_capture::run, "spu_region_capture"};

	// The kernel that replaces a whole function at this address, if there is one (its instructions are for the caller to compare)
	inline const kernel* find_function(u32 pc)
	{
		for (const kernel& k : kernels)
		{
			if (k.entry == pc && !k.exit) return &k;
		}

		return nullptr;
	}

	// The kernel whose region starts with these instructions at this address, if any
	inline const kernel* find(u32 pc, std::span<const u32> words_from_pc)
	{
		if (const auto& capture = region_capture::get(); capture.pc && capture.pc == pc && !words_from_pc.empty() && words_from_pc[0] == capture.word) [[unlikely]]
		{
			return &capture_kernel;
		}

		if (!enabled() || words_from_pc.size() < 4)
		{
			return nullptr;
		}

		for (const kernel& k : kernels)
		{
			if (k.entry == pc && words_from_pc[0] == k.words[0] && words_from_pc[1] == k.words[1] && words_from_pc[2] == k.words[2] && words_from_pc[3] == k.words[3])
			{
				return &k;
			}
		}

		return nullptr;
	}
}

#if defined(__clang__)
#pragma clang attribute pop
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif
