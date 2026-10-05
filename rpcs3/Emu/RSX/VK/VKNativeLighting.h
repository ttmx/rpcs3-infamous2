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
	// 32 = with 16: the game's two blits are not done either while that holds; the passes take the render targets
	namespace native_lighting
	{
		u32 mode();

		// The game has blitted a G-buffer image to main memory for the job. src is the render target it came from:
		// 1280x720 times the resolution scale
		void on_gbuffer(vk::command_buffer& cmd, vk::image* src, const areai& area, u32 address);

		// The view to sample instead of the game's texture at this address, or null
		vk::image_view* substitute(vk::command_buffer& cmd, vk::image_view* original, u32 address);

		bool is_gbuffer(u32 address);
		// The address of the G-buffer copy this address is inside of, or 0
		u32 gbuffer_containing(u32 address);

		// True while nothing but the passes would use the two blits (bit 32). It follows the lighting job of the frame
		// before: a frame that turns out to be left to the SPU job after its blits were skipped is lit from old copies.
		bool blits_unneeded();
		// The same as on_gbuffer for a blit that is skipped: src is the render target the game would blit from, colour
		// for the normals and depth-stencil for the depth. Returns the image kept for the passes, as the blit would
		// have written it, or null if the target cannot be used.
		vk::image* on_gbuffer_target(vk::command_buffer& cmd, vk::image* src, u32 address);

		// True while both SPU jobs leave the G-buffer copies alone: reading them back ahead of a fault is wasted
		bool readback_unneeded(u32 address);
		void on_readback(u32 address);

		void destroy();
	}
}
