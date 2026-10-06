#include "stdafx.h"
#include "../Common/BufferUtils.h"
#include "../Program/GLSLCommon.h"
#include "../rsx_methods.h"

#include "VKAsyncScheduler.h"
#include "VKNativeSSAO.h"
#include "VKNativeLighting.h"
#include "VKGSRender.h"
#include "VKPassTiming.hpp"
#include "VKGpuPassProfile.hpp"
#include "VKLiveCtl.hpp"
#include "Emu/infamous_titles.h"
#include "vkutils/buffer_object.h"
#include "vkutils/chip_class.h"
#include <vulkan/vulkan_core.h>

namespace vk
{
	VkImageViewType get_view_type(rsx::texture_dimension_extended type)
	{
		switch (type)
		{
		case rsx::texture_dimension_extended::texture_dimension_1d:
			return VK_IMAGE_VIEW_TYPE_1D;
		case rsx::texture_dimension_extended::texture_dimension_2d:
			return VK_IMAGE_VIEW_TYPE_2D;
		case rsx::texture_dimension_extended::texture_dimension_cubemap:
			return VK_IMAGE_VIEW_TYPE_CUBE;
		case rsx::texture_dimension_extended::texture_dimension_3d:
			return VK_IMAGE_VIEW_TYPE_3D;
		default: fmt::throw_exception("Unreachable");
		}
	}

	VkCompareOp get_compare_func(rsx::comparison_function op, bool reverse_direction = false)
	{
		switch (op)
		{
		case rsx::comparison_function::never: return VK_COMPARE_OP_NEVER;
		case rsx::comparison_function::greater: return reverse_direction ? VK_COMPARE_OP_LESS: VK_COMPARE_OP_GREATER;
		case rsx::comparison_function::less: return reverse_direction ? VK_COMPARE_OP_GREATER: VK_COMPARE_OP_LESS;
		case rsx::comparison_function::less_or_equal: return reverse_direction ? VK_COMPARE_OP_GREATER_OR_EQUAL: VK_COMPARE_OP_LESS_OR_EQUAL;
		case rsx::comparison_function::greater_or_equal: return reverse_direction ? VK_COMPARE_OP_LESS_OR_EQUAL: VK_COMPARE_OP_GREATER_OR_EQUAL;
		case rsx::comparison_function::equal: return VK_COMPARE_OP_EQUAL;
		case rsx::comparison_function::not_equal: return VK_COMPARE_OP_NOT_EQUAL;
		case rsx::comparison_function::always: return VK_COMPARE_OP_ALWAYS;
		default:
			fmt::throw_exception("Unknown compare op: 0x%x", static_cast<u32>(op));
		}
	}

	void validate_image_layout_for_read_access(
		vk::command_buffer& cmd,
		vk::image_view* view,
		VkPipelineStageFlags dst_stage,
		const rsx::sampled_image_descriptor_base* sampler_state)
	{
		switch (auto raw = view->image(); +raw->current_layout)
		{
		default:
			//case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
			//ensure(sampler_state->upload_context == rsx::texture_upload_context::blit_engine_dst);
			raw->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			break;
		case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
			ensure(sampler_state->upload_context == rsx::texture_upload_context::blit_engine_src);
			raw->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			break;
		case VK_IMAGE_LAYOUT_GENERAL:
		case VK_IMAGE_LAYOUT_ATTACHMENT_FEEDBACK_LOOP_OPTIMAL_EXT:
			ensure(sampler_state->upload_context == rsx::texture_upload_context::framebuffer_storage);
			if (sampler_state->is_cyclic_reference) [[ unlikely ]]
			{
				// Nothing to do
				break;
			}

			// This was used in a cyclic ref before, but is missing a barrier
			// No need for a full stall, use a custom barrier instead
			VkPipelineStageFlags src_stage;
			VkAccessFlags src_access, dst_access;
			if (raw->aspect() == VK_IMAGE_ASPECT_COLOR_BIT)
			{
				src_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
				src_access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
				dst_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
				dst_stage |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
			}
			else
			{
				src_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
				src_access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
				dst_access = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
				dst_stage |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
			}

			vk::insert_image_memory_barrier(
				cmd,
				raw->value,
				raw->current_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				src_stage, dst_stage,
				src_access, dst_access,
				{ raw->aspect(), 0, 1, 0, 1 });

			raw->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			break;
		case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
		case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
			ensure(sampler_state->upload_context == rsx::texture_upload_context::framebuffer_storage);
			if (!sampler_state->is_cyclic_reference) [[ likely ]]
			{
				// Standard pre-read barrier.
				raw->change_layout(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
				break;
			}

			// Normally this shouldn't happen. But that is only guaranteed if the attachment state never changes outside of RTT rebind interrupts.
			// If the shader changes between binds to "disable" the cyclic nature, we could end up here.
			// Draw 1 (cyllic) -> texture_barrier -> Draw 2 (no textures) -> attachment_optimal -> Draw 3 (cylic again, no new data) -> incorrect layout.
			vk::as_rtt(raw)->texture_barrier(cmd);
			break;
		}
	}
}

void VKGSRender::begin_render_pass()
{
	vk::begin_renderpass(
		*m_current_command_buffer,
		get_render_pass(),
		m_draw_fbo->value,
		{ positionu{0u, 0u}, sizeu{m_draw_fbo->width(), m_draw_fbo->height()} });
}

void VKGSRender::close_render_pass()
{
	vk::end_renderpass(*m_current_command_buffer);
}

VkRenderPass VKGSRender::get_render_pass()
{
	if (!m_cached_renderpass)
	{
		m_cached_renderpass = vk::get_renderpass(*m_device, m_current_renderpass_key);
	}

	return m_cached_renderpass;
}

void VKGSRender::invalidate_render_pass()
{
	// Regenerate renderpass key for the next draw call
	std::vector<u8> input_attachments{};
	if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING)
	{
		input_attachments.resize(m_draw_buffers.size());
		std::iota(input_attachments.begin(), input_attachments.end(), 0);
	}

	if (const auto key = vk::get_renderpass_key(m_fbo_images, m_current_renderpass_key, input_attachments);
		key != m_current_renderpass_key)
	{
		m_current_renderpass_key = key;
		m_cached_renderpass = VK_NULL_HANDLE;
	}
}

void VKGSRender::set_depth_bias_state()
{
	if (rsx::method_registers.poly_offset_fill_enabled())
	{
		// offset_bias is the constant factor, multiplied by the implementation factor R
		// offst_scale is the slope factor, multiplied by the triangle slope factor M
		// R is implementation dependent and has to be derived empirically for supported implementations.
		// Lucky for us, only NVIDIA currently supports fixed-point 24-bit depth buffers.

		const auto polygon_offset_scale = rsx::method_registers.poly_offset_scale();
		auto polygon_offset_bias = rsx::method_registers.poly_offset_bias();

		if (m_draw_fbo->depth_format() == VK_FORMAT_D24_UNORM_S8_UINT && is_NVIDIA(vk::get_chip_family()))
		{
			// Empirically derived to be 0.5 * (2^24 - 1) for fixed type on Pascal. The same seems to apply for other NVIDIA GPUs.
			// RSX seems to be using 2^24 - 1 instead making the biases twice as large when using fixed type Z-buffer on NVIDIA.
			// Note, that the formula for floating point is complicated, but actually works out for us.
			// Since the exponent range for a polygon is around 0, and we have 23 (+1) mantissa bits, R just works out to the same range by chance \o/.
			polygon_offset_bias *= 0.5f;
		}

		vkCmdSetDepthBias(*m_current_command_buffer, polygon_offset_bias, 0.f, polygon_offset_scale);
	}
	else
	{
		// Zero bias value - disables depth bias
		vkCmdSetDepthBias(*m_current_command_buffer, 0.f, 0.f, 0.f);
	}
}

void VKGSRender::update_draw_state()
{
	m_profiler.start();

	// Update conditional dynamic state
	if (rsx::method_registers.current_draw_clause.primitive >= rsx::primitive_type::points &&   // AMD/AMDVLK driver does not like it if you render points without setting line width for some reason
		rsx::method_registers.current_draw_clause.primitive <= rsx::primitive_type::line_strip)
	{
		const float actual_line_width =
			m_device->get_wide_lines_support() ? rsx::method_registers.line_width() * framebuffer_scaling_config().scale_factor() : 1.f;
		vkCmdSetLineWidth(*m_current_command_buffer, actual_line_width);
	}

	if (rsx::method_registers.blend_enabled_mask())
	{
		// Update blend constants
		auto blend_colors = rsx::get_constant_blend_colors();
		vkCmdSetBlendConstants(*m_current_command_buffer, blend_colors.data());
	}

	if (rsx::method_registers.stencil_test_enabled())
	{
		const bool two_sided_stencil = rsx::method_registers.two_sided_stencil_test_enabled();
		VkStencilFaceFlags face_flag = (two_sided_stencil) ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FRONT_AND_BACK;

		vkCmdSetStencilWriteMask(*m_current_command_buffer, face_flag, rsx::method_registers.stencil_mask());
		vkCmdSetStencilCompareMask(*m_current_command_buffer, face_flag, rsx::method_registers.stencil_func_mask());
		vkCmdSetStencilReference(*m_current_command_buffer, face_flag, rsx::method_registers.stencil_func_ref());

		if (two_sided_stencil)
		{
			vkCmdSetStencilWriteMask(*m_current_command_buffer, VK_STENCIL_FACE_BACK_BIT, rsx::method_registers.back_stencil_mask());
			vkCmdSetStencilCompareMask(*m_current_command_buffer, VK_STENCIL_FACE_BACK_BIT, rsx::method_registers.back_stencil_func_mask());
			vkCmdSetStencilReference(*m_current_command_buffer, VK_STENCIL_FACE_BACK_BIT, rsx::method_registers.back_stencil_func_ref());
		}
	}

	// The remaining dynamic state should only be set once and we have signals to enable/disable mid-renderpass
	if (!(m_current_command_buffer->flags & vk::command_buffer::cb_reload_dynamic_state))
	{
		// Dynamic state already set
		m_frame_stats.setup_time += m_profiler.duration();
		return;
	}

	set_depth_bias_state();

	if (m_device->get_depth_bounds_support())
	{
		f32 bounds_min, bounds_max;
		if (rsx::method_registers.depth_bounds_test_enabled())
		{
			// Update depth bounds min/max
			bounds_min = rsx::method_registers.depth_bounds_min();
			bounds_max = rsx::method_registers.depth_bounds_max();
		}
		else
		{
			// Avoid special case where min=max and depth bounds (incorrectly) fails
			bounds_min = std::min(0.f, rsx::method_registers.clip_min());
			bounds_max = std::max(1.f, rsx::method_registers.clip_max());
		}

		if (!m_device->get_unrestricted_depth_range_support())
		{
			bounds_min = std::clamp(bounds_min, 0.f, 1.f);
			bounds_max = std::clamp(bounds_max, 0.f, 1.f);
		}

		vkCmdSetDepthBounds(*m_current_command_buffer, bounds_min, bounds_max);
	}

	bind_viewport();

	m_current_command_buffer->flags &= ~vk::command_buffer::cb_reload_dynamic_state;
	m_graphics_state.clear(rsx::pipeline_state::polygon_offset_state_dirty | rsx::pipeline_state::depth_bounds_state_dirty);
	m_frame_stats.setup_time += m_profiler.duration();
}

