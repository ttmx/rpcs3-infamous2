#pragma once

#include "util/types.hpp"

#include <atomic>

namespace vk
{
	class command_buffer;
	class image;
	struct image_view;

	// GPU replacement for the anti-aliasing SPU job of God of War III (BCES00510 1.03). The game blits its finished
	// 1280x720 picture to main memory (a 1024 pixel wide piece, then the remaining 256), an SPU job on five threads
	// reads it and writes it back smoothed, and the game samples the result as a texture for the final image.
	// RPCS3_NATIVE_AA is a bit mask:
	// 1 = the game samples the GPU result instead of the texture the SPU job writes
	// 2 = the SPU task's two work functions return at once (it is recognised by the first bytes of its program)
	// 4 = with 1 and 2: the two blits are not done either; the pass takes the render target
	namespace native_aa
	{
		u32 mode();

		// Set by the SPU side when the task's work has been taken out, cleared when the game stops
		extern std::atomic<bool> g_job_skipped;

		// SPU side. With image: a piece of a program is being loaded to the local store at lsa; if it is the task's, it
		// is copied with its two work functions returning at once, and true is returned. Without image: the same
		// for a program that is in the local store already.
		bool skip_task_work(u8* ls, u32 lsa, const u8* image, u32 size);

		// Guest address of the picture's copy, once the game has blitted it (0 before); for the SPU side
		extern std::atomic<u32> g_frame;

		// The blits of the picture can be left out: nothing reads the copy
		bool blits_unneeded();

		// True for the first piece of the picture's blit (remembers where the copy goes) and for its second piece
		bool is_piece(u32 dst_address, u32 dst_pitch, u32 clip_width, u32 clip_height, bool first);
		u32 frame_address();

		// The game has finished the picture; src is the render target, 1280x720 times the resolution scale
		void on_frame(vk::command_buffer& cmd, vk::image* src);
		vk::image_view* substitute(vk::command_buffer& cmd, vk::image_view* original, u32 texture_address);
		void destroy();
	}
}
