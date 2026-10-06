#include "stdafx.h"
#include "VKNativeLighting.h"
#include "VKNativeLightingShaders.hpp"
#include "VKCompute.h"
#include "VKOverlays.h"
#include "VKHelpers.h"
#include "VKRenderPass.h"
#include "VKResourceManager.h"
#include "VKGpuPassProfile.hpp"
#include "vkutils/image.h"
#include "vkutils/sampler.h"
#include "vkutils/buffer_object.h"
#include "Emu/Memory/vm.h"
#include "Emu/System.h"
#include "Emu/infamous_titles.h"
#include "Emu/RSX/RSXThread.h"

#include <bit>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

LOG_CHANNEL(lighting_log, "NativeLighting");

// SPUThread.cpp: both stubbed jobs leave the G-buffer copies alone / bytes of list transfers left out for that reason
extern std::atomic<bool> g_native_gbuffer_unread;
extern std::atomic<u64> g_native_gbuffer_skipped_bytes;

// inFamous 2 (BCES01143, 1.04) computes its deferred lighting in an SPU job. Every frame the game blits the normals
// and the depth image of its G-buffer to main memory; the job reads them, lights the frame in 40x18 tiles and writes
// the diffuse and the specular light over the same memory, which the game then uses as two textures.
//
// Here the same result is computed on the GPU: the two blits are kept as images (latch), the job's parameters and
// light table are taken when the job starts (native_lighting_job_start, called on its SPU thread), and when the game
// binds one of the two textures it gets the image written by five compute passes instead (substitute).
//
// The images have the size of the render targets the game blits from, which is 1280x720 times the resolution scale,
// so the frame is lit at the resolution it is drawn at.
namespace vk::native_lighting
{
	namespace
	{
		// Guest memory of the two images: normals in, diffuse light out / depth in, specular light out
		constexpr u32 guest_width = 1280, guest_height = 720;

		// Shader limits (VKNativeLightingShaders.hpp): MASK_WORDS * 32 lights, 40x18 tiles, five blocks of pixels per tile
		constexpr u32 max_lights = 256, tiles_x = 40, tiles_y = 18, tile_count = tiles_x * tiles_y, blocks_per_tile = 5;
		constexpr u32 block_count = tile_count * blocks_per_tile;

		// Job parameter block (256 bytes, big-endian) and light records (48 bytes each)
		constexpr u32 parameter_bytes = 256, light_bytes = 48;
		constexpr u32 position_matrix_offset = 16, clip_matrix_offset = 80, light_count_offset = 0x90, colour_scale_offset = 0x98;

		// Shader input: 32 header words, 16 words per light, then the view-to-clip matrix (shaders::common)
		constexpr u32 light_words = 16, header_words = 32 + light_words * max_lights, input_words = header_words + 16;

		f32 half_float(u16 v)
		{
			const u32 exp = (v >> 10) & 31, frac = v & 1023;
			if (exp == 31) return std::numeric_limits<f32>::quiet_NaN();
			const f32 value = exp ? std::ldexp(1.f + static_cast<f32>(frac) / 1024.f, static_cast<int>(exp) - 15) : std::ldexp(static_cast<f32>(frac), -24);
			return (v & 0x8000) ? -value : value;
		}

		void warn_unsupported(const std::string& what)
		{
			if (static std::atomic<u32> count{0}; count++ < 8)
			{
				lighting_log.warning("unsupported: %s", what);
			}
		}