void VKGSRender::load_texture_env()
{
	auto* const texture_diagnostics = diagnostic_texture_values();
	if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::load_calls);
	// Load textures
	bool check_for_cyclic_refs = false;
	auto check_surface_cache_sampler_valid = [&](auto descriptor, const auto& tex)
	{
		if (!m_texture_cache.test_if_descriptor_expired(*m_current_command_buffer, m_rtts, descriptor, tex))
		{
			check_for_cyclic_refs |= descriptor->is_cyclic_reference;
			return true;
		}

		return false;
	};

	auto get_border_color = [&](const rsx::Texture auto& tex, bool remap_colorspace)
	{
		return m_device->get_custom_border_color_support().require_border_color_remap
			? tex.remapped_border_color(remap_colorspace)
			: rsx::decode_border_color(tex.border_color(remap_colorspace));
	};

	std::lock_guard lock(m_sampler_mutex);
	// Live control 10: bits 0-1 = mode (1 reuse, 2 full lookup with the proposed reuse checked), bit 2 = bindings of
	// textures away from every surface are kept across surface cache generations
	const auto material_mode = vk::live_ctl::get(10) & 3;
	const bool material_surfaces = (vk::live_ctl::get(10) & 4) != 0;
	if (m_samplers_dirty || !material_mode) m_material_bindings.invalidate();
	const auto synchronize_materials = [&](const std::array<u64, 3>& generation)
	{
		m_material_bindings.synchronize(generation, material_surfaces, [&](auto&& func) { m_rtts.for_each_surface_range(func); });
	};
	const auto material_generation = [&]() -> std::array<u64, 3>
	{
		const auto tags = m_texture_cache.material_cache_generation();
		return {tags[0], tags[1], m_rtts.cache_tag};
	};

	for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
	{
		if (!(textures_ref & 1))
		{
			continue;
		}

		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::fs_slots);
		if (!fs_sampler_state[i])
		{
			fs_sampler_state[i] = std::make_unique<vk::texture_cache::sampled_image_descriptor>();
		}

		auto sampler_state = static_cast<vk::texture_cache::sampled_image_descriptor*>(fs_sampler_state[i].get());
		const auto& tex = rsx::method_registers.fragment_textures[i];
		const auto previous_format_class = fs_sampler_state[i]->format_class;

		if (!m_samplers_dirty &&
			!m_textures_dirty[i] &&
			check_surface_cache_sampler_valid(sampler_state, tex))
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::clean_reuse);
			continue;
		}

		const bool is_sampler_dirty = m_textures_dirty[i];
		m_textures_dirty[i] = false;

		vk::material_bindings::key_type material_key{};
		const bool cache_material = material_mode && tex.enabled() && tex.is_compressed_format();
		const vk::material_bindings::entry* expected_material = nullptr;
		std::array<u64, 3> lookup_generation{};
		if (cache_material)
		{
			std::memcpy(material_key.data(), &rsx::method_registers.registers[NV4097_SET_TEXTURE_OFFSET + i * 8], 8 * sizeof(u32));
			material_key[8] = rsx::method_registers.registers[NV4097_SET_TEXTURE_CONTROL2 + i];
			material_key[9] = rsx::method_registers.registers[NV4097_SET_TEXTURE_CONTROL3 + i];
			material_key[10] = (current_fp_metadata.bx2_texture_reads_mask >> i) & 1;
			lookup_generation = material_generation();
			synchronize_materials(lookup_generation);
			expected_material = m_material_bindings.find(material_key);
			if (expected_material && material_mode == 1)
			{
				*sampler_state = expected_material->descriptor;
				vk::material_bindings::bind_sampler(fs_sampler_handles[i], expected_material->handle);
				if (!is_sampler_dirty && previous_format_class != sampler_state->format_class)
					m_graphics_state |= rsx::fragment_program_state_dirty;
				continue;
			}
		}

		// Called only after the complete path has finished both the image and sampler setup.
		const auto remember_material = [&]()
		{
			if (!cache_material) return;
			const auto generation = material_generation();
			if (expected_material && material_mode == 2 && generation == lookup_generation)
			{
				m_material_bindings.checked++;
				if (!vk::material_bindings::matches(*expected_material, *sampler_state, fs_sampler_handles[i]))
				{
					if (m_material_bindings.mismatches++ < 8)
						rsx_log.error("Material binding check differs at texture slot %u, address 0x%x", i, tex.offset());
				}
			}
			synchronize_materials(generation);
			if (vk::material_bindings::eligible(*sampler_state, fs_sampler_handles[i]))
			{
				// Everything the lookup could have read: all faces and mipmaps, or the padded rows of the base level
				const u64 rows = u64{std::max<u32>(tex.pitch(), tex.width() * 4)} * tex.height() * std::max<u32>(tex.depth(), 1) * (tex.cubemap() ? 6 : 1);
				const bool surface_dependent = m_material_bindings.on_surface_pages(rsx::get_address(tex.offset(), tex.location()), std::max<u64>(rows, get_texture_size(tex)));
				m_material_bindings.store(material_key, *sampler_state, fs_sampler_handles[i], surface_dependent);
			}
		};

		if (!tex.enabled())
		{
			*sampler_state = {};
			continue;
		}

		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uploads);
		// Diagnostic-only image input identity; the original upload always executes.
		std::array<u32, 18> image_inputs{};
		bool prospective_key_match = false;
		std::uintptr_t previous_view = 0;
		if (texture_diagnostics)
		{
			image_inputs = {tex.offset(), tex.location(), tex.format(), tex.cubemap(), tex.border_type(),
				static_cast<u32>(tex.get_extended_texture_dimension()), tex.width(), tex.height(), tex.depth(), tex.pitch(),
				tex.mipmap(), tex.remap(), static_cast<u32>(tex.wrap_s()), static_cast<u32>(tex.wrap_t()),
				tex.format_ex().texel_remap_control, tex.argb_signed(), tex.gamma(), (current_fp_metadata.bx2_texture_reads_mask >> i) & 1u};
			prospective_key_match = !m_samplers_dirty && is_sampler_dirty && tex.is_compressed_format() &&
				sampler_state->upload_context == rsx::texture_upload_context::shader_read &&
				sampler_state->image_handle && m_native_texture_image_inputs_valid[i] && m_native_texture_image_inputs[i] == image_inputs;
			previous_view = reinterpret_cast<std::uintptr_t>(sampler_state->image_handle);
		}
		// inFamous 2 with GPU ambient occlusion: the half-resolution depth texture comes from the GPU pass. Once a binding
		// has been through the texture cache, later ones reuse its descriptor so that guest memory is not read at all
		// (the buffer shares a page with a G-buffer image that should not have to be read back).
		static struct
		{
			std::array<u32, 4> key{};
			vk::texture_cache::sampled_image_descriptor descriptor;
			bool valid = false;
		}
		native_half_depth;

		const std::array<u32, 4> native_key{ tex.format(), tex.width(), tex.height(), tex.remap() };
		vk::image_view* native_view = nullptr;

		if (vk::native_ssao::mode() && native_half_depth.valid && native_half_depth.key == native_key &&
			vk::native_ssao::is_half_depth(rsx::get_address(tex.offset(), tex.location()))) [[unlikely]]
		{
			native_view = vk::native_ssao::half_depth_view();
		}

		if (native_view)
		{
			*sampler_state = native_half_depth.descriptor;
			sampler_state->image_handle = native_view;
		}
		else
		{
			*sampler_state = m_texture_cache.upload_texture(*m_current_command_buffer, tex, m_rtts);
		}
		if (vk::native_lighting::mode() && sampler_state->image_handle) [[unlikely]]
		{
			if (auto view = vk::native_lighting::substitute(*m_current_command_buffer, sampler_state->image_handle, rsx::get_address(tex.offset(), tex.location())))
			{
				sampler_state->image_handle = view;
			}
		}
		if (vk::native_ssao::mode() && sampler_state->image_handle) [[unlikely]]
		{
			if (auto view = vk::native_ssao::substitute(*m_current_command_buffer, sampler_state->image_handle, rsx::get_address(tex.offset(), tex.location())))
			{
				sampler_state->image_handle = view;

				if (!native_view && vk::native_ssao::is_half_depth(rsx::get_address(tex.offset(), tex.location())))
				{
					native_half_depth.key = native_key;
					native_half_depth.descriptor = *sampler_state;
					native_half_depth.valid = true;
				}
			}
		}
		if (texture_diagnostics)
		{
			if (prospective_key_match)
			{
				texture_diagnostics->add_domain(native_texture_counters::prospective_key_matches);
				const bool same = sampler_state->upload_context == rsx::texture_upload_context::shader_read &&
					reinterpret_cast<std::uintptr_t>(sampler_state->image_handle) == previous_view;
				texture_diagnostics->add_domain(same ? native_texture_counters::stock_view_matches : native_texture_counters::stock_view_mismatches);
			}
			m_native_texture_image_inputs[i] = image_inputs;
			m_native_texture_image_inputs_valid[i] = sampler_state->validate();
		}
		if (texture_diagnostics)
		{
			using namespace native_texture_counters;
			const auto context = sampler_state->upload_context;
			texture_diagnostics->add_domain(context == rsx::texture_upload_context::shader_read ? shader_read_uploads :
				context == rsx::texture_upload_context::framebuffer_storage ? framebuffer_uploads :
				context == rsx::texture_upload_context::blit_engine_src ? blit_source_uploads :
				context == rsx::texture_upload_context::blit_engine_dst ? blit_destination_uploads : other_uploads);
			if (m_samplers_dirty) texture_diagnostics->add_domain(global_dirty_uploads);
			if (is_sampler_dirty) texture_diagnostics->add_domain(slot_dirty_uploads);
			const auto format = tex.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
			if (format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 || format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 || format == CELL_GCM_TEXTURE_COMPRESSED_DXT45)
				texture_diagnostics->add_domain(compressed_uploads);
			if (!m_samplers_dirty && is_sampler_dirty && context == rsx::texture_upload_context::shader_read)
				texture_diagnostics->add_domain(shader_read_slot_dirty_without_global);
			if (sampler_state->is_cyclic_reference) texture_diagnostics->add_domain(cyclic_uploads);
			if (sampler_state->image_handle) texture_diagnostics->add_domain(direct_image_uploads);
			if (!sampler_state->validate()) texture_diagnostics->add_domain(invalid_domain_uploads);
		}
		if (!sampler_state->validate())
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::invalid_uploads);
			continue;
		}

		if (sampler_state->is_cyclic_reference)
		{
			check_for_cyclic_refs |= true;
		}

		if (!is_sampler_dirty)
		{
			if (sampler_state->format_class != previous_format_class)
			{
				// Host details changed but RSX is not aware
				m_graphics_state |= rsx::fragment_program_state_dirty;
			}

			if (sampler_state->format_ex)
			{
				// Nothing to change, use cached sampler
				remember_material();
				continue;
			}
		}

		sampler_state->format_ex = tex.format_ex();

		if (sampler_state->format_ex.texel_remap_control &&
			sampler_state->image_handle &&
			sampler_state->upload_context == rsx::texture_upload_context::shader_read &&
			(current_fp_metadata.bx2_texture_reads_mask & (1u << i)) == 0 &&
			!g_cfg.video.disable_hardware_texel_remapping) [[ unlikely ]]
		{
			// Check if we need to override the view format
			const auto vk_format = sampler_state->image_handle->format();
			VkFormat format_override = vk_format;;
			rsx::flags32_t flags_to_erase = 0u;
			rsx::flags32_t host_flags_to_set = 0u;

			if (sampler_state->format_ex.hw_SNORM_possible())
			{
				format_override = vk::get_compatible_snorm_format(vk_format);
				flags_to_erase = rsx::texture_control_bits::SEXT_MASK;
				host_flags_to_set = rsx::RSX_HOST_FORMAT_FEATURE_SNORM;
			}
			else if (sampler_state->format_ex.hw_SRGB_possible())
			{
				format_override = vk::get_compatible_srgb_format(vk_format);
				flags_to_erase = rsx::texture_control_bits::GAMMA_CTRL_MASK;
				host_flags_to_set = rsx::RSX_HOST_FORMAT_FEATURE_SRGB;
			}

			if (format_override != VK_FORMAT_UNDEFINED && format_override != vk_format)
			{
				sampler_state->image_handle = sampler_state->image_handle->as(format_override);
				sampler_state->format_ex.texel_remap_control &= (~flags_to_erase);
				sampler_state->format_ex.host_features |= host_flags_to_set;
			}
		}

		VkFilter mag_filter;
		vk::minification_filter min_filter;
		f32 min_lod = 0.f, max_lod = 0.f;
		f32 lod_bias = 0.f;

		const u32 texture_format = sampler_state->format_ex.format();
		VkBool32 compare_enabled = VK_FALSE;
		VkCompareOp depth_compare_mode = VK_COMPARE_OP_NEVER;

		if (texture_format >= CELL_GCM_TEXTURE_DEPTH24_D8 && texture_format <= CELL_GCM_TEXTURE_DEPTH16_FLOAT)
		{
			compare_enabled = VK_TRUE;
			depth_compare_mode = vk::get_compare_func(tex.zfunc(), true);
		}

		const f32 af_level = vk::max_aniso(tex.max_aniso());
		const auto wrap_s = vk::vk_wrap_mode(tex.wrap_s());
		const auto wrap_t = vk::vk_wrap_mode(tex.wrap_t());
		const auto wrap_r = vk::vk_wrap_mode(tex.wrap_r());

		// NOTE: In vulkan, the border color can bypass the sample swizzle stage.
		// Check the device properties to determine whether to pre-swizzle the colors or not.
		const bool sext_conv_required = (sampler_state->format_ex.texel_remap_control & rsx::texture_control_bits::SEXT_MASK) != 0;
		vk::border_color_t border_color(VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);

		if (rsx::is_border_clamped_texture(tex))
		{
			auto color_value = get_border_color(tex, sext_conv_required);
			if (sampler_state->format_ex.host_snorm_format_active())
			{
				// Convert the border color in host space (2N - 1)
				// HW does the conversion in integer space as (x - 128) / 127 which introduces a biasing error.
				const float bias_v = 128.f / 255.f;
				const float scale_v = 255.f / 127.f;

				color4f scale{ 1.f }, bias{ 0.f };
				const auto snorm_mask = tex.argb_signed();
				if (snorm_mask & 1) { scale.a = scale_v; bias.a = -bias_v; }
				if (snorm_mask & 2) { scale.r = scale_v; bias.r = -bias_v; }
				if (snorm_mask & 4) { scale.g = scale_v; bias.g = -bias_v; }
				if (snorm_mask & 8) { scale.b = scale_v; bias.b = -bias_v; }
				color_value = (color_value + bias) * scale;
			}

			border_color = color_value;
		}

		// Check if non-point filtering can even be used on this format
		bool can_sample_linear;
		if (sampler_state->format_class == RSX_FORMAT_CLASS_COLOR) [[likely]]
		{
			// Most PS3-like formats can be linearly filtered without problem
			// Exclude textures that require SNORM conversion however
			can_sample_linear = !sext_conv_required;
		}
		else if (sampler_state->format_class != rsx::classify_format(texture_format) &&
			(texture_format == CELL_GCM_TEXTURE_A8R8G8B8 || texture_format == CELL_GCM_TEXTURE_D8R8G8B8))
		{
			// Depth format redirected to BGRA8 resample stage. Do not filter to avoid bits leaking
			can_sample_linear = false;
		}
		else
		{
			// Not all GPUs support linear filtering of depth formats
			const auto vk_format = sampler_state->image_handle ? sampler_state->image_handle->image()->format() :
				vk::get_compatible_sampler_format(m_device->get_formats_support(), sampler_state->external_subresource_desc.gcm_format);

			can_sample_linear = m_device->get_format_properties(vk_format).optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		}

		const auto mipmap_count = tex.get_exact_mipmap_count();
		min_filter = vk::get_min_filter(tex.min_filter());

		if (can_sample_linear)
		{
			mag_filter = vk::get_mag_filter(tex.mag_filter());
		}
		else
		{
			mag_filter = VK_FILTER_NEAREST;
			min_filter.filter = VK_FILTER_NEAREST;
			min_filter.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		}

		if (min_filter.sample_mipmaps && mipmap_count > 1)
		{
			f32 actual_mipmaps;
			if (sampler_state->upload_context == rsx::texture_upload_context::shader_read)
			{
				actual_mipmaps = static_cast<f32>(mipmap_count);
			}
			else if (sampler_state->external_subresource_desc.op != rsx::deferred_request_command::nop)
			{
				actual_mipmaps = sampler_state->external_subresource_desc.exact_mip_count();
			}
			else
			{
				actual_mipmaps = 1.f;
			}

			if (actual_mipmaps > 1.f)
			{
				min_lod = tex.min_lod();
				max_lod = tex.max_lod();
				lod_bias = tex.bias();

				min_lod = std::min(min_lod, actual_mipmaps - 1.f);
				max_lod = std::min(max_lod, actual_mipmaps - 1.f);

				if (min_filter.mipmap_mode == VK_SAMPLER_MIPMAP_MODE_NEAREST)
				{
					// Round to nearest 0.5 to work around some broken games
					// Unlike openGL, sampler parameters cannot be dynamically changed on vulkan, leading to many permutations
					lod_bias = std::floor(lod_bias * 2.f + 0.5f) * 0.5f;
				}
			}
			else
			{
				min_lod = max_lod = lod_bias = 0.f;
				min_filter.mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			}
		}

		if (fs_sampler_handles[i] &&
			fs_sampler_handles[i]->matches(wrap_s, wrap_t, wrap_r, false, lod_bias, af_level, min_lod, max_lod,
				min_filter.filter, mag_filter, min_filter.mipmap_mode, border_color, compare_enabled, depth_compare_mode))
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::sampler_reuse);
			remember_material();
			continue;
		}

		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::sampler_requests);
		fs_sampler_handles[i] = vk::get_resource_manager()->get_sampler(
			*m_device,
			fs_sampler_handles[i],
			wrap_s, wrap_t, wrap_r,
			false,
			lod_bias, af_level, min_lod, max_lod,
			min_filter.filter, mag_filter, min_filter.mipmap_mode,
			border_color, compare_enabled, depth_compare_mode);
		remember_material();
	}

	for (u32 textures_ref = current_vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
	{
		if (!(textures_ref & 1))
		{
			continue;
		}

		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::vs_slots);
		if (!vs_sampler_state[i])
		{
			vs_sampler_state[i] = std::make_unique<vk::texture_cache::sampled_image_descriptor>();
		}

		auto sampler_state = static_cast<vk::texture_cache::sampled_image_descriptor*>(vs_sampler_state[i].get());
		const auto& tex = rsx::method_registers.vertex_textures[i];
		const auto previous_format_class = sampler_state->format_class;

		if (!m_samplers_dirty &&
			!m_vertex_textures_dirty[i] &&
			check_surface_cache_sampler_valid(sampler_state, tex))
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::clean_reuse);
			continue;
		}

		const bool is_sampler_dirty = m_vertex_textures_dirty[i];
		m_vertex_textures_dirty[i] = false;

		if (!rsx::method_registers.vertex_textures[i].enabled())
		{
			*sampler_state = {};
			continue;
		}

		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uploads);
		*sampler_state = m_texture_cache.upload_texture(*m_current_command_buffer, tex, m_rtts);
		if (texture_diagnostics)
		{
			using namespace native_texture_counters;
			const auto context = sampler_state->upload_context;
			texture_diagnostics->add_domain(context == rsx::texture_upload_context::shader_read ? shader_read_uploads :
				context == rsx::texture_upload_context::framebuffer_storage ? framebuffer_uploads :
				context == rsx::texture_upload_context::blit_engine_src ? blit_source_uploads :
				context == rsx::texture_upload_context::blit_engine_dst ? blit_destination_uploads : other_uploads);
			if (m_samplers_dirty) texture_diagnostics->add_domain(global_dirty_uploads);
			if (is_sampler_dirty) texture_diagnostics->add_domain(slot_dirty_uploads);
			const auto format = tex.format() & ~(CELL_GCM_TEXTURE_LN | CELL_GCM_TEXTURE_UN);
			if (format == CELL_GCM_TEXTURE_COMPRESSED_DXT1 || format == CELL_GCM_TEXTURE_COMPRESSED_DXT23 || format == CELL_GCM_TEXTURE_COMPRESSED_DXT45)
				texture_diagnostics->add_domain(compressed_uploads);
			if (!m_samplers_dirty && is_sampler_dirty && context == rsx::texture_upload_context::shader_read)
				texture_diagnostics->add_domain(shader_read_slot_dirty_without_global);
			if (sampler_state->is_cyclic_reference) texture_diagnostics->add_domain(cyclic_uploads);
			if (sampler_state->image_handle) texture_diagnostics->add_domain(direct_image_uploads);
			if (!sampler_state->validate()) texture_diagnostics->add_domain(invalid_domain_uploads);
		}
		if (!sampler_state->validate())
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::invalid_uploads);
			continue;
		}

		if (sampler_state->is_cyclic_reference || sampler_state->external_subresource_desc.do_not_cache)
		{
			check_for_cyclic_refs |= true;
		}

		if (!is_sampler_dirty)
		{
			if (sampler_state->format_class != previous_format_class)
			{
				// Host details changed but RSX is not aware
				m_graphics_state |= rsx::vertex_program_state_dirty;
			}

			if (vs_sampler_handles[i])
			{
				continue;
			}
		}

		const VkBool32 unnormalized_coords = !!(tex.format() & CELL_GCM_TEXTURE_UN);
		const auto min_lod = tex.min_lod();
		const auto max_lod = tex.max_lod();
		const auto wrap_s = vk::vk_wrap_mode(tex.wrap_s());
		const auto wrap_t = vk::vk_wrap_mode(tex.wrap_t());

		// NOTE: In vulkan, the border color can bypass the sample swizzle stage.
		// Check the device properties to determine whether to pre-swizzle the colors or not.
		const auto border_color = is_border_clamped_texture(tex)
			? vk::border_color_t(get_border_color(tex, false))
			: vk::border_color_t(VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK);

		if (vs_sampler_handles[i] &&
			vs_sampler_handles[i]->matches(wrap_s, wrap_t, VK_SAMPLER_ADDRESS_MODE_REPEAT,
				unnormalized_coords, 0.f, 1.f, min_lod, max_lod, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, border_color))
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::sampler_reuse);
			continue;
		}

		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::sampler_requests);
		vs_sampler_handles[i] = vk::get_resource_manager()->get_sampler(
			*m_device,
			vs_sampler_handles[i],
			wrap_s, wrap_t, VK_SAMPLER_ADDRESS_MODE_REPEAT,
			unnormalized_coords,
			0.f, 1.f, min_lod, max_lod,
			VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, border_color);
	}

	m_samplers_dirty.store(false);

	if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE)
	{
		// Transition our FBO to a loop-friendly format.
		// We can also convert it into an input attachment, but for now this is easier.
		auto ds = ensure(m_rtts.m_bound_depth_stencil.second, "Invalid FS export configuration.");
		ds->texture_barrier(*m_current_command_buffer);

		check_for_cyclic_refs = true;
	}

	if (check_for_cyclic_refs)
	{
		// Regenerate renderpass key
		invalidate_render_pass();
	}

	if (backend_config.supports_asynchronous_compute)
	{
		// We have to do this here, because we have to assume the CB will be dumped
		auto async_task_scheduler = g_fxo->try_get<vk::AsyncTaskScheduler>();

		if (async_task_scheduler &&
			async_task_scheduler->is_recording() &&
			!async_task_scheduler->is_host_mode())
		{
			// Sync any async scheduler tasks
			if (auto ev = async_task_scheduler->get_primary_sync_label())
			{
				ev->gpu_wait(*m_current_command_buffer, m_async_compute_dependency_info);
			}
		}
	}
}

