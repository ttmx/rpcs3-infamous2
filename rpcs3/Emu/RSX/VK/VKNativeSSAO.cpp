#include "stdafx.h"
#include "VKNativeSSAO.h"
#include "VKOverlays.h"
#include "VKRenderPass.h"
#include "VKHelpers.h"
#include "VKResourceManager.h"
#include "VKGpuPassProfile.hpp"
#include "vkutils/image.h"
#include "vkutils/buffer_object.h"
#include "vkutils/device.h"
#include "Emu/Memory/vm.h"
#include "Emu/System.h"
#include "Emu/infamous_titles.h"
#include "Emu/RSX/RSXThread.h"

#include <cmath>
#include <cstdlib>

LOG_CHANNEL(ssao_log, "NativeSSAO");

extern std::atomic<bool> g_native_ssao_gpu_active;

// inFamous 2 (BCES01143, 1.04) computes its ambient occlusion in an SPU job, from the copies of the normals and the
// depth image of its G-buffer that the game blits to main memory every frame. The job works at half resolution:
// linear depth, 12 occlusion samples per pixel, a depth-aware 5-tap blur in each direction, and a depth-aware
// upsample to the full-size 8-bit image the game uses as a texture.
//
// Here the same five stages run as fragment passes when both blits of a frame have arrived (on_gbuffer), and the game
// gets their output when it binds the occlusion texture or the half-resolution depth (substitute).
//
// The passes work at the size of the render targets the game blits from, 1280x720 times the resolution scale, and at
// half of that. Distances the job has in pixels are scaled with the image, so the result covers the same part of the
// picture at every scale.
namespace vk::native_ssao
{
	namespace
	{
		// Guest memory: the two G-buffer copies, the job's output texture, its half-resolution depth, its camera matrix
		// Sizes of the game's own textures
		constexpr u32 guest_w = 1280, guest_h = 720, guest_half_w = 640, guest_half_h = 360;

		const char* fs_header = R"(
#version 440
layout(set=0, binding=0) uniform sampler2D fs0;
layout(set=0, binding=1) uniform sampler2D fs1;
layout(set=0, binding=2) uniform sampler2D fs2;
layout(location=0) out vec4 ocol;

float linear_depth(vec4 texel)
{
	// The game keeps 24-bit depth in the A, R, G channels of a colour target.
	// 1/z = 0.100000347 - 5.96042646e-09 * d24, arranged to stay exact near the far plane
	ivec3 b = ivec3(texel.arg * 255.0 + 0.5);
	int d24 = (b.x << 16) | (b.y << 8) | b.z;
	return 1.0 / (5.96042646e-09 * (float(16777381 - d24) + 0.2278526));
}
)";

		// fs0: depth
		const char* fs_downsample = R"(
void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	ocol = vec4(linear_depth(texelFetch(fs0, p * 2, 0)));
}
)";

		// fs0: half-resolution linear depth, fs1: normals. The constants are the job's, for a 640 pixel wide image.
		const char* fs_occlusion = R"(
// Rows of the camera rotation
layout(push_constant) uniform static_data
{
	vec4 m0;
	vec4 m1;
	vec4 m2;
};

// The 12 samples: offset in the image plane and height of the sample sphere there
const float SX[12] = float[12](0.0, 0.360393, 0.767721, 0.0606153, -0.347511, -0.37089, 0.381673, -0.488981, -0.779765, -0.339852, 0.021904, 0.393308);
const float SY[12] = float[12](0.0, -0.221962, -0.0020895, -0.561965, 0.688986, 0.221229, 0.682595, -0.294946, 0.0797645, -0.691509, 0.448074, 0.250064);
const float SH[12] = float[12](1.0, 0.906008, 0.640781, 0.824937, 0.63603, 0.901942, 0.623209, 0.820917, 0.620971, 0.637428, 0.893728, 0.884747);