		// Shader input from the job's parameter block and light table. False if the passes do not handle this frame
		// (a light kind other than point and spot, or values that are not finite); the SPU job then does its own work.
		bool pack_input(const u8* params, const u8* lights, u32 count, std::vector<u32>& words)
		{
			const auto word = [](const u8* at) { return static_cast<u32>(*reinterpret_cast<const be_t<u32>*>(at)); };
			const auto finite = [](u32 bits) { return std::isfinite(std::bit_cast<f32>(bits)); };

			words.assign(input_words, 0);
			words[16] = word(params + colour_scale_offset);
			words[17] = count;
			words[22] = header_words;

			if (!finite(words[16]))
			{
				return false;
			}

			for (u32 i = 0; i < 16; i++)
			{
				words[i] = word(params + position_matrix_offset + 4 * i);
				words[header_words + i] = word(params + clip_matrix_offset + 4 * i);

				if (!finite(words[i]) || !finite(words[header_words + i]))
				{
					return false;
				}
			}

			for (u32 i = 0; i < count; i++)
			{
				const u8* record = lights + light_bytes * i;
				const auto half = [&](u32 offset) { return half_float(static_cast<u16>(*reinterpret_cast<const be_t<u16>*>(record + offset))); };
				u32* row = words.data() + 32 + light_words * i;

				// Position, kind (1 point, 2 spot), colour, inner and outer radius, cone gain and cosine, near distance, direction
				const u32 kind = word(record + 20);
				for (u32 c = 0; c < 3; c++) row[c] = word(record + 4 * c);
				row[3] = kind;
				for (u32 c = 0; c < 3; c++) row[4 + c] = std::bit_cast<u32>(half(12 + 2 * c));
				for (u32 c = 0; c < 4; c++) row[7 + c] = std::bit_cast<u32>(half(32 + 2 * c));
				row[11] = std::bit_cast<u32>(half(46));
				for (u32 c = 0; c < 3; c++) row[12 + c] = std::bit_cast<u32>(half(40 + 2 * c));

				if (kind != 1 && kind != 2)
				{
					warn_unsupported(fmt::format("light %u of %u has kind %u", i, count, kind));
					return false;
				}

				for (u32 c = 0; c < 15; c++)
				{
					if (c != 3 && !finite(row[c]))
					{
						warn_unsupported(fmt::format("light %u of %u (kind %u): field %u is not finite", i, count, kind, c));
						return false;
					}
				}

				if (half(34) <= half(32))
				{
					warn_unsupported(fmt::format("light %u of %u (kind %u): radii %f %f", i, count, kind, half(32), half(34)));
					return false;
				}
			}

			return true;
		}

		// Parameters of the lighting job that started most recently, taken on its SPU thread from the block it is
		// about to load. The same block read when the result is sampled can already belong to the next frame.
		struct job_t
		{
			std::mutex mutex;
			std::array<u8, parameter_bytes> params{};
			std::vector<u8> lights;
			std::vector<u32> words;
			u64 starts = 0;
			bool seen = false;
			bool supported = false;
		};

		job_t g_job;

		// Set once the game samples the GPU images; the SPU job may skip its pixel work from then on
		std::atomic<bool> g_sampled{false};

		// G-buffer readbacks since start (any thread)
		std::atomic<u64> g_readbacks{0};

		// One compute pass. Bindings as in shaders::common and shaders::io.
		struct pass : vk::compute_task
		{
			std::array<vk::buffer*, 4> buffers{};
			std::array<vk::image_view*, 2> inputs{}, outputs{};
			vk::sampler* sampler = nullptr;

			explicit pass(const char* body)
			{
				m_src = std::string("#version 450\n") + shaders::common + shaders::io + body;
				create();
			}

			std::vector<vk::glsl::program_input> get_inputs() override
			{
				std::vector<vk::glsl::program_input> result;

				for (u32 i = 0; i < 8; i++)
				{
					const auto type = i < 4 ? vk::glsl::input_type_storage_buffer : i < 6 ? vk::glsl::input_type_texture : vk::glsl::input_type_storage_texture;
					result.push_back(vk::glsl::program_input::make(::glsl::glsl_compute_program, "binding" + std::to_string(i), type, 0, i));
				}

				return result;
			}

			void bind_resources(const vk::command_buffer&) override
			{
				for (u32 i = 0; i < 4; i++) m_program->bind_uniform({ *buffers[i], 0, buffers[i]->size() }, 0, i);
				for (u32 i = 0; i < 2; i++) m_program->bind_uniform({ *inputs[i], *sampler }, 0, 4 + i);
				for (u32 i = 0; i < 2; i++) m_program->bind_uniform({ *outputs[i] }, 0, 6 + i);
			}
		};