bool VKGSRender::bind_texture_env()
{
	auto* const texture_diagnostics = diagnostic_texture_values();
	if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::regular_bind_calls);
	bool out_of_memory = false;

	for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
	{
		if (!(textures_ref & 1))
		{
			// Unused TIU
			continue;
		}

		if (m_fs_binding_table->ftex_location[i] == umax)
		{
			// Corrupt shader table
			break;
		}

		vk::image_view* view = nullptr;
		auto sampler_state = static_cast<vk::texture_cache::sampled_image_descriptor*>(fs_sampler_state[i].get());

		if (rsx::method_registers.fragment_textures[i].enabled() &&
			sampler_state->validate())
		{
			if (view = sampler_state->image_handle; !view)
			{
				//Requires update, copy subresource
				if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::temporary_requests);
				if (!(view = m_texture_cache.create_temporary_subresource(*m_current_command_buffer, sampler_state->external_subresource_desc)))
				{
					out_of_memory = true;
				}
			}
			else
			{
				if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::layout_checks);
				validate_image_layout_for_read_access(*m_current_command_buffer, view, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, sampler_state);
			}
		}

		if (view) [[likely]]
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
			m_program->bind_uniform({ *view, *fs_sampler_handles[i] },
				vk::glsl::binding_set_index_fragment,
				m_fs_binding_table->ftex_location[i]);

			if (current_fragment_program.texture_state.redirected_textures & (1 << i))
			{
				// Stencil mirror required
				auto root_image = static_cast<vk::viewable_image*>(view->image());
				auto stencil_view = root_image->get_view(rsx::default_remap_vector, VK_IMAGE_ASPECT_STENCIL_BIT);

				if (!m_stencil_mirror_sampler)
				{
					m_stencil_mirror_sampler = std::make_unique<vk::sampler>(*m_device,
						VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
						VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
						VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
						VK_FALSE, 0.f, 1.f, 0.f, 0.f,
						VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
						VK_BORDER_COLOR_INT_OPAQUE_BLACK);
				}

				if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
				m_program->bind_uniform({ *stencil_view, *m_stencil_mirror_sampler },
					vk::glsl::binding_set_index_fragment,
					m_fs_binding_table->ftex_stencil_location[i]);
			}
		}
		else
		{
			const VkImageViewType view_type = vk::get_view_type(current_fragment_program.get_texture_dimension(i));
			const VkDescriptorImageInfoEx desc = { *vk::null_image_view(*m_current_command_buffer, view_type), vk::null_sampler() };
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
			m_program->bind_uniform(desc,
				vk::glsl::binding_set_index_fragment,
				m_fs_binding_table->ftex_location[i]);

			if (current_fragment_program.texture_state.redirected_textures & (1 << i))
			{
				if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
				m_program->bind_uniform(desc,
					vk::glsl::binding_set_index_fragment,
					m_fs_binding_table->ftex_stencil_location[i]);
			}
		}
	}

	for (u32 textures_ref = current_vp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
	{
		if (!(textures_ref & 1))
		{
			// Unused TIU
			continue;
		}

		if (m_vs_binding_table->vtex_location[i] == umax)
		{
			// Corrupt shader
			break;
		}

		if (!rsx::method_registers.vertex_textures[i].enabled())
		{
			const auto view_type = vk::get_view_type(current_vertex_program.get_texture_dimension(i));
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
			m_program->bind_uniform({ *vk::null_image_view(*m_current_command_buffer, view_type), vk::null_sampler() },
				vk::glsl::binding_set_index_vertex,
				m_vs_binding_table->vtex_location[i]);

			continue;
		}

		auto sampler_state = static_cast<vk::texture_cache::sampled_image_descriptor*>(vs_sampler_state[i].get());
		auto image_ptr = sampler_state->image_handle;

		if (!image_ptr && sampler_state->validate())
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::temporary_requests);
			if (!(image_ptr = m_texture_cache.create_temporary_subresource(*m_current_command_buffer, sampler_state->external_subresource_desc)))
			{
				out_of_memory = true;
			}
		}

		if (!image_ptr)
		{
			rsx_log.error("Texture upload failed to vtexture index %d. Binding null sampler.", i);
			const auto view_type = vk::get_view_type(current_vertex_program.get_texture_dimension(i));

			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
			m_program->bind_uniform({ *vk::null_image_view(*m_current_command_buffer, view_type), vk::null_sampler() },
				vk::glsl::binding_set_index_vertex,
				m_vs_binding_table->vtex_location[i]);

			continue;
		}

		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::layout_checks);
		validate_image_layout_for_read_access(*m_current_command_buffer, image_ptr, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, sampler_state);

		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
		m_program->bind_uniform({ *image_ptr, *vs_sampler_handles[i] },
			vk::glsl::binding_set_index_vertex,
			m_vs_binding_table->vtex_location[i]);
	}

	if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE)
	{
		auto ds = ensure(m_rtts.m_bound_depth_stencil.second);
		auto view = ds->get_view(rsx::default_remap_vector, VK_IMAGE_ASPECT_DEPTH_BIT);
		if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
		m_program->bind_uniform({ *view, vk::null_sampler() }, vk::glsl::binding_set_index_fragment, m_fs_binding_table->frag_depth_input_location);
	}

	if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING)
	{
		ensure(current_fragment_program.mrt_buffers_count == m_draw_buffers.size());
		const auto remap = rsx::default_remap_vector.with_encoding(vk::VK_REMAP_IDENTITY);

		for (u32 i = 0; i < current_fragment_program.mrt_buffers_count; ++i)
		{
			auto viewable = static_cast<vk::viewable_image*>(m_fbo_images[i]);
			const auto view = viewable->get_view(remap);
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::uniform_binds);
			m_program->bind_uniform(*view, vk::glsl::binding_set_index_fragment, m_fs_binding_table->frag_src_location[i]);
		}
	}

	return out_of_memory;
}