void main()
{
	// Projection scale of the sample offsets and radius of the sample sphere
	const float S = 5589.43, R = 15.0;
	ivec2 p = ivec2(gl_FragCoord.xy);
	ivec2 last = textureSize(fs0, 0) - 1;
	float scale = float(last.x + 1) / 640.0;
	float z = texelFetch(fs0, p, 0).r;
	vec3 n = floor(texelFetch(fs1, p * 2, 0).rgb * 255.0 + 0.5) * (2.007843137 / 256.0) - 1.0;
	float c0 = dot(n, m0.xyz), c1 = dot(n, m1.xyz), c2 = dot(n, m2.xyz);
	// The sphere keeps its size on screen nearer than the first and beyond the second depth
	float zcl = clamp(z, 1050.52, 2101.04);
	float s = z / zcl;
	float zref = z + s * c2;
	float total = 0.0;
	for (int i = 0; i < 12; ++i)
	{
		vec2 o = vec2(S * SX[i] + c0, S * SY[i] + c1) / zcl * scale;
		ivec2 q = clamp(ivec2(floor(vec2(p) + 0.5 + o)), ivec2(0), last);
		float h = SH[i] * R * s;
		float d = 0.5 * (texelFetch(fs0, q, 0).r - zref + h);
		total += clamp(d, 0.0, h) + h * clamp(d * (-1.0 / 60.0) / s - 0.75 * s, 0.0, 1.0);
	}
	// 140.86047: the total of an unoccluded pixel
	float vis = total / (140.86047 * s);
	ocol = vec4((floor(clamp(1.2 * vis - 0.2, 0.0, 1.0) * 255.0) + 0.5) / 255.0);
}
)";

		// fs0: occlusion, fs1: half-resolution linear depth. Binomial weights, reduced where the depth differs.
		// The taps are one pixel apart in a 640 pixel wide image.
		const char* fs_blur = R"(
void main()
{
	const float G[5] = float[5](1.0, 6.0, 10.0, 6.0, 1.0);
	const ivec2 dir = ivec2(%dir);
	ivec2 p = ivec2(gl_FragCoord.xy);
	ivec2 last = textureSize(fs0, 0) - 1;
	float scale = float(last.x + 1) / 640.0;
	float zc = texelFetch(fs1, p, 0).r;
	float num = 0.0, den = 0.0;
	for (int t = -2; t <= 2; ++t)
	{
		ivec2 q = p + int(round(float(t) * scale)) * dir;
		if (q.x < 0 || q.y < 0 || q.x > last.x || q.y > last.y) continue;
		float a = floor(texelFetch(fs0, q, 0).r * 255.0 + 0.5);
		float w = G[t + 2] * clamp(1.0 - 30.0 * abs(texelFetch(fs1, q, 0).r - zc) / zc, 0.0, 1.0);
		num += w * a;
		den += w;
	}
	ocol = vec4((floor(num / den) + 0.5) / 255.0);
}
)";

		// fs0: blurred occlusion, fs1: half-resolution linear depth, fs2: full-resolution depth.
		// Bilinear weights, reduced where the depth differs; the nearest texel if no weight is left.
		const char* fs_upsample = R"(