		// Writes a depth render target as the bytes the game's blit of it gives: 24-bit depth in the A, R and G channels
		struct depth_bytes_pass : vk::overlay_pass
		{
			depth_bytes_pass()
			{
				vs_src =
				#include "../Program/GLSLSnippets/GenericVSPassthrough.glsl"
				;
				fs_src = R"(
#version 440
layout(set=0, binding=0) uniform sampler2D fs0;
layout(location=0) out vec4 ocol;

void main()
{
	uint depth = uint(texelFetch(fs0, ivec2(gl_FragCoord.xy), 0).r * 16777215.0);
	ocol = vec4(float((depth >> 8u) & 255u), float(depth & 255u), 0.0, float(depth >> 16u)) * (1.0 / 255.0);
}
)";
				renderpass_config.set_depth_mask(false);
				renderpass_config.set_color_mask(0, true, true, true, true);
				renderpass_config.set_attachment_count(1);

				m_num_usable_samplers = 1;
				m_num_uniform_buffers = 0;
				m_sampler_filter = VK_FILTER_NEAREST;
			}
		};

		struct state_t
		{
			std::unique_ptr<depth_bytes_pass> depth_bytes;

			// In the order they run, with their work group counts
			static constexpr u32 pass_count = 5;
			std::array<std::unique_ptr<pass>, pass_count> passes;
			std::unique_ptr<vk::sampler> sampler;
			std::unique_ptr<vk::buffer> input_buffer, bounds, tiles, lights;

			// Index 0: normals in, diffuse light out. Index 1: depth in, specular light out.
			std::array<std::unique_ptr<vk::viewable_image>, 2> input, output;
			u32 width = 0, height = 0; // Of those four

			// Views of the output with the component mapping of the game's own texture views
			std::array<std::unordered_map<u32, std::unique_ptr<vk::image_view>>, 2> views;

			u32 latched = 0;        // Bit per input image of the frame being collected
			bool inputs = false;    // Both images of a frame are held
			bool computed = false;  // The passes ran (or were refused) for those images
			bool supported = false; // ... and produced the output

			// Statistics
			u64 frames = 0, job_starts = 0, stale = 0, rejected = 0;
		};

		std::unique_ptr<state_t> g_state;

		std::unique_ptr<vk::viewable_image> make_image(const vk::render_device& dev, VkFormat format, u32 width, u32 height, VkImageUsageFlags usage)
		{
			return std::make_unique<vk::viewable_image>(dev, dev.get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_IMAGE_TYPE_2D, format, width, height, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_TILING_OPTIMAL,
				usage, 0, VMM_ALLOCATION_POOL_UNDEFINED);
		}

		std::unique_ptr<vk::buffer> make_buffer(const vk::render_device& dev, u64 size, VkBufferUsageFlags usage)
		{
			return std::make_unique<vk::buffer>(dev, size, dev.get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				usage, 0, VMM_ALLOCATION_POOL_UNDEFINED);
		}

		vk::image_view* view_of(vk::viewable_image* image)
		{
			return image->get_view(rsx::default_remap_vector.with_encoding(VK_REMAP_IDENTITY), VK_IMAGE_ASPECT_COLOR_BIT);
		}

