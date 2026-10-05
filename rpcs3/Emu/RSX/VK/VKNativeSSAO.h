#pragma once

#include "util/types.hpp"
#include "Utilities/geometry.h"

namespace vk
{
	class command_buffer;
	class image;
	struct image_view;

	// GPU replacement for the ambient occlusion SPU job of inFamous 2 (BCES01143). RPCS3_NATIVE_SSAO is a bit mask:
	// 1 = the game samples the GPU result instead of the texture the SPU job writes
	// 4 = the SPU job skips its work
	// Any non-zero value computes the image.
	namespace native_ssao
	{
		u32 mode();
		// The game has blitted a finished G-buffer image to main memory; src is the render target it came from
		// (see native_lighting::on_gbuffer)
		void on_gbuffer(vk::command_buffer& cmd, vk::image* src, const areai& src_area, u32 dst_address);
		vk::image_view* substitute(vk::command_buffer& cmd, vk::image_view* original, u32 texture_address);
		// The job's half-resolution linear depth buffer (0x37b08b80, 640x360 floats). The game also samples it as a
		// texture (lightning and other particle effects), so substitute() covers it as well.
		bool is_half_depth(u32 texture_address);
		// The view substitute() last gave for that texture, without the guest image having to be looked up
		vk::image_view* half_depth_view();
		void destroy();
	}
}