bool VKGSRender::bind_interpreter_texture_env()
{
	auto* const texture_diagnostics = diagnostic_texture_values();
	if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::interpreter_calls);
	if (current_fp_metadata.referenced_textures_mask == 0)
	{
		// Nothing to do
		return false;
	}

	std::array<VkDescriptorImageInfoEx, 68> texture_env;
	VkDescriptorImageInfoEx fallback =
	{
		*vk::null_image_view(*m_current_command_buffer, VK_IMAGE_VIEW_TYPE_1D),
		vk::null_sampler()
	};

	auto start = texture_env.begin();
	auto end = start;

	// Fill default values
	// 1D
	std::advance(end, 16);
	std::fill(start, end, fallback);
	// 2D
	start = end;
	fallback.imageView = vk::null_image_view(*m_current_command_buffer, VK_IMAGE_VIEW_TYPE_2D)->value;
	std::advance(end, 16);
	std::fill(start, end, fallback);
	// 3D
	start = end;
	fallback.imageView = vk::null_image_view(*m_current_command_buffer, VK_IMAGE_VIEW_TYPE_3D)->value;
	std::advance(end, 16);
	std::fill(start, end, fallback);
	// CUBE
	start = end;
	fallback.imageView = vk::null_image_view(*m_current_command_buffer, VK_IMAGE_VIEW_TYPE_CUBE)->value;
	std::advance(end, 16);
	std::fill(start, end, fallback);

	bool out_of_memory = false;

	auto decay_view_for_interpreter = [&](
		const rsx::image_section_attributes_t& attr,
		vk::texture_cache::sampled_image_descriptor* desc,
		vk::image_view* base,
		const rsx::texture_channel_remap_t& decoded_remap,
		bool is_msaa,
		bool is_redirected) -> vk::image_view*
	{
		if (!is_msaa && !is_redirected)
		{
			return base;
		}

		if (is_redirected && desc->image_type > rsx::texture_dimension_extended::texture_dimension_2d)
		{
			// Cannot handle redirect on 3D or cubemap with the interpreter.
			auto view_type = vk::get_view_type(desc->image_type);
			return vk::null_image_view(*m_current_command_buffer, view_type);
		}

		using deferred_subresource_t = vk::texture_cache::deferred_subresource;
		auto image = static_cast<vk::viewable_image*>(base->image());
		auto rtt = vk::try_as_rtt(base->image());

		if (is_msaa)
		{
			// MSAA resolve
			ensure(rtt);
			rtt->memory_barrier(*m_current_command_buffer, rsx::surface_access::transfer_read);
			image = rtt->get_surface(rsx::surface_access::transfer_read);
		}

		if (is_redirected)
		{
			// Force bitcast
			rsx::image_section_attributes_t flatten_attrs{};
			flatten_attrs.address = desc->ref_address;
			flatten_attrs.gcm_format = desc->format_ex.format();
			flatten_attrs.width = image->width();
			flatten_attrs.height = image->height();
			flatten_attrs.depth = 1;

			const coord3u flatten_rect = { 0, 0, 0, flatten_attrs.width, flatten_attrs.height, 1 };
			auto flatten_op = deferred_subresource_t::create_copy(
				image, flatten_attrs, flatten_rect, rsx::surface_transform::identity, decoded_remap, desc->is_cyclic_reference);

			ensure(desc->ref_address);
			flatten_op.cache_range = rtt
				? rtt->get_memory_range()
				: utils::address_range32::start_length(desc->ref_address, attr.pitch * attr.height);

			return m_texture_cache.create_temporary_subresource(*m_current_command_buffer, flatten_op);
		}

		image->change_layout(*m_current_command_buffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		return image->get_view(decoded_remap, base->info.subresourceRange.aspectMask);
	};

	for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
	{
		if (!(textures_ref & 1))
			continue;

		vk::image_view* view = nullptr;
		auto sampler_state = static_cast<vk::texture_cache::sampled_image_descriptor*>(fs_sampler_state[i].get());

		auto& tex = rsx::method_registers.fragment_textures[i];
		if (tex.enabled() && sampler_state->validate())
		{
			if (view = sampler_state->image_handle; !view)
			{
				//Requires update, copy subresource
				if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::temporary_requests);
				if (!(view = m_texture_cache.create_temporary_subresource(*m_current_command_buffer, sampler_state->external_subresource_desc)))
				{
					out_of_memory = true;
				}
			}
		}

		if (!view)
		{
			// OOM or disabled texture
			continue;
		}

		auto primary_view = view;

		// Flatten MSAA and DEPTH24S8 redirects
		if (view->image()->samples() > 1 || view->info.subresourceRange.aspectMask != VK_IMAGE_ASPECT_COLOR_BIT)
		{
			const auto mask = (1u << i);
			const bool is_redirected = !!(current_fragment_program.texture_state.redirected_textures & mask);
			const bool is_msaa = !!(current_fragment_program.texture_state.multisampled_textures & mask);
			if (is_redirected || is_msaa)
			{
				view = decay_view_for_interpreter(
					tex.attributes(),
					sampler_state,
					view,
					tex.decoded_remap(),
					is_msaa,
					is_redirected);

				if (!view)
				{
					// OOM
					out_of_memory = true;
					continue;
				}
			}
		}

		if (primary_view == view)
		{
			if (texture_diagnostics) texture_diagnostics->add(native_texture_counters::layout_checks);
			validate_image_layout_for_read_access(*m_current_command_buffer, view, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, sampler_state);
		}

		const int offsets[] = { 0, 16, 48, 32 };
		auto& sampled_image_info = texture_env[offsets[static_cast<u32>(sampler_state->image_type)] + i];
		sampled_image_info = { *view, *fs_sampler_handles[i] };
	}

	m_shader_interpreter.update_fragment_textures(texture_env);
	return out_of_memory;
}

