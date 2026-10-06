#pragma once

#include "Emu/System.h"

#include <atomic>

namespace rsx
{
	// inFamous (BCES00609), inFamous 2 (BCES01143) and inFamous: Festival of Blood (NPEA00322): the three games share
	// the 512x288 particle target
	inline bool is_infamous_title()
	{
		const std::string& id = Emu.GetTitleID();
		return id == "BCES01143" || id == "BCES00609" || id == "NPEA00322";
	}
}

// Where a game keeps what the GPU occlusion and lighting passes (VKNativeSSAO, VKNativeLighting) and their SPU side
// (SPUThread.cpp) work with. inFamous 2 and Festival of Blood run the same two SPU jobs: the occlusion job and its
// five kernels are the same bytes, the lighting job was compiled again with one comparison turned around. Only the
// addresses differ.
namespace infamous_native
{
	struct addresses
	{
		u32 normals;         // 1280x720 copy of the normals, becomes the diffuse light image
		u32 depth;           // 1280x720 copy of the depth, becomes the specular light image; follows the normals
		u32 half_depth;      // the occlusion job's 640x360 linear depth, first of its scratch buffers
		u32 scratch_end;     // end of those scratch buffers
		u32 occlusion;       // 1280x720 one byte per pixel occlusion image the game samples
		u32 matrix;          // camera matrix in the occlusion job's parameters
		u32 lighting_params; // the lighting job's 256-byte parameter block
		u32 kernels[5];      // the occlusion job's kernels, in the order the job loads them per frame

		// Both images together
		static constexpr u32 gbuffer_bytes = 0x708000;
		static constexpr u32 occlusion_bytes = 0xe1000;

		bool in_gbuffer(u32 address) const { return address - normals < gbuffer_bytes; }
		bool in_occlusion(u32 address) const { return address - occlusion < occlusion_bytes; }
		bool is_kernel(u32 address) const
		{
			for (const u32 kernel : kernels) if (address == kernel) return true;
			return false;
		}
	};

	inline constexpr addresses infamous2{0x37400b80, 0x37784b80, 0x37b08b80, 0x37c22480, 0xcf800000, 0x00a94a20, 0x00a93c80,
		{0x0087d500, 0x0087f880, 0x0087d880, 0x0087e480, 0x0087ec80}};

	// Buffers 0x3cc00 higher, parameters 0x4ee00 higher, kernels (the executable) 0x48000 higher
	inline constexpr addresses festival_of_blood{0x3743d780, 0x377c1780, 0x37b45780, 0x37c5f080, 0xcf800000, 0x00ae3820, 0x00ae2a80,
		{0x008c5500, 0x008c7880, 0x008c5880, 0x008c6480, 0x008c6c80}};

	// Any other title: nothing matches
	inline constexpr addresses none{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
		{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff}};

	inline std::atomic<const addresses*> g_current{&none};

	// The running game's set. Chosen by refresh(), which the Vulkan renderer calls when it starts for a game.
	inline const addresses& current()
	{
		return *g_current.load(std::memory_order_relaxed);
	}

	inline bool active()
	{
		return g_current.load(std::memory_order_relaxed) != &none;
	}

	inline void refresh()
	{
		const std::string& id = Emu.GetTitleID();
		g_current = id == "BCES01143" ? &infamous2 : id == "NPEA00322" ? &festival_of_blood : &none;
	}
}
