#include "stdafx.h"
#include "VKNativeAA.h"
#include "VKOverlays.h"
#include "VKRenderPass.h"
#include "VKHelpers.h"
#include "VKResourceManager.h"
#include "vkutils/image.h"
#include "vkutils/device.h"
#include "Emu/RSX/RSXThread.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

LOG_CHANNEL(aa_log, "NativeAA");

namespace vk::native_aa
{
	std::atomic<bool> g_job_skipped{false};
	std::atomic<u32> g_frame{0};

	namespace
	{
		constexpr u32 guest_w = 1280, guest_h = 720, piece_w = 1024;

		// Edge-directed smoothing in one pass. For a pixel on a luminance edge the edge is followed along its
		// direction, one pixel at a time, to both of its ends; the distance to the nearer end gives how far the pixel
		// is shifted towards its neighbour across the edge, which is the coverage a straight line through the
		// step would give it. Pixels away from edges are copied. fs0: the picture, filtered.
		const char* fs_smooth = R"(
#version 440
layout(set=0, binding=0) uniform sampler2D fs0;
layout(location=0) out vec4 ocol;

float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

void main()
{
	vec2 size = vec2(textureSize(fs0, 0));
	vec2 px = 1.0 / size;
	vec2 uv = gl_FragCoord.xy * px;
	// The search runs over the game's pixels: with a resolution scale a step is still short enough to stay on the edge
	const int STEPS = 32;

	vec4 cM = textureLod(fs0, uv, 0.0);
	float lM = luma(cM.rgb);
	float lN = luma(textureLodOffset(fs0, uv, 0.0, ivec2(0, -1)).rgb);
	float lS = luma(textureLodOffset(fs0, uv, 0.0, ivec2(0, 1)).rgb);
	float lW = luma(textureLodOffset(fs0, uv, 0.0, ivec2(-1, 0)).rgb);
	float lE = luma(textureLodOffset(fs0, uv, 0.0, ivec2(1, 0)).rgb);
	float lmax = max(lM, max(max(lN, lS), max(lW, lE)));
	float lmin = min(lM, min(min(lN, lS), min(lW, lE)));
	float range = lmax - lmin;

	if (range < max(0.04, lmax * 0.1))
	{
		ocol = cM;
		return;
	}

	float lNW = luma(textureLodOffset(fs0, uv, 0.0, ivec2(-1, -1)).rgb);
	float lNE = luma(textureLodOffset(fs0, uv, 0.0, ivec2(1, -1)).rgb);
	float lSW = luma(textureLodOffset(fs0, uv, 0.0, ivec2(-1, 1)).rgb);
	float lSE = luma(textureLodOffset(fs0, uv, 0.0, ivec2(1, 1)).rgb);

	float edge_h = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
	float edge_v = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
	bool horizontal = edge_h >= edge_v;

	// The two neighbours across the edge, and the side with the larger step
	float l1 = horizontal ? lN : lW;
	float l2 = horizontal ? lS : lE;
	float g1 = abs(l1 - lM), g2 = abs(l2 - lM);
	bool first = g1 >= g2;
	float across = (horizontal ? px.y : px.x) * (first ? -1.0 : 1.0);
	float gradient = max(g1, g2) * 0.25;
	float pair = 0.5 * ((first ? l1 : l2) + lM);

	// Halfway between the two rows (or columns) the filtered picture gives their mean
	vec2 start = uv + (horizontal ? vec2(0.0, across * 0.5) : vec2(across * 0.5, 0.0));
	vec2 along = horizontal ? vec2(px.x, 0.0) : vec2(0.0, px.y);

	float end_n = 0.0, end_p = 0.0;
	int dist_n = STEPS, dist_p = STEPS;

	for (int i = 1; i <= STEPS; i++)
	{
		end_n = luma(textureLod(fs0, start - along * float(i), 0.0).rgb) - pair;
		if (abs(end_n) >= gradient) { dist_n = i; break; }
	}

	for (int i = 1; i <= STEPS; i++)
	{
		end_p = luma(textureLod(fs0, start + along * float(i), 0.0).rgb) - pair;
		if (abs(end_p) >= gradient) { dist_p = i; break; }
	}

	// Only the end where the edge turns away from this pixel's side smooths it
	bool nearer_n = dist_n < dist_p;
	float end = nearer_n ? end_n : end_p;
	bool turns = (end < 0.0) != (lM - pair < 0.0);
	float shift = turns ? 0.5 - float(min(dist_n, dist_p)) / float(dist_n + dist_p) : 0.0;

	vec2 at = uv + (horizontal ? vec2(0.0, across * shift) : vec2(across * shift, 0.0));
	ocol = vec4(textureLod(fs0, at, 0.0).rgb, cM.a);
}
)";