void VKGSRender::emit_geometry(u32 sub_index)
{
	auto &draw_call = rsx::method_registers.current_draw_clause;
	m_profiler.start();

	const rsx::flags32_t vertex_state_mask = rsx::vertex_base_changed | rsx::vertex_arrays_changed;
	const rsx::flags32_t state_flags = (sub_index == 0) ? rsx::vertex_arrays_changed : draw_call.execute_pipeline_dependencies(m_ctx);

	if (state_flags & rsx::vertex_arrays_changed)
	{
		m_draw_processor.analyse_inputs_interleaved(m_vertex_layout, current_vp_metadata);
	}
	else if (state_flags & rsx::vertex_base_changed)
	{
		// Rebase vertex bases instead of
		for (auto& info : m_vertex_layout.interleaved_blocks)
		{
			info->vertex_range.second = 0;
			const auto vertex_base_offset = rsx::method_registers.vertex_data_base_offset();
			info->real_offset_address = rsx::get_address(rsx::get_vertex_offset_from_base(vertex_base_offset, info->base_offset), info->memory_location);
		}
	}
	else
	{
		// Discard cached results
		for (auto& info : m_vertex_layout.interleaved_blocks)
		{
			info->vertex_range.second = 0;
		}
	}

	if ((state_flags & vertex_state_mask) && !m_vertex_layout.validate())
	{
		// No vertex inputs enabled
		// Execute remainining pipeline barriers with NOP draw
		do
		{
			draw_call.execute_pipeline_dependencies(m_ctx);
		}
		while (draw_call.next());

		draw_call.end();
		return;
	}

	// Programs data is dependent on vertex state
	auto upload_info = upload_vertex_data();
	if (!upload_info.vertex_draw_count)
	{
		// Malformed vertex setup; abort
		return;
	}

	m_frame_stats.vertex_upload_time += m_profiler.duration();
	vk::pass_timing::mark(2);

	// Faults are allowed during vertex upload. Ensure consistent CB state after uploads.
	// Queries are spawned and closed outside render pass scope for consistency reasons.
	if (m_current_command_buffer->flags & vk::command_buffer::cb_load_occluson_task)
	{
		u32 occlusion_id = m_occlusion_query_manager->allocate_query(*m_current_command_buffer);
		if (occlusion_id == umax)
		{
			// Force flush
			rsx_log.warning("[Performance Warning] Out of free occlusion slots. Forcing hard sync.");
			ZCULL_control::sync(this);

			occlusion_id = m_occlusion_query_manager->allocate_query(*m_current_command_buffer);
			if (occlusion_id == umax)
			{
				//rsx_log.error("Occlusion pool overflow");
				if (m_current_task) m_current_task->result = 1;
			}
		}

		// Before starting a query, we need to match RP scope (VK_1_0 rules).
		// We always want our queries to start outside a renderpass whenever possible.
		// We ignore this for performance reasons whenever possible of course and only do this for sensitive drivers.
		if (vk::use_strict_query_scopes() &&
			vk::is_renderpass_open(*m_current_command_buffer))
		{
			vk::end_renderpass(*m_current_command_buffer);
			emergency_query_cleanup(m_current_command_buffer);
		}

		// Begin query
		m_occlusion_query_manager->begin_query(*m_current_command_buffer, occlusion_id);

		auto& data = m_occlusion_map[m_active_query_info->driver_handle];
		data.indices.push_back(occlusion_id);
		data.set_sync_command_buffer(m_current_command_buffer);

		m_current_command_buffer->flags &= ~vk::command_buffer::cb_load_occluson_task;
		m_current_command_buffer->flags |= (vk::command_buffer::cb_has_occlusion_task | vk::command_buffer::cb_has_open_query);
	}

	VkDescriptorBufferViewEx persistent_buffer = upload_info.static_vertices ? *m_geometry_cache.vertex_heap.view :
		m_persistent_attribute_storage ? *m_persistent_attribute_storage : *null_buffer_view;
	VkDescriptorBufferViewEx volatile_buffer = m_volatile_attribute_storage ? *m_volatile_attribute_storage : *null_buffer_view;
	bool update_descriptors = false;

	if (m_current_draw.subdraw_id == 0 || m_static_vertices_bound != upload_info.static_vertices)
	{
		update_descriptors = true;
		m_static_vertices_bound = upload_info.static_vertices;
	}

	if (m_current_draw.subdraw_id == 0)
	{

		// Allocate stream layout memory for this batch
		const u64 alloc_size = rsx::method_registers.current_draw_clause.pass_count() * 168;
		m_vertex_layout_dynamic_offset = m_vertex_layout_ring_info.alloc<8>(alloc_size);
	}

	// Update vertex fetch parameters
	update_vertex_env(sub_index, upload_info);

	if (update_descriptors)
	{
		m_program->bind_uniform(persistent_buffer, vk::glsl::binding_set_index_vertex, m_vs_binding_table->vertex_buffers_location);
		m_program->bind_uniform(volatile_buffer, vk::glsl::binding_set_index_vertex, m_vs_binding_table->vertex_buffers_location + 1);
	}

	bool reload_state = (!m_current_draw.subdraw_id++);
	vk::renderpass_op(*m_current_command_buffer, [&](const vk::command_buffer& cmd, VkRenderPass pass, VkFramebuffer fbo)
	{
		if (get_render_pass() == pass && m_draw_fbo->value == fbo)
		{
			// Nothing to do
			return;
		}

		if (pass)
		{
			// Subpass mismatch, end it before proceeding
			vk::end_renderpass(cmd);
		}

		// Starting a new renderpass should clobber dynamic state
		m_current_command_buffer->flags |= vk::command_buffer::cb_reload_dynamic_state;

		reload_state = true;
	});

	if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING)
	{
		// Subpass inter-draw dependency for input attachment reads. Preserves open renderpasses.
		for (u32 i = 0; i < current_fragment_program.mrt_buffers_count; ++i)
		{
			vk::insert_image_memory_barrier(
				*m_current_command_buffer,
				m_fbo_images[i]->value,
				m_fbo_images[i]->current_layout,
				m_fbo_images[i]->current_layout,
				VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
				VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
				VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
				true,
				VK_DEPENDENCY_BY_REGION_BIT
			);
		}
	}

	// Bind both pipe and descriptors in one go
	// FIXME: We only need to rebind the pipeline when reload state is set. Flags?
	m_program->bind(*m_current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);

	if (reload_state)
	{
		update_draw_state();
		begin_render_pass();

		if (cond_render_ctrl.hw_cond_active && m_device->get_conditional_render_support())
		{
			// It is inconvenient that conditional rendering breaks other things like compute dispatch
			// TODO: If this is heavy, add refactor the resources into global and add checks around compute dispatch
			VkConditionalRenderingBeginInfoEXT info{};
			info.sType = VK_STRUCTURE_TYPE_CONDITIONAL_RENDERING_BEGIN_INFO_EXT;
			info.buffer = m_cond_render_buffer->value;

			_vkCmdBeginConditionalRenderingEXT(*m_current_command_buffer, &info);
			m_current_command_buffer->flags |= vk::command_buffer::cb_has_conditional_render;
		}
	}

	// Bind the new set of descriptors for use with this draw call
	m_frame_stats.setup_time += m_profiler.duration();
	vk::pass_timing::mark(3);

	u32 diagnostic_draw_token = ~0u;
	if (m_draw_attribution_enabled)
		diagnostic_draw_token = diagnostic_draw_begin(upload_info.vertex_draw_count, sub_index,
			upload_info.index_info.has_value(), static_cast<u32>(draw_call.primitive), draw_call.pass_count());

	if (!upload_info.index_info)
	{
		if (draw_call.is_trivial_instanced_draw)
		{
			vkCmdDraw(*m_current_command_buffer, upload_info.vertex_draw_count, draw_call.pass_count(), 0, 0);
		}
		else if (draw_call.is_single_draw())
		{
			vkCmdDraw(*m_current_command_buffer, upload_info.vertex_draw_count, 1, 0, 0);
		}
		else if (m_device->get_multidraw_support())
		{
			const auto& subranges = draw_call.get_subranges();
			auto ptr = utils::bless<const VkMultiDrawInfoEXT>(&subranges.front().first);
			_vkCmdDrawMultiEXT(*m_current_command_buffer, ::size32(subranges), ptr, 1, 0, sizeof(rsx::draw_range_t));
		}
		else
		{
			u32 vertex_offset = 0;
			const auto& subranges = draw_call.get_subranges();
			for (const auto& range : subranges)
			{
				vkCmdDraw(*m_current_command_buffer, range.count, 1, vertex_offset, 0);
				vertex_offset += range.count;
			}
		}
	}
	else
	{
		const VkIndexType index_type = std::get<1>(*upload_info.index_info);
		const VkDeviceSize offset = std::get<0>(*upload_info.index_info);

		vkCmdBindIndexBuffer(*m_current_command_buffer,
			upload_info.static_indices ? m_geometry_cache.index_heap.buffer->value : m_index_buffer_ring_info.heap->value, offset, index_type);

		if (draw_call.is_trivial_instanced_draw)
		{
			vkCmdDrawIndexed(*m_current_command_buffer, upload_info.vertex_draw_count, draw_call.pass_count(), 0, 0, 0);
		}
		else if (rsx::method_registers.current_draw_clause.is_single_draw())
		{
			vkCmdDrawIndexed(*m_current_command_buffer, upload_info.vertex_draw_count, 1, 0, 0, 0);
		}
		else if (m_device->get_multidraw_support())
		{
			const auto& subranges = draw_call.get_subranges();
			const auto subranges_count = ::size32(subranges);
			const auto allocation_size = subranges_count * sizeof(VkMultiDrawIndexedInfoEXT);

			m_multidraw_parameters_buffer.resize(allocation_size);
			auto base_ptr = utils::bless<VkMultiDrawIndexedInfoEXT>(m_multidraw_parameters_buffer.data());

			u32 vertex_offset = 0;
			auto _ptr = base_ptr;

			for (const auto& range : subranges)
			{
				const auto count = get_index_count(draw_call.primitive, range.count);
				_ptr->firstIndex = vertex_offset;
				_ptr->indexCount = count;
				_ptr->vertexOffset = 0;

				_ptr++;
				vertex_offset += count;
			}
			_vkCmdDrawMultiIndexedEXT(*m_current_command_buffer, subranges_count, base_ptr, 1, 0, sizeof(VkMultiDrawIndexedInfoEXT), nullptr);
		}
		else
		{
			u32 vertex_offset = 0;
			const auto& subranges = draw_call.get_subranges();
			for (const auto& range : subranges)
			{
				const auto count = get_index_count(draw_call.primitive, range.count);
				vkCmdDrawIndexed(*m_current_command_buffer, count, 1, vertex_offset, 0, 0);
				vertex_offset += count;
			}
		}
	}

	if (m_draw_attribution_enabled)
		diagnostic_draw_end(diagnostic_draw_token);

	m_frame_stats.draw_exec_time += m_profiler.duration();
}

void VKGSRender::begin()
{
	if (vk::live_ctl::get(9) == 3) [[unlikely]]
	{
		fast_draw_verify_begin();
	}

	// Save shader state now before prefetch and loading happens
	m_interpreter_state = (m_graphics_state.load() & rsx::pipeline_state::invalidate_pipeline_bits);

	rsx::thread::begin();

	if (skip_current_frame ||
		swapchain_unavailable ||
		cond_render_ctrl.disable_rendering())
	{
		return;
	}

	init_buffers(rsx::framebuffer_creation_context::context_draw);

	if (m_graphics_state & rsx::pipeline_state::invalidate_pipeline_bits)
	{
		// Shaders need to be reloaded.
		m_prev_program = m_program;
		m_program = nullptr;
	}
}

