#pragma once

#include "util/types.hpp"
#include "Utilities/geometry.h"

namespace vk
{
	class command_buffer;
	class image;
	struct image_view;

	// GPU replacement for the deferred lighting SPU job of inFamous 2 (BCES01143). RPCS3_NATIVE_LIGHTING is a bit mask:
	//  1 = the game samples the GPU result instead of the textures the SPU job writes
	//  4 = the SPU job skips its pixel work
	//  8 = its writes to the two images are dropped as well
	// 16 = with 4, 8 and RPCS3_NATIVE_SSAO bit 4: neither job loads the G-buffer copies, so they are not read back
	namespace native_lighting
	{
		u32 mode();

		// The game has blitted a G-buffer image (1280x720) to main memory for the job
		void on_gbuffer(vk::command_buffer& cmd, vk::image* src, const areai& area, u32 address);

		// The view to sample instead of the game's texture at this address, or null
		vk::image_view* substitute(vk::command_buffer& cmd, vk::image_view* original, u32 address);

		bool is_gbuffer(u32 address);

		// True while both SPU jobs leave the G-buffer copies alone: reading them back ahead of a fault is wasted
		bool readback_unneeded(u32 address);
		void on_readback(u32 address);

		void destroy();
	}
}