		struct pass : public vk::overlay_pass
		{
			pass()
			{
				vs_src =
				#include "../Program/GLSLSnippets/GenericVSPassthrough.glsl"
				;
				fs_src = fs_smooth;

				renderpass_config.set_depth_mask(false);
				renderpass_config.set_color_mask(0, true, true, true, true);
				renderpass_config.set_attachment_count(1);

				m_num_usable_samplers = 1;
				m_num_uniform_buffers = 0;
				m_sampler_filter = VK_FILTER_LINEAR;
			}
		};

		struct state_t
		{
			std::unique_ptr<pass> smooth;
			std::unique_ptr<vk::viewable_image> output;
			std::unordered_map<u32, std::unique_ptr<vk::image_view>> output_views;
			u32 frame = 0; // Guest address of the picture's copy
			bool valid = false;
		};

		std::unique_ptr<state_t> g_state;
	}

	u32 mode()
	{
		static const u32 value = []() -> u32
		{
			const char* flag = std::getenv("RPCS3_NATIVE_AA");
			return flag ? static_cast<u32>(std::strtoul(flag, nullptr, 0)) : 0;
		}();
		return value;
	}

	bool skip_task_work(u8* ls, u32 lsa, const u8* image, u32 size)
	{
		// First words of the program (at 0x3000), and the first instruction of each of the two functions the task's
		// main loop calls for a frame: 0x65d8 (the picture's edges) and 0x8168 (the blend)
		static constexpr u32 magic[4]{0x431e0a02, 0x43c32882, 0x43a4ab02, 0x42bbc982};
		static constexpr std::pair<u32, u32> work[2]{{0x65d8, 0x4020007f}, {0x8168, 0x0400030f}};
		constexpr u32 return_to_caller = 0x35000000; // BI $lr

		const auto word = [](const u8* p) -> u32 { be_t<u32> v; std::memcpy(&v, p, 4); return v; };
		const auto put = [](u8* p, u32 value) { const be_t<u32> v = value; std::memcpy(p, &v, 4); };

		// The first piece carries the program's first words; the second is recognised by the first being there
		const u8* head = image && lsa == 0x3000 && size >= 16 ? image : ls + 0x3000;

		for (u32 i = 0; i < 4; i++)
		{
			if (word(head + i * 4) != magic[i]) return false;
		}

		bool found = false;

		for (const auto& [address, original] : work)
		{
			if (image)
			{
				found |= address >= lsa && address + 4 <= lsa + size && word(image + (address - lsa)) == original;
			}
			else if (word(ls + address) == original)
			{
				put(ls + address, return_to_caller);
				found = true;
			}
		}

		if (!found) return false;

		if (image)
		{
			std::memcpy(ls + lsa, image, size);

			for (const auto& [address, original] : work)
			{
				if (address >= lsa && address + 4 <= lsa + size && word(image + (address - lsa)) == original)
				{
					put(ls + address, return_to_caller);
				}
			}
		}

		g_job_skipped = true;
		return true;
	}

	bool blits_unneeded()
	{
		return (mode() & 7) == 7 && g_job_skipped.load(std::memory_order_relaxed);
	}

	u32 frame_address()
	{
		return g_state ? g_state->frame : 0;
	}

	bool is_piece(u32 dst_address, u32 dst_pitch, u32 clip_width, u32 clip_height, bool first)
	{
		if (!mode() || dst_pitch != guest_w * 4 || clip_height != guest_h) return false;

		if (first)
		{
			if (clip_width != piece_w) return false;
			if (!g_state) g_state = std::make_unique<state_t>();
			g_state->frame = dst_address;
			g_frame = dst_address;
			return true;
		}

		return g_state && g_state->frame && clip_width == guest_w - piece_w && dst_address == g_state->frame + piece_w * 4;
	}

	void on_frame(vk::command_buffer& cmd, vk::image* src)
	{
		if (!mode() || !g_state || !src || src->samples() != 1 || src->format() != VK_FORMAT_B8G8R8A8_UNORM ||
			src->width() < guest_w / 4 || src->height() < guest_h / 4 || !rsx::get_current_renderer()->is_current_thread())
		{
			return;
		}

		auto& s = *g_state;
		auto& dev = cmd.get_command_pool().get_owner();

		if (!s.smooth)
		{
			s.smooth = std::make_unique<pass>();
			s.smooth->create(dev);
			aa_log.success("GPU anti-aliasing active (mode %u)", mode());
		}

		// First frame, or the resolution scale was changed
		if (!s.output || s.output->width() != src->width() || s.output->height() != src->height())
		{
			auto& resources = *vk::get_resource_manager();
			for (auto& view : s.output_views) resources.dispose(view.second);
			s.output_views.clear();
			if (s.output) resources.dispose(s.output);

			s.output = std::make_unique<vk::viewable_image>(dev, dev.get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, src->width(), src->height(), 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
				VMM_ALLOCATION_POOL_UNDEFINED);
			s.valid = false;
			aa_log.notice("GPU anti-aliasing at %ux%u", src->width(), src->height());
		}

		if (vk::is_renderpass_open(cmd)) vk::end_renderpass(cmd);

		src->push_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		const auto src_view = static_cast<vk::viewable_image*>(src)->get_view(rsx::default_remap_vector.with_encoding(VK_REMAP_IDENTITY), VK_IMAGE_ASPECT_COLOR_BIT);

		auto target = s.output.get();
		target->change_layout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		const auto renderpass = vk::get_renderpass(dev, vk::get_renderpass_key(std::vector<vk::image*>{ target }));
		s.smooth->run(cmd, areau{ 0, 0, target->width(), target->height() }, target, { src_view }, renderpass);
		vk::end_renderpass(cmd);
		target->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		src->pop_layout(cmd);
		s.valid = true;
	}

	vk::image_view* substitute(vk::command_buffer& /*cmd*/, vk::image_view* original, u32 texture_address)
	{
		if (!(mode() & 1) || !g_state || !g_state->valid || texture_address != g_state->frame || !original) return nullptr;
		auto& s = *g_state;

		if (original->format() != VK_FORMAT_B8G8R8A8_UNORM || original->image()->width() != guest_w || original->image()->height() != guest_h)
		{
			if (static bool logged = false; !logged)
			{
				logged = true;
				aa_log.error("unexpected picture texture (format %u, %ux%u), keeping the guest image", static_cast<u32>(original->format()), original->image()->width(), original->image()->height());
			}
			return nullptr;
		}

		const auto& map = original->info.components;
		const u32 key = (map.r & 0xff) | ((map.g & 0xff) << 8) | ((map.b & 0xff) << 16) | ((map.a & 0xff) << 24);
		auto& view = s.output_views[key];
		if (!view)
		{
			view = std::make_unique<vk::image_view>(*vk::get_current_renderer(), s.output.get(), VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, map);
			aa_log.success("game now samples the GPU anti-aliased picture (component map %u %u %u %u)", static_cast<u32>(map.r), static_cast<u32>(map.g), static_cast<u32>(map.b), static_cast<u32>(map.a));
		}
		return view.get();
	}

	void destroy()
	{
		g_job_skipped = false;
		g_frame = 0;
		g_state.reset();
	}
}