		void memory_barrier(vk::command_buffer& cmd, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
		{
			VkMemoryBarrier barrier{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
			barrier.srcAccessMask = src;
			barrier.dstAccessMask = dst;
			vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
		}

		void create_resources(const vk::render_device& dev, state_t& s)
		{
			const char* const bodies[state_t::pass_count] = { shaders::lights, shaders::bounds, shaders::cull, shaders::shade, shaders::clear };

			for (u32 i = 0; i < state_t::pass_count; i++)
			{
				s.passes[i] = std::make_unique<pass>(bodies[i]);
			}

			s.sampler = std::make_unique<vk::sampler>(dev, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
				VK_FALSE, 0.f, 1.f, 0.f, 0.f, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);

			// Two vectors per block, 16 words per tile, four vectors per light
			s.input_buffer = make_buffer(dev, input_words * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
			s.bounds = make_buffer(dev, block_count * 32, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			s.tiles = make_buffer(dev, tile_count * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
			s.lights = make_buffer(dev, max_lights * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

			lighting_log.success("GPU lighting active (mode %u)", mode());
		}

		// The G-buffer has another size than the images held (first frame, or the resolution scale was changed)
		void resize(state_t& s, u32 width, u32 height)
		{
			auto& resources = *vk::get_resource_manager();

			for (u32 i = 0; i < 2; i++)
			{
				for (auto& view : s.views[i])
				{
					resources.dispose(view.second);
				}

				s.views[i].clear();
				if (s.input[i]) resources.dispose(s.input[i]);
				if (s.output[i]) resources.dispose(s.output[i]);
			}

			s.width = width;
			s.height = height;
			s.latched = 0;
			s.inputs = s.computed = s.supported = false;
			lighting_log.notice("GPU lighting at %ux%u", width, height);
		}

		// The game may draw over the G-buffer again before it samples the lighting result, so the two images are kept
		// as they were blitted for the job. src is a blitted image, or the render target itself (colour, depth-stencil).
		vk::image* latch(vk::command_buffer& cmd, vk::image* src, u32 address)
		{
			if (!g_state)
			{
				g_state = std::make_unique<state_t>();
			}

			auto& s = *g_state;
			const u32 i = address == infamous_native::current().normals ? 0 : 1;

			if (src->width() != s.width || src->height() != s.height)
			{
				resize(s, src->width(), src->height());
			}

			if (!s.input[i])
			{
				s.input[i] = make_image(cmd.get_command_pool().get_owner(), VK_FORMAT_B8G8R8A8_UNORM, s.width, s.height,
					VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
			}

			if (vk::is_renderpass_open(cmd))
			{
				vk::end_renderpass(cmd);
			}

			vk::gpu_pass_profile::mark(cmd, vk::gpu_pass_profile::label_lighting_latch);

			if (src->aspect() & VK_IMAGE_ASPECT_DEPTH_BIT)
			{
				auto& dev = cmd.get_command_pool().get_owner();

				if (!s.depth_bytes)
				{
					s.depth_bytes = std::make_unique<depth_bytes_pass>();
					s.depth_bytes->create(dev);
				}

				auto target = s.input[i].get();
				const auto depth_view = static_cast<vk::viewable_image*>(src)->get_view(rsx::default_remap_vector.with_encoding(VK_REMAP_IDENTITY), VK_IMAGE_ASPECT_DEPTH_BIT);

				src->push_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				target->change_layout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
				const auto renderpass = vk::get_renderpass(dev, vk::get_renderpass_key(std::vector<vk::image*>{ target }));
				s.depth_bytes->run(cmd, areau{ 0, 0, s.width, s.height }, target, std::vector<vk::image_view*>{ depth_view }, renderpass);
				vk::end_renderpass(cmd);
				target->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				src->pop_layout(cmd);
			}
			else
			{
				VkImageCopy copy{};
				copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				copy.extent = { s.width, s.height, 1 };

				src->push_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				s.input[i]->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				vkCmdCopyImage(cmd, src->value, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.input[i]->value, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
				s.input[i]->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				src->pop_layout(cmd);
			}

			s.latched |= 1u << i;

			if (s.latched != 3)
			{
				return s.input[i].get();
			}

			s.latched = 0;
			s.inputs = true;
			s.computed = false;
			s.frames++;

			if (s.frames % 1800 == 0)
			{
				lighting_log.notice("GPU lighting: %u frames, %u without a new job start, %u left to the SPU, %u G-buffer readbacks, %u MB of SPU transfers skipped",
					s.frames, s.stale, s.rejected, g_readbacks.load(), g_native_gbuffer_skipped_bytes.load() >> 20);
			}

			return s.input[i].get();
		}

		// Light the latched frame with the parameters of the most recent job
		void compute(vk::command_buffer& cmd, state_t& s)
		{
			s.computed = true;
			s.supported = false;

			std::vector<u32> words;
			{
				std::lock_guard lock(g_job.mutex);

				if (!g_job.seen || !g_job.supported)
				{
					s.rejected++;
					return;
				}

				// No job start since the previous frame: the previous parameters are used again
				if (g_job.starts == s.job_starts)
				{
					s.stale++;
				}

				s.job_starts = g_job.starts;
				words = g_job.words;
			}

			if (!s.passes[0])
			{
				create_resources(cmd.get_command_pool().get_owner(), s);
			}

			for (auto& image : s.output)
			{
				if (!image)
				{
					image = make_image(cmd.get_command_pool().get_owner(), VK_FORMAT_R8G8B8A8_UNORM, s.width, s.height,
						VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
				}
			}

			// Image size, its inverse, and the offset that puts the centre of an image pixel where the game's 1280x720
			// grid has it: the job takes pixel x of that grid as x / 1280
			const f32 size[2] = { static_cast<f32>(s.width), static_cast<f32>(s.height) };
			const f32 guest_size[2] = { static_cast<f32>(guest_width), static_cast<f32>(guest_height) };

			for (u32 i = 0; i < 2; i++)
			{
				words[24 + i] = i ? s.height : s.width;
				words[26 + i] = std::bit_cast<u32>(1.f / size[i]);
				words[28 + i] = std::bit_cast<u32>(0.5f - 0.5f * size[i] / guest_size[i]);
			}

			if (vk::is_renderpass_open(cmd))
			{
				vk::end_renderpass(cmd);
			}

			vk::gpu_pass_profile::mark(cmd, vk::gpu_pass_profile::label_lighting_setup);

			// The data is copied into the command buffer, so one buffer serves every frame in flight
			vkCmdUpdateBuffer(cmd, s.input_buffer->value, 0, input_words * 4, words.data());
			memory_barrier(cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

			// Tiles without lights are not written by the passes
			for (auto& image : s.output)
			{
				const VkClearColorValue zero{};
				const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
				image->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				vkCmdClearColorImage(cmd, image->value, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
				image->change_layout(cmd, VK_IMAGE_LAYOUT_GENERAL);
			}

			// Work groups of lights (64 lights), bounds (one block), cull (64 tiles), shade (8x8 pixels), clear (64 pixel rows of a tile)
			const u32 groups[state_t::pass_count][2] =
			{
				{ max_lights / 64, 1 },
				{ tiles_x, tiles_y * blocks_per_tile },
				{ (tile_count + 63) / 64, 1 },
				{ (s.width + 7) / 8, (s.height + 7) / 8 },
				{ (tiles_x * s.height + 63) / 64, 1 },
			};

			for (u32 i = 0; i < state_t::pass_count; i++)
			{
				auto& pass = *s.passes[i];
				pass.buffers = { s.input_buffer.get(), s.bounds.get(), s.tiles.get(), s.lights.get() };
				pass.inputs = { view_of(s.input[0].get()), view_of(s.input[1].get()) };
				pass.outputs = { view_of(s.output[0].get()), view_of(s.output[1].get()) };
				pass.sampler = s.sampler.get();
				vk::gpu_pass_profile::mark(cmd, vk::gpu_pass_profile::label_lighting_pass + i);
				pass.run(cmd, groups[i][0], groups[i][1], 1);

				// Each pass reads what the one before it wrote
				memory_barrier(cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
			}

			for (auto& image : s.output)
			{
				image->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			}

			vk::gpu_pass_profile::mark(cmd, vk::gpu_pass_profile::label_lighting_user);

			s.supported = true;
		}
	}

	u32 mode()
	{
		static const u32 value = []() -> u32
		{
			const char* flag = std::getenv("RPCS3_NATIVE_LIGHTING");
			return flag ? static_cast<u32>(std::strtoul(flag, nullptr, 0)) : 0;
		}();

		return value;
	}

	bool is_gbuffer(u32 address)
	{
		return address == infamous_native::current().normals || address == infamous_native::current().depth;
	}

	u32 gbuffer_containing(u32 address)
	{
		constexpr u32 bytes = guest_width * guest_height * 4;
		return address - infamous_native::current().normals < bytes ? infamous_native::current().normals : address - infamous_native::current().depth < bytes ? infamous_native::current().depth : 0;
	}

	bool blits_unneeded()
	{
		return (mode() & 32) && g_native_gbuffer_unread.load(std::memory_order_relaxed);
	}

	vk::image* on_gbuffer_target(vk::command_buffer& cmd, vk::image* src, u32 address)
	{
		const bool depth = address == infamous_native::current().depth;

		if (!is_gbuffer(address) || src->width() < guest_width / 4 || src->height() < guest_height / 4 || src->samples() != 1 ||
			(depth ? !(src->aspect() & VK_IMAGE_ASPECT_DEPTH_BIT) : src->format() != VK_FORMAT_B8G8R8A8_UNORM) ||
			!rsx::get_current_renderer()->is_current_thread())
		{
			return nullptr;
		}

		return latch(cmd, src, address);
	}

	bool readback_unneeded(u32 address)
	{
		return is_gbuffer(address) && g_native_gbuffer_unread.load(std::memory_order_relaxed);
	}

	void on_readback(u32 address)
	{
		if (is_gbuffer(address))
		{
			g_readbacks++;
		}
	}

	void on_gbuffer(vk::command_buffer& cmd, vk::image* src, const areai& area, u32 address)
	{
		if (!mode() || !is_gbuffer(address))
		{
			return;
		}

		if (area.x1 || area.y1 || area.width() != static_cast<int>(src->width()) || area.height() != static_cast<int>(src->height()) ||
			src->width() < guest_width / 4 || src->height() < guest_height / 4 || src->samples() != 1 ||
			src->format() != VK_FORMAT_B8G8R8A8_UNORM || !rsx::get_current_renderer()->is_current_thread())
		{
			return;
		}

		latch(cmd, src, address);
	}

	vk::image_view* substitute(vk::command_buffer& cmd, vk::image_view* original, u32 address)
	{
		if (!(mode() & 1) || !g_state || !original || !is_gbuffer(address))
		{
			return nullptr;
		}

		auto& s = *g_state;

		if (!s.inputs)
		{
			return nullptr;
		}

		// The size is the game's when the texture comes from guest memory and scaled when it is still the blitted copy
		if (original->format() != VK_FORMAT_B8G8R8A8_UNORM)
		{
			if (static bool logged = false; !logged)
			{
				logged = true;
				lighting_log.error("unexpected lighting texture (format %u, %ux%u), keeping the guest image",
					static_cast<u32>(original->format()), original->image()->width(), original->image()->height());
			}

			return nullptr;
		}

		if (!s.computed)
		{
			compute(cmd, s);
		}

		if (!s.supported)
		{
			return nullptr;
		}

		// The output has the same channels under the same names as the game's texture, so its component mapping applies as it is
		const u32 i = address == infamous_native::current().normals ? 0 : 1;
		const auto& map = original->info.components;
		const u32 key = (map.r & 0xff) | ((map.g & 0xff) << 8) | ((map.b & 0xff) << 16) | ((map.a & 0xff) << 24);
		auto& result = s.views[i][key];

		if (!result)
		{
			result = std::make_unique<vk::image_view>(*vk::get_current_renderer(), s.output[i].get(), VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_VIEW_TYPE_2D, map);
			lighting_log.success("game now samples the GPU lighting image %u (component map %u %u %u %u)", i,
				static_cast<u32>(map.r), static_cast<u32>(map.g), static_cast<u32>(map.b), static_cast<u32>(map.a));
		}

		g_sampled = true;
		return result.get();
	}

	void destroy()
	{
		g_sampled = false;
		g_state.reset();
	}
}

// Called on an SPU thread whenever the lighting job starts, with its parameter block and the light table its input
// list is about to load (the pointer inside the parameter block is not what it loads). True when the GPU produces this
// frame's images and the game is known to sample them.
bool native_lighting_job_start(const u8* params, u32 table, u32 table_bytes)
{
	using namespace vk::native_lighting;

	const u32 count = *reinterpret_cast<const be_t<u32>*>(params + light_count_offset);
	const bool readable = count <= max_lights && table_bytes >= count * light_bytes && (!count || vm::check_addr(table, vm::page_readable, count * light_bytes));

	if (!readable)
	{
		warn_unsupported(fmt::format("%u lights, table 0x%x of %u bytes", count, table, table_bytes));
	}

	const u8* lights = readable && count ? vm::get_super_ptr<const u8>(table) : nullptr;
	const u32 bytes = readable ? count * light_bytes : 0;

	std::lock_guard lock(g_job.mutex);
	g_job.starts++;

	if (!g_job.seen || std::memcmp(g_job.params.data(), params, parameter_bytes) || g_job.lights.size() != bytes || (bytes && std::memcmp(g_job.lights.data(), lights, bytes)))
	{
		std::memcpy(g_job.params.data(), params, parameter_bytes);
		g_job.lights.assign(lights, lights + bytes);
		g_job.supported = readable && pack_input(g_job.params.data(), g_job.lights.data(), count, g_job.words);
		g_job.seen = true;
	}

	return g_job.supported && g_sampled.load(std::memory_order_relaxed);
}