void main()
{
	ivec2 p = ivec2(gl_FragCoord.xy);
	float zf = linear_depth(texelFetch(fs2, p, 0));
	vec2 uv = (vec2(p) - 0.5) * 0.5;
	ivec2 b = ivec2(floor(uv));
	vec2 f = uv - vec2(b);
	float num = 0.0, den = 0.0, best = 0.0, bestw = -1.0;
	for (int i = 0; i < 4; ++i)
	{
		ivec2 o = ivec2(i & 1, i >> 1);
		float wb = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
		ivec2 q = clamp(b + o, ivec2(0), textureSize(fs0, 0) - 1);
		float a = floor(texelFetch(fs0, q, 0).r * 255.0 + 0.5);
		float w = wb * clamp(1.0 - 40.0 * abs(texelFetch(fs1, q, 0).r - zf) / zf, 0.0, 1.0);
		num += w * a;
		den += w;
		if (wb > bestw) { bestw = wb; best = a; }
	}
	float v = den > 1e-9 ? num / den : best;
	ocol = vec4((floor(v) + 0.5) / 255.0);
}
)";

		struct pass : public vk::overlay_pass
		{
			f32 constants[12]{};
			bool has_constants = false;

			pass(std::string body, u32 samplers, bool push_constants)
			{
				vs_src =
				#include "../Program/GLSLSnippets/GenericVSPassthrough.glsl"
				;
				fs_src = std::string(fs_header) + body;
				has_constants = push_constants;

				renderpass_config.set_depth_mask(false);
				renderpass_config.set_color_mask(0, true, true, true, true);
				renderpass_config.set_attachment_count(1);

				m_num_usable_samplers = samplers;
				m_num_uniform_buffers = 0;
				m_sampler_filter = VK_FILTER_NEAREST;
			}

			std::vector<vk::glsl::program_input> get_fragment_inputs() override
			{
				auto result = overlay_pass::get_fragment_inputs();
				if (has_constants)
				{
					result.push_back(vk::glsl::program_input::make(::glsl::glsl_fragment_program, "push_constants",
						vk::glsl::input_type_push_constant, 0, 0, glsl::push_constant_ref{ .size = sizeof(constants) }));
				}
				return result;
			}

			void update_uniforms(vk::command_buffer& cmd, vk::glsl::program* program) override
			{
				if (has_constants)
				{
					vkCmdPushConstants(cmd, program->layout(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), constants);
				}
			}
		};

		struct state_t
		{
			std::unique_ptr<pass> downsample, occlusion, blur_h, blur_v, upsample;
			std::unique_ptr<vk::viewable_image> z_half, ao_raw, ao_h, ao_v, ao_full;
			std::unordered_map<u32, std::unique_ptr<vk::image_view>> output_views;
			std::unordered_map<u32, std::unique_ptr<vk::image_view>> half_depth_views;
			vk::image_view* half_depth_view = nullptr;

			vk::image* normals = nullptr;
			vk::image* depth = nullptr;
			u32 width = 0, height = 0; // Of ao_full; the other four have half of it, rounded up
			bool valid = false;
		};

		std::unique_ptr<state_t> g_state;

		std::unique_ptr<vk::viewable_image> make_image(const vk::render_device& dev, VkFormat format, u32 w, u32 h)
		{
			return std::make_unique<vk::viewable_image>(dev, dev.get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				VK_IMAGE_TYPE_2D, format, w, h, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_TILING_OPTIMAL,
				VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0,
				VMM_ALLOCATION_POOL_UNDEFINED);
		}

		vk::image_view* view_of(vk::image* image, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
		{
			return static_cast<vk::viewable_image*>(image)->get_view(rsx::default_remap_vector.with_encoding(VK_REMAP_IDENTITY), aspect);
		}

		void run_pass(vk::command_buffer& cmd, pass& p, vk::image* target, const std::vector<vk::image_view*>& src)
		{
			auto& dev = cmd.get_command_pool().get_owner();
			target->change_layout(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
			const auto renderpass = vk::get_renderpass(dev, vk::get_renderpass_key(std::vector<vk::image*>{ target }));
			p.run(cmd, areau{ 0, 0, target->width(), target->height() }, target, src, renderpass);
			vk::end_renderpass(cmd);
			target->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

		void run(vk::command_buffer& cmd, state_t& s)
		{
			auto& dev = cmd.get_command_pool().get_owner();

			if (!s.downsample)
			{
				s.downsample = std::make_unique<pass>(fs_downsample, 1, false);
				s.occlusion = std::make_unique<pass>(fs_occlusion, 2, true);
				s.blur_h = std::make_unique<pass>(fmt::replace_all(fs_blur, "%dir", "1, 0"), 2, false);
				s.blur_v = std::make_unique<pass>(fmt::replace_all(fs_blur, "%dir", "0, 1"), 2, false);
				s.upsample = std::make_unique<pass>(fs_upsample, 3, false);
				for (auto* p : { s.downsample.get(), s.occlusion.get(), s.blur_h.get(), s.blur_v.get(), s.upsample.get() }) p->create(dev);
				ssao_log.success("GPU ambient occlusion active (mode %u)", mode());
			}

			// First frame, or the resolution scale was changed
			if (s.depth->width() != s.width || s.depth->height() != s.height)
			{
				auto& resources = *vk::get_resource_manager();

				for (auto* views : { &s.output_views, &s.half_depth_views })
				{
					for (auto& view : *views) resources.dispose(view.second);
					views->clear();
				}

				for (auto* image : { &s.z_half, &s.ao_raw, &s.ao_h, &s.ao_v, &s.ao_full })
				{
					if (*image) resources.dispose(*image);
				}

				s.width = s.depth->width();
				s.height = s.depth->height();
				s.half_depth_view = nullptr;
				s.valid = false;

				const u32 half_w = (s.width + 1) / 2, half_h = (s.height + 1) / 2;
				s.z_half = make_image(dev, VK_FORMAT_R32_SFLOAT, half_w, half_h);
				s.ao_raw = make_image(dev, VK_FORMAT_R8_UNORM, half_w, half_h);
				s.ao_h = make_image(dev, VK_FORMAT_R8_UNORM, half_w, half_h);
				s.ao_v = make_image(dev, VK_FORMAT_R8_UNORM, half_w, half_h);
				s.ao_full = make_image(dev, VK_FORMAT_R8_UNORM, s.width, s.height);
				ssao_log.notice("GPU ambient occlusion at %ux%u", s.width, s.height);
			}

			// Camera matrix from the job's parameter block (big-endian floats, rows as stored)
			f32 m[16];
			for (u32 i = 0; i < 16; ++i)
			{
				m[i] = vm::_ref<be_t<f32>>(infamous_native::current().matrix + i * 4);
				if (!std::isfinite(m[i])) return;
			}
			for (u32 col = 0; col < 3; ++col)
			{
				for (u32 row = 0; row < 3; ++row) s.occlusion->constants[col * 4 + row] = m[row * 4 + col];
			}

			if (vk::is_renderpass_open(cmd)) vk::end_renderpass(cmd);
			vk::gpu_pass_profile::mark(cmd, vk::gpu_pass_profile::label_occlusion);

			s.depth->push_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			s.normals->push_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			const auto depth_view = view_of(s.depth);
			const auto normals_view = view_of(s.normals);

			run_pass(cmd, *s.downsample, s.z_half.get(), { depth_view });
			run_pass(cmd, *s.occlusion, s.ao_raw.get(), { view_of(s.z_half.get()), normals_view });
			run_pass(cmd, *s.blur_h, s.ao_h.get(), { view_of(s.ao_raw.get()), view_of(s.z_half.get()) });
			run_pass(cmd, *s.blur_v, s.ao_v.get(), { view_of(s.ao_h.get()), view_of(s.z_half.get()) });
			run_pass(cmd, *s.upsample, s.ao_full.get(), { view_of(s.ao_v.get()), view_of(s.z_half.get()), depth_view });

			s.normals->pop_layout(cmd);
			s.depth->pop_layout(cmd);
			vk::gpu_pass_profile::mark(cmd, vk::gpu_pass_profile::label_occlusion_user);

			s.valid = true;
		}
	}

	u32 mode()
	{
		static const u32 value = []() -> u32
		{
			const char* flag = std::getenv("RPCS3_NATIVE_SSAO");
			return flag ? static_cast<u32>(std::strtoul(flag, nullptr, 0)) : 0;
		}();
		return value;
	}

	void on_gbuffer(vk::command_buffer& cmd, vk::image* src, const areai& src_area, u32 dst_address)
	{
		if (!mode() || (dst_address != infamous_native::current().normals && dst_address != infamous_native::current().depth)) return;
		if (src_area.x1 != 0 || src_area.y1 != 0 || src_area.width() != static_cast<s32>(src->width()) || src_area.height() != static_cast<s32>(src->height()) ||
			src->width() < guest_w / 4 || src->height() < guest_h / 4 || src->samples() != 1)
		{
			return;
		}

		if (!g_state) g_state = std::make_unique<state_t>();
		auto& s = *g_state;

		if (src->format() != VK_FORMAT_B8G8R8A8_UNORM || !rsx::get_current_renderer()->is_current_thread()) return;
		(dst_address == infamous_native::current().normals ? s.normals : s.depth) = src;

		if (s.normals && s.depth)
		{
			if (s.normals->width() == s.depth->width() && s.normals->height() == s.depth->height())
			{
				run(cmd, s);
			}

			s.normals = s.depth = nullptr;
		}
	}

	bool is_half_depth(u32 texture_address)
	{
		return texture_address == infamous_native::current().half_depth;
	}

	vk::image_view* half_depth_view()
	{
		return (mode() & 1) && g_state && g_state->valid ? g_state->half_depth_view : nullptr;
	}

	vk::image_view* substitute(vk::command_buffer& /*cmd*/, vk::image_view* original, u32 texture_address)
	{
		if ((texture_address != infamous_native::current().occlusion && texture_address != infamous_native::current().half_depth) || !g_state || !g_state->valid || !original) return nullptr;
		auto& s = *g_state;

		if (texture_address == infamous_native::current().half_depth)
		{
			// The downsample pass output is what the job's first kernel stores there
			if (!(mode() & 1) || original->format() != VK_FORMAT_R32_SFLOAT || original->image()->width() != guest_half_w || original->image()->height() != guest_half_h)
			{
				return nullptr;
			}

			const auto& map = original->info.components;
			const u32 key = (map.r & 0xff) | ((map.g & 0xff) << 8) | ((map.b & 0xff) << 16) | ((map.a & 0xff) << 24);
			auto& view = s.half_depth_views[key];
			if (!view)
			{
				view = std::make_unique<vk::image_view>(*vk::get_current_renderer(), s.z_half.get(), VK_FORMAT_R32_SFLOAT, VK_IMAGE_VIEW_TYPE_2D, map);
				ssao_log.success("game now samples the GPU half-resolution depth image (component map %u %u %u %u)", static_cast<u32>(map.r), static_cast<u32>(map.g), static_cast<u32>(map.b), static_cast<u32>(map.a));
			}
			return s.half_depth_view = view.get();
		}

		if (!(mode() & 1)) return nullptr;

		if (original->format() != VK_FORMAT_R8_UNORM || original->image()->width() != guest_w || original->image()->height() != guest_h)
		{
			if (static bool logged = false; !logged)
			{
				logged = true;
				ssao_log.error("unexpected occlusion texture (format %u, %ux%u), keeping the guest image", static_cast<u32>(original->format()), original->image()->width(), original->image()->height());
			}
			return nullptr;
		}

		const auto& map = original->info.components;
		const u32 key = (map.r & 0xff) | ((map.g & 0xff) << 8) | ((map.b & 0xff) << 16) | ((map.a & 0xff) << 24);
		auto& view = s.output_views[key];
		if (!view)
		{
			view = std::make_unique<vk::image_view>(*vk::get_current_renderer(), s.ao_full.get(), VK_FORMAT_R8_UNORM, VK_IMAGE_VIEW_TYPE_2D, map);
			g_native_ssao_gpu_active = true;
			ssao_log.success("game now samples the GPU occlusion image (component map %u %u %u %u)", static_cast<u32>(map.r), static_cast<u32>(map.g), static_cast<u32>(map.b), static_cast<u32>(map.a));
		}
		return view.get();
	}

	void destroy()
	{
		g_native_ssao_gpu_active = false;
		g_state.reset();
	}
}