void VKGSRender::end()
{
	if (skip_current_frame || !m_graphics_state.test(rsx::rtt_config_valid) || swapchain_unavailable || cond_render_ctrl.disable_rendering())
	{
		execute_nop_draw();
		rsx::thread::end();
		return;
	}

	// Experiment only (live control 9 == 1): drop depth-only draws to bound what the shadow pass costs after FIFO parsing
	if (vk::live_ctl::get(9) == 1 && !m_surface_info[0].address && m_depth_surface_info.address) [[unlikely]]
	{
		execute_nop_draw();
		rsx::thread::end();
		return;
	}

	m_profiler.start();

	if (vk::pass_timing::enabled())
	{
		vk::pass_timing::draw(m_surface_info[0].address, m_depth_surface_info.address);
	}

	if (vk::gpu_pass_profile::enabled())
	{
		vk::gpu_pass_profile::mark(*m_current_command_buffer, (u64{ m_surface_info[0].address } << 32) | m_depth_surface_info.address, m_framebuffer_layout.width, m_framebuffer_layout.height);
	}

	// Check for frame resource status here because it is possible for an async flip to happen between begin/end
	if (m_current_frame->flags & frame_context_state::dirty) [[unlikely]]
	{
		check_present_status();

		if (m_current_frame->swap_command_buffer) [[unlikely]]
		{
			// Borrow time by using the auxilliary context
			m_aux_frame_context.grab_resources(*m_current_frame);
			m_current_frame = &m_aux_frame_context;
		}

		ensure(!m_current_frame->swap_command_buffer);

		m_current_frame->flags &= ~frame_context_state::dirty;
	}

	analyse_current_rsx_pipeline();

	m_frame_stats.setup_time += m_profiler.duration();

	load_texture_env();
 const auto texture_load_us = m_profiler.duration();
 m_frame_stats.textures_upload_time += texture_load_us;
 if (auto* texture = diagnostic_texture_values()) texture->elapsed(false, texture_load_us);

	if (!load_program())
	{
		// Program is not ready, skip drawing this
		std::this_thread::yield();
		execute_nop_draw();
		// m_rtts.on_write(); - breaks games for obvious reasons
		rsx::thread::end();
		return;
	}

	// Load program execution environment
	load_program_env();
	m_frame_stats.setup_time += m_profiler.duration();
	vk::pass_timing::mark(1);

	// Apply write memory barriers
	if (auto ds = std::get<1>(m_rtts.m_bound_depth_stencil))
	{
		// The inFamous games fill the depth buffer of their 512x288 particle target with one full-screen triangle that
		// writes every pixel (depth test ALWAYS). What the barrier would bring into the buffer before that, last frame's
		// contents and the shadow map that shares the address, is overwritten at once, and bringing it in costs several
		// times the draw itself (most with full resolution particles). Start from a cleared buffer instead.
		if (!m_surface_info[0].address && m_framebuffer_layout.width == 512 && m_framebuffer_layout.height == 288 &&
			!ds->old_contents.empty() && vk::live_ctl::get(14) != 1 && rsx::is_infamous_title()) [[unlikely]]
		{
			auto& clause = rsx::method_registers.current_draw_clause;
			if (clause.command == rsx::draw_command::array && !clause.empty() && clause.pass_count() == 1 &&
				(clause.begin(), clause.get_elements_count() == 3) &&
				rsx::method_registers.depth_test_enabled() && rsx::method_registers.depth_write_enabled() &&
				rsx::method_registers.depth_func() == rsx::comparison_function::always)
			{
				if (static bool logged = false; !logged)
				{
					logged = true;
					rsx_log.notice("inFamous particle depth buffer at 0x%x starts cleared, %u pending transfers dropped", m_depth_surface_info.address, ds->old_contents.size());
				}

				ds->clear_rw_barrier();
				ds->state_flags |= rsx::surface_state_flags::erase_bkgnd;
			}
		}

		ds->write_barrier(*m_current_command_buffer);

		if (m_graphics_state.test(rsx::zeta_address_cyclic_barrier) &&
			ds->current_layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
		{
			// We actually need to end the subpass as a minimum. Without this, early-Z optimiazations in following draws will clobber reads from previous draws and cause flickering.
			// Since we're ending the subpass, might as well restore DCC/HiZ for extra performance
			ds->change_layout(*m_current_command_buffer, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
			ds->reset_surface_counters();

			// Regenerate render pass key
			invalidate_render_pass();
		}
	}

	for (auto &rtt : m_rtts.m_bound_render_targets)
	{
		if (auto surface = std::get<1>(rtt))
		{
			surface->write_barrier(*m_current_command_buffer);
		}
	}

	m_graphics_state.clear(rsx::zeta_address_cyclic_barrier);

	m_frame_stats.setup_time += m_profiler.duration();

	// Now bind the shader resources. It is important that this takes place after the barriers so that we don't end up with stale descriptors
	for (int retry = 0; retry < 3; ++retry)
	{
		if (retry > 0) { if (auto* texture = diagnostic_texture_values()) texture->add(native_texture_counters::oom_retries); }
		if (retry > 0 && m_samplers_dirty) [[ unlikely ]]
		{
			// Reload texture env if referenced objects were invalidated during OOM handling.
			load_texture_env();

			// Do not trust fragment/vertex texture state after a texture state reset.
			// NOTE: We don't want to change the program - it's too late for that now. We just need to harmonize the state.
			m_graphics_state |= rsx::vertex_program_state_dirty | rsx::fragment_program_state_dirty;
			get_current_fragment_program(fs_sampler_state);
			get_current_vertex_program(vs_sampler_state);
			m_graphics_state.clear(rsx::pipeline_state::invalidate_pipeline_bits);
		}

		const bool out_of_memory = m_shader_interpreter.is_interpreter(m_program)
			? bind_interpreter_texture_env()
			: bind_texture_env();

		if (!out_of_memory)
		{
			break;
		}

		// Handle OOM
		if (!on_vram_exhausted(rsx::problem_severity::fatal))
		{
			// It is not possible to free memory. Just use placeholder textures. Can cause graphics glitches but shouldn't crash otherwise
			break;
		}
	}

	m_texture_cache.release_uncached_temporary_subresources();
 const auto texture_bind_us = m_profiler.duration();
 m_frame_stats.textures_upload_time += texture_bind_us;
 if (auto* texture = diagnostic_texture_values())
 {
  texture->elapsed(true, texture_bind_us);
  texture->add(native_texture_counters::bind_blocks);
 }

	u32 sub_index = 0;               // RSX subdraw ID
	m_current_draw.subdraw_id = 0;   // Host subdraw ID. Invalid RSX subdraws do not increment this value

	if (m_graphics_state & rsx::pipeline_state::invalidate_vk_dynamic_state)
	{
		m_current_command_buffer->flags |= vk::command_buffer::cb_reload_dynamic_state;
	}

	auto& draw_call = rsx::method_registers.current_draw_clause;
	draw_call.begin();
	do
	{
		emit_geometry(sub_index++);

		if (draw_call.is_trivial_instanced_draw)
		{
			// We already completed. End the draw.
			draw_call.end();
		}
	}
	while (draw_call.next());

	if (m_current_command_buffer->flags & vk::command_buffer::cb_has_conditional_render)
	{
		_vkCmdEndConditionalRenderingEXT(*m_current_command_buffer);
		m_current_command_buffer->flags &= ~(vk::command_buffer::cb_has_conditional_render);
	}

	m_rtts.on_write(m_framebuffer_layout.color_write_enabled, m_framebuffer_layout.zeta_write_enabled);

	rsx::thread::end();
	vk::pass_timing::mark(4);

	// Before the periodic submit, which legitimately starts another command buffer
	if (vk::live_ctl::get(9) == 3) [[unlikely]]
	{
		fast_draw_verify_end();
	}

	if (m_draw_prefix_submit_enabled)
		maybe_submit_draw_prefix();

	if (m_periodic_submit_us)
		maybe_periodic_submit();

	// Following draws that only repeat this one with other geometry and constants are taken from the FIFO directly
	if (fast_draw_enabled() && !m_fast_draw.in_batch) [[likely]]
	{
		u32 blocker = fast_draw_run_blocker();
		if (!blocker) blocker = fast_draw_blocker();
		if (!blocker && !vk::is_renderpass_open(*m_current_command_buffer)) blocker = 8;

		if (blocker)
		{
			m_fast_draw.not_armed[blocker]++;
		}
		else
		{
			fast_draw_batch();
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Repeat draws consumed straight from the FIFO (live control 9 == 2).
//
// Long runs of draws (shadow-map passes most of all) repeat one short command sequence: new transform constants,
// a vertex array offset, an index buffer address, BEGIN, DRAW_INDEX_ARRAY, END. Everything else stays as the
// previous draw left it. After a draw has gone through the complete path, the commands that follow are read here.
// The setup commands are applied exactly as their handlers would apply them outside a BEGIN/END pair. A draw is
// then emitted with the program, pipeline, descriptors, render pass and dynamic state that are already bound,
// reusing the regular vertex upload and layout code. Any other command, or any state that is not provably
// unchanged, ends the run with the FIFO positioned on the first command that was not consumed.
//
// Two kinds of state change are carried through a run as well, because they are what separates most draws of the
// G-buffer and shadow passes from their predecessor:
// - fragment texture setup. The commands go through their regular handlers; before the draw the regular texture
//   lookup, fragment program state derivation and texture binding run, and the run only continues if the program
//   properties that select the shader came out unchanged (fast_draw_rebind_textures).
// - polygon offset, which is dynamic state here: the depth bias is set again before the draw.
// ---------------------------------------------------------------------------------------------------------------

// Checked once when a run starts: things the consumed commands cannot change. 0 = nothing in the way.
u32 VKGSRender::fast_draw_run_blocker() const
{
	if (m_flattener.is_enabled() || capture_current_frame || pause_on_draw) return 2;
	if (fifo_ctrl->get_remaining_args_count()) return 3;
	if (!m_program || m_shader_interpreter.is_interpreter(m_program)) return 4;
	if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_PROGRAMMABLE_BLENDING) return 10;
	if (current_vertex_program.ctrl & RSX_SHADER_CONTROL_INSTANCED_CONSTANTS) return 11;

	// The complete path puts a barrier on the depth buffer for every draw of such a program
	if (current_fragment_program.ctrl & RSX_SHADER_CONTROL_EMULATE_DEPTH_COMPARE) return 15;

	// The complete path revalidates every sampled texture for every draw. That is skipped only for plain textures:
	// anything backed by a render target can expire or need a barrier between two draws.
	if (current_vp_metadata.referenced_textures_mask) return 13;

	return fast_draw_textures_plain() ? 0 : 14;
}

// Every texture the fragment program samples is an ordinary uploaded texture with a view of its own
bool VKGSRender::fast_draw_textures_plain() const
{
	for (u32 textures_ref = current_fp_metadata.referenced_textures_mask, i = 0; textures_ref; textures_ref >>= 1, ++i)
	{
		if (!(textures_ref & 1)) continue;

		const auto* sampler = static_cast<const vk::texture_cache::sampled_image_descriptor*>(fs_sampler_state[i].get());
		if (m_textures_dirty[i] || !sampler || sampler->upload_context != rsx::texture_upload_context::shader_read || sampler->is_cyclic_reference)
		{
			return false;
		}

		// A texture that is assembled from several sections for each draw
		if (rsx::method_registers.fragment_textures[i].enabled() && sampler->validate() && (!sampler->image_handle || !fs_sampler_handles[i]))
		{
			return false;
		}
	}

	return true;
}

// A draw of a run whose fragment textures were set up again. Runs the regular texture stages of the complete path
// and reports whether the bound program is still the right one; if not, the draw has to take the complete path.
bool VKGSRender::fast_draw_rebind_textures()
{
	const auto* const command_buffer = m_current_command_buffer;
	const auto bound = [&]()
	{
		return m_current_command_buffer == command_buffer && vk::is_renderpass_open(*m_current_command_buffer) && !m_samplers_dirty;
	};

	// Lookup (or upload) of the textures that changed, and their samplers
	load_texture_env();

	if (!bound() || !fast_draw_textures_plain())
	{
		return false;
	}

	// Everything that selects the fragment shader besides the program itself (fragment_program_compare)
	const auto ctrl = current_fragment_program.ctrl;
	const auto texture_state = current_fragment_program.texture_state;
	const auto texcoord_control_mask = current_fragment_program.texcoord_control_mask;
	const auto two_sided_lighting = current_fragment_program.two_sided_lighting;
	const auto mrt_buffers_count = current_fragment_program.mrt_buffers_count;

	get_current_fragment_program(fs_sampler_state);

	if (ctrl != current_fragment_program.ctrl || !(texture_state == current_fragment_program.texture_state) ||
		texcoord_control_mask != current_fragment_program.texcoord_control_mask || two_sided_lighting != current_fragment_program.two_sided_lighting ||
		mrt_buffers_count != current_fragment_program.mrt_buffers_count)
	{
		// Another shader variant: as before the draw, with the textures already loaded
		m_graphics_state |= rsx::pipeline_state::fragment_program_state_dirty;
		m_fast_draw.texture_program_changes++;
		return false;
	}

	if (vk::live_ctl::get(9) == 4) [[unlikely]]
	{
		// Check: the complete path's program lookup has to return the program that is bound
		const auto* const expected = m_program;
		m_graphics_state |= rsx::pipeline_state::fragment_program_state_dirty;
		m_prev_program = m_program;
		m_program = nullptr;
		const bool loaded = load_program();
		m_fast_draw.texture_checks++;

		if (!loaded || m_program != expected)
		{
			if (m_fast_draw.texture_check_mismatches++ < 8)
			{
				rsx_log.error("Fast draw check: a texture change that left the program properties equal selected another program");
			}

			m_graphics_state |= rsx::pipeline_state::fragment_program_state_dirty;
			return false;
		}
	}

	// Texture parameters (scale, bias, remap, control bits) of this draw
	update_fragment_texture_params_buffer();
	m_program->bind_uniform(m_fragment_texture_params_buffer_info, vk::glsl::binding_set_index_fragment, m_fs_binding_table->tex_param_location);
	m_graphics_state.clear(rsx::pipeline_state::fragment_texture_state_dirty);

	if (bind_texture_env() || !bound())
	{
		return false;
	}

	m_fast_draw.texture_rebinds++;
	return true;
}

// Checked for every draw: state that other code, or the upload of the previous draw, may have changed
u32 VKGSRender::fast_draw_blocker(u32 handled_state) const
{
	if (!fast_draw_enabled()) return 1;
	if (m_samplers_dirty) return 12;
	if (cond_render_ctrl.hw_cond_active || cond_render_ctrl.eval_pending() || cond_render_ctrl.disable_rendering()) return 5;
	if (skip_current_frame || swapchain_unavailable || (m_current_frame->flags & frame_context_state::dirty)) return 6;

	// No pending state change of any kind. Transform constants are handled by the fast path itself.
	// Also allowed, because a draw does not act on them:
	// - status bits (rtt_config_valid/contested, scissor_setup_clipped, zeta_address_is_cyclic; the cyclic *barrier* is not)
	// - bits nothing consumes (vertex_texture_state_dirty, line_stipple_pattern_dirty, the undefined top bits)
	// - pipeline_config_dirty: the complete path leaves it set when re-evaluation finds the same pipeline, and none of
	//   the registers it covers is written by the commands consumed here
	// - polygon_stipple_pattern_dirty while stippling is off
	u32 allowed = rsx::pipeline_state::framebuffer_reads_dirty | rsx::pipeline_state::transform_constants_dirty |
		rsx::pipeline_state::rtt_config_valid | rsx::pipeline_state::rtt_config_contested | rsx::pipeline_state::scissor_setup_clipped |
		rsx::pipeline_state::zeta_address_is_cyclic | rsx::pipeline_state::vertex_texture_state_dirty |
		rsx::pipeline_state::line_stipple_pattern_dirty | rsx::pipeline_state::pipeline_config_dirty | 0xe0000000u | handled_state;

	if (!rsx::method_registers.polygon_stipple_enabled())
	{
		allowed |= rsx::pipeline_state::polygon_stipple_pattern_dirty;
	}

	const u32 state = m_graphics_state.load();
	if ((state & ~allowed) || !(state & rsx::pipeline_state::rtt_config_valid))
	{
		m_fast_draw.blocking_state_bits |= (state & ~allowed) | (~state & rsx::pipeline_state::rtt_config_valid);
		return 7;
	}

	if (m_current_command_buffer->flags & (vk::command_buffer::cb_reload_dynamic_state | vk::command_buffer::cb_load_occluson_task | vk::command_buffer::cb_has_conditional_render)) return 9;
	return 0;
}

void VKGSRender::fast_draw_batch()
{
	enum stop_reason : u32 { limit_reached, fifo_end, flow_control, other_method, unusual_packet, blocked, other_primitive, unusual_draw, complete_path };
	static_assert(NV4097_SET_TRANSFORM_CONSTANT == NV4097_SET_TRANSFORM_CONSTANT_LOAD + 1);

	m_fast_draw.in_batch = true;
	m_fast_draw.batches++;

	auto& fifo = *fifo_ctrl;
	auto& regs = rsx::method_registers;
	auto& clause = regs.current_draw_clause;

	u32 stop = limit_reached;
	u32 pos = fifo.get_pos() + 4;
	u32 words[40];
	u32 draw_values[32];

	// Complete packets only: every word comes through the FIFO's own fetch, which ends at PUT
	const auto fetch = [&](u32 at, u32 count) -> bool
	{
		for (u32 i = 0; i < count; i++)
		{
			if (!fifo.peek(at + i * 4, words[i])) return false;
		}
		return true;
	};

	// Buffers behind the descriptors that stay bound. A ring buffer that was replaced needs the complete path.
	struct bound_buffers_t
	{
		VkBuffer buffers[7];
		bool operator==(const bound_buffers_t&) const = default;
	};

	const auto bound_buffers = [&]() -> bound_buffers_t
	{
		return {{ m_vertex_env_buffer_info.buffer, m_vertex_layout_stream_info.buffer, m_fragment_env_buffer_info.buffer,
			m_fragment_texture_params_buffer_info.buffer, m_raster_env_buffer_info.buffer, m_fragment_constants_buffer_info.buffer,
			m_vertex_layout_ring_info.heap->value }};
	};

	const auto initial_buffers = bound_buffers();
	const auto* initial_cb = m_current_command_buffer;

	// Setup commands that are passed to their regular handlers: fragment texture setup and polygon offset.
	// What they leave pending is dealt with before the draw (texture_state, depth_bias_state).
	constexpr u32 texture_state = rsx::pipeline_state::fragment_program_state_dirty;
	constexpr u32 depth_bias_state = rsx::pipeline_state::polygon_offset_state_dirty;

	const bool delegate = vk::live_ctl::get(9) != 5;

	// Vertex formats and the texture read semaphore (live control 9 == 6 leaves them out)
	const bool extended = delegate && vk::live_ctl::get(9) != 6;

	// The label write of a texture read semaphore only touches guest memory in this configuration; with strict
	// rendering or host labels it can flush the command queue
	const bool semaphores = extended && !g_cfg.video.strict_rendering_mode && !backend_config.supports_host_gpu_labels;

	const auto delegated = [&](u32 reg, u32 count) -> bool
	{
		if (!delegate) return false;
		const auto within = [&](u32 first, u32 length) { return reg >= first && reg + count <= first + length; };
		return within(NV4097_SET_TEXTURE_OFFSET, 8 * 16) || within(NV4097_SET_TEXTURE_CONTROL3, 16) || within(NV4097_SET_TEXTURE_CONTROL2, 16) ||
			within(NV4097_SET_POLY_OFFSET_FILL_ENABLE, 1) || within(NV4097_SET_POLYGON_OFFSET_SCALE_FACTOR, 2);
	};

	// The complete path for a draw whose clause is already set up
	const auto complete_draw = [&]()
	{
		begin();

		if (clause.is_trivial_instanced_draw != is_current_vertex_program_instanced())
		{
			m_graphics_state |= rsx::pipeline_state::xform_instancing_state_dirty;
		}

		end();
		m_fast_draw.fallbacks++;
	};

	u32 flow_commands = 0;

	for (u32 draws = 0; draws < 1024;)
	{
		if (!fetch(pos, 1))
		{
			stop = fifo_end;
			break;
		}

		const u32 cmd = words[0];

		if (cmd & RSX_METHOD_NON_METHOD_CMD_MASK)
		{
			// The display lists of a pass are chained by jumps, calls and returns. The plain ones go through the
			// same steps as in the FIFO loop; a jump to self, an error or anything else is left to that loop.
			if (extended && flow_commands < 256)
			{
				fifo.fast_forward(pos);

				if (fifo_flow_control(cmd))
				{
					pos = fifo.get_pos();
					fifo.fast_forward(pos - 4);
					flow_commands++;
					m_fast_draw.flow_commands++;
					continue;
				}

				fifo.fast_forward(pos - 4);
			}

			stop = flow_control;
			break;
		}

		const u32 reg = (cmd & 0xfffc) >> 2;
		const u32 count = (cmd >> 18) & 0x7ff;
		const bool non_increment = (cmd & RSX_METHOD_NON_INCREMENT_CMD_MASK) == RSX_METHOD_NON_INCREMENT_CMD;

		if (extended && reg == NV4097_NO_OPERATION && count && (non_increment || count == 1) && !rsx::methods[reg] && !rsx::state_signals[reg])
		{
			// Data the game carries in the command stream: every word is stored in the one register, nothing else happens
			bool complete = true;

			for (u32 i = 0; i < count && complete; i++)
			{
				complete = fifo.peek(pos + 4 + i * 4, words[0]);
			}

			if (!complete)
			{
				stop = fifo_end;
				break;
			}

			for (u32 i = 0; i < count; i++)
			{
				fifo.peek(pos + 4 + i * 4, words[0]);
				regs.decode(reg, words[0]);
			}

			pos += 4 + count * 4;
			fifo.fast_forward(pos - 4);
			continue;
		}

		if (!count || count > 33 || non_increment)
		{
			stop = unusual_packet;
			break;
		}

		if (reg != NV4097_SET_BEGIN_END)
		{
			// Setup commands, applied the way their handlers apply them outside BEGIN/END
			// Neither the vertex formats nor the semaphore offset has a handler or signals state: the layout is
			// analysed again for every draw anyway
			if ((reg >= NV4097_SET_VERTEX_DATA_ARRAY_OFFSET && reg + count <= NV4097_SET_VERTEX_DATA_ARRAY_OFFSET + 16) ||
				(extended && reg >= NV4097_SET_VERTEX_DATA_ARRAY_FORMAT && reg + count <= NV4097_SET_VERTEX_DATA_ARRAY_FORMAT + 16) ||
				(semaphores && reg == NV4097_SET_SEMAPHORE_OFFSET && count == 1))
			{
				if (!fetch(pos + 4, count))
				{
					stop = fifo_end;
					break;
				}

				for (u32 i = 0; i < count; i++)
				{
					regs.decode(reg + i, words[i]);
				}
			}
			else if (reg == NV4097_SET_INDEX_ARRAY_ADDRESS && count <= 2)
			{
				if (!fetch(pos + 4, count))
				{
					stop = fifo_end;
					break;
				}

				if (count == 2 && (words[1] & ~(CELL_GCM_LOCATION_MAIN | (CELL_GCM_DRAW_INDEX_ARRAY_TYPE_16 << 4))))
				{
					stop = unusual_packet;
					break;
				}

				for (u32 i = 0; i < count; i++)
				{
					if (regs.registers[reg + i] != words[i])
					{
						m_graphics_state |= rsx::state_signals[reg + i];
					}

					regs.decode(reg + i, words[i]);
				}
			}
			else if (reg >= NV4097_SET_TRANSFORM_CONSTANT_LOAD && reg + count <= NV4097_SET_TRANSFORM_CONSTANT + 32)
			{
				if (!fetch(pos + 4, count))
				{
					stop = fifo_end;
					break;
				}

				const u32 first_constant = (reg == NV4097_SET_TRANSFORM_CONSTANT_LOAD) ? 1 : 0;
				const u32 load = first_constant ? words[0] : regs.transform_constant_load();
				const u32 index = (reg + first_constant) - NV4097_SET_TRANSFORM_CONSTANT;
				const u32 values_count = count - first_constant;

				if (load >= 468 || (load + index / 4) * 4 + (index % 4) + values_count > 468 * 4)
				{
					// Out of range writes have their own handling
					stop = unusual_packet;
					break;
				}

				if (first_constant)
				{
					regs.decode(reg, words[0]);
				}

				if (values_count)
				{
					// The handler stores the first word in the register file and all of them in the constants
					regs.decode(reg + first_constant, words[first_constant]);

					u32* values = &regs.transform_constants[load + index / 4][index % 4];

					if (std::memcmp(values, &words[first_constant], values_count * 4))
					{
						std::memcpy(values, &words[first_constant], values_count * 4);
						m_graphics_state |= rsx::pipeline_state::transform_constants_dirty;
					}
				}
			}
			else if (semaphores && reg == NV4097_TEXTURE_READ_SEMAPHORE_RELEASE && count == 1)
			{
				// An unaligned offset makes the handler reposition the FIFO
				if (regs.semaphore_offset_4097() % 16)
				{
					stop = unusual_packet;
					break;
				}

				if (!fetch(pos + 4, 1))
				{
					stop = fifo_end;
					break;
				}

				// As the FIFO loop dispatches it. What it may leave behind (m_samplers_dirty after the deferred
				// page protections are applied, another command buffer) is checked before every draw.
				regs.decode(reg, words[0]);
				rsx::methods[reg](m_ctx, reg, words[0]);
				m_fast_draw.semaphores++;
			}
			else if (delegated(reg, count))
			{
				if (!fetch(pos + 4, count))
				{
					stop = fifo_end;
					break;
				}

				// As the FIFO loop dispatches them. None of these handlers defers or reads other commands.
				for (u32 i = 0; i < count; i++)
				{
					regs.decode(reg + i, words[i]);

					if (const auto method = rsx::methods[reg + i])
					{
						method(m_ctx, reg + i, words[i]);
					}
					else if (regs.latch != words[i])
					{
						m_graphics_state |= rsx::state_signals[reg + i];
					}
				}
			}
			else
			{
				if (!m_fast_draw_stop_methods) m_fast_draw_stop_methods = std::make_unique<u32[]>(0x4000);
				m_fast_draw_stop_methods[reg]++;
				stop = other_method;
				break;
			}

			pos += 4 + count * 4;
			fifo.fast_forward(pos - 4);
			continue;
		}

		// BEGIN ... DRAW_INDEX_ARRAY ... END, read completely before anything is applied
		if (count != 1 || !fetch(pos + 4, 1))
		{
			stop = count != 1 ? unusual_packet : fifo_end;
			break;
		}

		const u32 begin_arg = words[0];

		if (static_cast<u8>(begin_arg) != static_cast<u8>(rsx::primitive_type::triangles))
		{
			stop = other_primitive;
			break;
		}

		u32 draw_count = 0;
		u32 next = pos + 8;
		bool closed = false;

		while (!closed)
		{
			if (!fetch(next, 1))
			{
				stop = fifo_end;
				break;
			}

			const u32 header = words[0];
			const u32 r = (header & 0xfffc) >> 2;
			const u32 c = (header >> 18) & 0x7ff;
			const bool repeat = (header & RSX_METHOD_NON_INCREMENT_CMD_MASK) == RSX_METHOD_NON_INCREMENT_CMD;

			if ((header & RSX_METHOD_NON_METHOD_CMD_MASK) || !c)
			{
				stop = unusual_draw;
				break;
			}

			if (r == NV4097_DRAW_INDEX_ARRAY && (c == 1 || repeat) && draw_count + c <= 32)
			{
				if (!fetch(next + 4, c))
				{
					stop = fifo_end;
					break;
				}

				std::memcpy(draw_values + draw_count, words, c * 4);
				draw_count += c;
				next += 4 + c * 4;
			}
			else if (r == NV4097_SET_BEGIN_END && c == 1 && !repeat)
			{
				if (!fetch(next + 4, 1))
				{
					stop = fifo_end;
					break;
				}

				if (static_cast<u8>(words[0]) || !draw_count)
				{
					stop = unusual_draw;
					break;
				}

				next += 8;
				closed = true;
			}
			else
			{
				stop = unusual_draw;
				break;
			}
		}

		if (!closed)
		{
			break;
		}

		if (const u32 blocker = fast_draw_blocker(texture_state | depth_bias_state))
		{
			m_fast_draw.not_armed[blocker]++;
			stop = blocked;
			break;
		}

		if (vk::pass_timing::enabled())
		{
			vk::pass_timing::draw(m_surface_info[0].address, m_depth_surface_info.address);
		}

		if (vk::gpu_pass_profile::enabled())
		{
			vk::gpu_pass_profile::mark(*m_current_command_buffer, (u64{ m_surface_info[0].address } << 32) | m_depth_surface_info.address, m_framebuffer_layout.width, m_framebuffer_layout.height);
		}

		// The draw is consumed from here on; set the clause up as BEGIN, DRAW_INDEX_ARRAY and END do
		regs.decode(NV4097_SET_BEGIN_END, begin_arg);
		clause.reset(rsx::primitive_type::triangles);

		for (u32 i = 0; i < draw_count; i++)
		{
			regs.decode(NV4097_DRAW_INDEX_ARRAY, draw_values[i]);
			clause.command = rsx::draw_command::indexed;
			const rsx::registers_decoder<NV4097_DRAW_INDEX_ARRAY>::decoded_type range(draw_values[i]);
			clause.append(range.start(), range.count());
		}

		regs.decode(NV4097_SET_BEGIN_END, 0);
		clause.is_immediate_draw = false;
		clause.compile();

		pos = next;
		fifo.fast_forward(pos - 4);
		draws++;

		if (clause.empty() || clause.is_trivial_instanced_draw || clause.pass_count() != 1 || !clause.is_single_draw())
		{
			complete_draw();
			stop = complete_path;
			break;
		}

		if ((m_graphics_state & texture_state) && !fast_draw_rebind_textures())
		{
			complete_draw();
			stop = complete_path;
			break;
		}

		m_draw_processor.analyse_inputs_interleaved(m_vertex_layout, current_vp_metadata);

		if (!m_vertex_layout.validate())
		{
			complete_draw();
			stop = complete_path;
			break;
		}

		if (m_graphics_state & rsx::pipeline_state::transform_constants_dirty)
		{
			const auto previous = m_vertex_constants_buffer_info;
			update_transform_constants_buffer();

			if (m_vs_binding_table->cbuf_location != umax &&
				(previous.buffer != m_vertex_constants_buffer_info.buffer || previous.offset != m_vertex_constants_buffer_info.offset || previous.range != m_vertex_constants_buffer_info.range))
			{
				m_program->bind_uniform(m_vertex_constants_buffer_info, vk::glsl::binding_set_index_vertex, m_vs_binding_table->cbuf_location);
			}

			m_graphics_state.clear(rsx::pipeline_state::transform_constants_dirty);
		}

		clause.begin();
		const auto upload_info = upload_vertex_data();
		bool emitted = !upload_info.vertex_draw_count;

		if (upload_info.vertex_draw_count)
		{
			// The upload can fault and flush, and ring buffers can be replaced: both need the complete path
			m_vertex_layout_dynamic_offset = m_vertex_layout_ring_info.alloc<8>(168);

			if (m_current_command_buffer == initial_cb && !fast_draw_blocker(depth_bias_state) && vk::is_renderpass_open(*m_current_command_buffer) &&
				bound_buffers() == initial_buffers)
			{
				if (m_graphics_state & depth_bias_state)
				{
					// The other dynamic state is as the previous draw set it
					set_depth_bias_state();
					m_graphics_state.clear(depth_bias_state);
					m_fast_draw.depth_bias_updates++;
				}

				update_vertex_env(0, upload_info);

				VkDescriptorBufferViewEx persistent_buffer = upload_info.static_vertices ? *m_geometry_cache.vertex_heap.view :
					m_persistent_attribute_storage ? *m_persistent_attribute_storage : *null_buffer_view;
				VkDescriptorBufferViewEx volatile_buffer = m_volatile_attribute_storage ? *m_volatile_attribute_storage : *null_buffer_view;
				m_static_vertices_bound = upload_info.static_vertices;

				m_program->bind_uniform(persistent_buffer, vk::glsl::binding_set_index_vertex, m_vs_binding_table->vertex_buffers_location);
				m_program->bind_uniform(volatile_buffer, vk::glsl::binding_set_index_vertex, m_vs_binding_table->vertex_buffers_location + 1);
				m_program->bind(*m_current_command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS);

				if (upload_info.index_info)
				{
					vkCmdBindIndexBuffer(*m_current_command_buffer,
						upload_info.static_indices ? m_geometry_cache.index_heap.buffer->value : m_index_buffer_ring_info.heap->value,
						std::get<0>(*upload_info.index_info), std::get<1>(*upload_info.index_info));
					vkCmdDrawIndexed(*m_current_command_buffer, upload_info.vertex_draw_count, 1, 0, 0, 0);
				}
				else
				{
					vkCmdDraw(*m_current_command_buffer, upload_info.vertex_draw_count, 1, 0, 0);
				}

				emitted = true;
			}
		}

		if (!emitted)
		{
			complete_draw();
			stop = complete_path;
			break;
		}

		while (clause.next());

		m_rtts.on_write(m_framebuffer_layout.color_write_enabled, m_framebuffer_layout.zeta_write_enabled);
		rsx::thread::end();
		m_fast_draw.draws++;
		vk::pass_timing::mark(4);

		if (m_periodic_submit_us)
		{
			maybe_periodic_submit();
		}
	}

	m_fast_draw.stops[stop]++;
	m_fast_draw.in_batch = false;
}

// Verification of the fast path's premise (live control 9 == 3). Nothing is skipped in this mode. For every draw that
// the fast path would take, the bound state is recorded before the complete path runs and compared afterwards:
// if the complete path changed anything, skipping it would have been wrong.
void VKGSRender::fast_draw_verify_begin()
{
	auto& v = m_fast_draw_verify;
	const bool only_consumable = rsx::geometry_sync::track_methods && rsx::geometry_sync::other_methods == 0;
	rsx::geometry_sync::track_methods = true;

	// The constants check of the per-draw blocker is the same one the fast path makes at BEGIN
	v.candidate = v.previous_draw_completed && only_consumable && !m_fast_draw.in_batch &&
		rsx::method_registers.current_draw_clause.primitive == rsx::primitive_type::triangles &&
		vk::is_renderpass_open(*m_current_command_buffer);

	if (v.candidate)
	{
		// Mode 2 is what the blockers test for
		const auto mode = vk::live_ctl::values[9].exchange(2);
		v.candidate = !fast_draw_run_blocker() && !fast_draw_blocker();
		vk::live_ctl::values[9] = mode;
	}

	v.previous_draw_completed = false;

	if (!v.candidate)
	{
		return;
	}

	v.program = m_program;
	v.command_buffer = m_current_command_buffer;
	v.framebuffer = m_draw_fbo;
	v.render_pass = m_cached_renderpass;
	v.pipeline = m_pipeline_properties;
	v.state = m_graphics_state.load();

	const u64 offsets[5] = { m_vertex_env_dynamic_offset, m_fragment_env_dynamic_offset, m_fragment_constants_dynamic_offset, m_texture_parameters_dynamic_offset, m_stipple_array_dynamic_offset };
	const VkBuffer buffers[6] = { m_vertex_env_buffer_info.buffer, m_vertex_layout_stream_info.buffer, m_fragment_env_buffer_info.buffer,
		m_fragment_texture_params_buffer_info.buffer, m_raster_env_buffer_info.buffer, m_fragment_constants_buffer_info.buffer };
	std::memcpy(v.offsets, offsets, sizeof(offsets));
	std::memcpy(v.buffers, buffers, sizeof(buffers));

	for (u32 i = 0; i < 16; i++)
	{
		const auto* sampler = static_cast<const vk::texture_cache::sampled_image_descriptor*>(fs_sampler_state[i].get());
		v.views[i] = ((current_fp_metadata.referenced_textures_mask >> i) & 1) && sampler ? static_cast<const void*>(sampler->image_handle) : nullptr;
	}
}

void VKGSRender::fast_draw_verify_end()
{
	auto& v = m_fast_draw_verify;
	rsx::geometry_sync::other_methods = 0;
	v.previous_draw_completed = true;

	if (!v.candidate)
	{
		return;
	}

	v.candidate = false;
	v.checked++;

	const u64 offsets[5] = { m_vertex_env_dynamic_offset, m_fragment_env_dynamic_offset, m_fragment_constants_dynamic_offset, m_texture_parameters_dynamic_offset, m_stipple_array_dynamic_offset };
	const VkBuffer buffers[6] = { m_vertex_env_buffer_info.buffer, m_vertex_layout_stream_info.buffer, m_fragment_env_buffer_info.buffer,
		m_fragment_texture_params_buffer_info.buffer, m_raster_env_buffer_info.buffer, m_fragment_constants_buffer_info.buffer };

	bool views_equal = true;
	for (u32 i = 0; i < 16; i++)
	{
		const auto* sampler = static_cast<const vk::texture_cache::sampled_image_descriptor*>(fs_sampler_state[i].get());
		const void* view = ((current_fp_metadata.referenced_textures_mask >> i) & 1) && sampler ? static_cast<const void*>(sampler->image_handle) : nullptr;
		views_equal &= view == v.views[i];
	}

	// A draw sets framebuffer_reads_dirty and consumes transform_constants_dirty
	constexpr u32 ignored = rsx::pipeline_state::framebuffer_reads_dirty | rsx::pipeline_state::transform_constants_dirty;

	const bool differs[8] =
	{
		v.program != m_program,
		!(v.pipeline == m_pipeline_properties),
		v.command_buffer != m_current_command_buffer || v.framebuffer != m_draw_fbo || v.render_pass != m_cached_renderpass || !vk::is_renderpass_open(*m_current_command_buffer),
		!!std::memcmp(v.offsets, offsets, sizeof(offsets)),
		!!std::memcmp(v.buffers, buffers, sizeof(buffers)),
		!views_equal,
		((v.state ^ m_graphics_state.load()) & ~ignored) != 0,
		!!(m_current_command_buffer->flags & vk::command_buffer::cb_reload_dynamic_state)
	};

	for (u32 i = 0; i < 8; i++)
	{
		if (differs[i] && v.mismatches[i]++ < 4)
		{
			rsx_log.error("Fast draw check: the complete path changed bound state (kind %u: 0 program, 1 pipeline, 2 command buffer or render pass, "
				"3 environment offsets, 4 buffers, 5 texture views, 6 state bits 0x%x -> 0x%x, 7 dynamic state reload)", i, v.state, m_graphics_state.load());
		}
	}
}
