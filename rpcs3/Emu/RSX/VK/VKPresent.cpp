#include "vkutils/descriptor_reuse.h"
#include "VKLiveCtl.hpp"
#include "stdafx.h"
#include "VKGSRender.h"
#include "VKPassTiming.hpp"
#include "VKGpuPassProfile.hpp"
#include "VKFrameIntervalTrace.hpp"
#include "vkutils/buffer_object.h"
#include "vkutils/memory.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_debug_overlay.h"
#include "Emu/Cell/Modules/cellVideoOut.h"

#include "upscalers/bilinear_pass.hpp"
#include "upscalers/fsr_pass.h"
#include "upscalers/nearest_pass.hpp"
#include "util/asm.hpp"
#include "util/video_provider.h"

extern u64 g_spu_xfloat_fast_checks;
extern u64 g_spu_xfloat_fast_mismatches;
extern u64 g_spu_xfloat_fast_slow;

extern atomic_t<bool> g_user_asked_for_screenshot;
extern atomic_t<recording_mode> g_recording_mode;

namespace
{
	VkFormat RSX_display_format_to_vk_format(u8 format)
	{
		switch (format)
		{
		default:
			rsx_log.error("Unhandled video output format 0x%x", static_cast<s32>(format));
			[[fallthrough]];
		case CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8R8G8B8:
			return VK_FORMAT_B8G8R8A8_UNORM;
		case CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_X8B8G8R8:
			return VK_FORMAT_R8G8B8A8_UNORM;
		case CELL_VIDEO_OUT_BUFFER_COLOR_FORMAT_R16G16B16X16_FLOAT:
			return VK_FORMAT_R16G16B16A16_SFLOAT;
		}
	}
}

bool VKGSRender::reinitialize_swapchain()
{
	m_swapchain_dims.width = m_frame->client_width();
	m_swapchain_dims.height = m_frame->client_height();

	// Reject requests to acquire new swapchain if the window is minimized
	// The NVIDIA driver will spam VK_ERROR_OUT_OF_DATE_KHR if you try to acquire an image from the swapchain and the window is minimized
	// However, any attempt to actually renew the swapchain will crash the driver with VK_ERROR_DEVICE_LOST while the window is in this state
	if (m_swapchain_dims.width == 0 || m_swapchain_dims.height == 0)
	{
		swapchain_unavailable = true;
		return false;
	}

	// NOTE: This operation will create a hard sync point
	close_and_submit_command_buffer(nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, "swapchain_reinitialize");
	m_current_command_buffer->reset();
	m_current_command_buffer->begin();
	diagnostic_command_begin();

	for (auto &ctx : m_frame_context_storage)
	{
		if (ctx.present_image == umax)
			continue;

		// Release present image by presenting it
		frame_context_cleanup(&ctx);
	}

	// NOTE: frame_context_cleanup alters the queued_frames structure.
	while (!m_queued_frames.empty())
	{
		auto& frame = m_queued_frames.front();
		if (!frame->swap_command_buffer)
		{
			// Drop it
			m_queued_frames.pop_front();
			continue;
		}

		frame_context_cleanup(frame);
	}
	ensure(m_queued_frames.empty());

	// Discard the current upscaling pipeline if any
	m_upscaler.reset();

	// Drain all the queues
	vkDeviceWaitIdle(*m_device);

	// Clean the FBO caches
	for (u32 i = 0; i < m_swapchain->get_swap_image_count(); ++i)
	{
		vk::remove_framebuffers_with_image(m_swapchain->get_image(i));
	}

	// Reset frame context storage
	for (auto& ctx : m_frame_context_storage)
	{
		ctx.destroy(*m_device);
	}
	m_current_frame = nullptr;
	m_max_async_frames = 0;
	m_current_queue_index = 0;
	m_frame_context_storage.clear();

	// Rebuild swapchain. Old swapchain destruction is handled by the init_swapchain call
	if (!m_swapchain->init(m_swapchain_dims.width, m_swapchain_dims.height))
	{
		rsx_log.warning("Swapchain initialization failed. Request ignored [%dx%d]", m_swapchain_dims.width, m_swapchain_dims.height);
		swapchain_unavailable = true;
		return false;
	}

	// Re-initialize CPU frame contexts
	m_max_async_frames = m_swapchain->get_swap_image_count();
	m_frame_context_storage.resize(m_max_async_frames);
	for (auto& ctx : m_frame_context_storage)
	{
		ctx.init(*m_device);
	}
	m_current_queue_index = 0;
	m_current_frame = &m_frame_context_storage[0];

	// Prepare new swapchain images for use
	for (u32 i = 0; i < m_swapchain->get_swap_image_count(); ++i)
	{
		const auto target_layout = m_swapchain->get_optimal_present_layout();
		const auto target_image = m_swapchain->get_image(i);
		VkClearColorValue clear_color{};
		VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

		vk::change_image_layout(*m_current_command_buffer, target_image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);
		vkCmdClearColorImage(*m_current_command_buffer, target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_color, 1, &range);
		vk::change_image_layout(*m_current_command_buffer, target_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target_layout, range);
	}

	// Will have to block until rendering is completed
	vk::fence resize_fence(*m_device);

	// Flush the command buffer
	close_and_submit_command_buffer(&resize_fence, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, "swapchain_resize");
	vk::wait_for_fence(&resize_fence);

	m_current_command_buffer->reset();
	m_current_command_buffer->begin();
	diagnostic_command_begin();

	swapchain_unavailable = false;
	should_reinitialize_swapchain = false;
	m_vsync_mode = g_cfg.video.vsync;
	return true;
}

void VKGSRender::present(vk::frame_context_t *ctx)
{
	ensure(ctx->present_image != umax);

	// Partial CS flush
	ctx->swap_command_buffer->flush();

	if (!swapchain_unavailable)
	{
		const VkResult error = m_swapchain->present(ctx->present_wait_semaphore, ctx->present_image);
		vk_frame_interval_trace::record_present(static_cast<int32_t>(error), ctx->present_image);
		switch (error)
		{
		case VK_SUCCESS:
			break;
		case VK_SUBOPTIMAL_KHR:
#ifndef ANDROID
			should_reinitialize_swapchain = true;
#endif
			break;
		case VK_ERROR_OUT_OF_DATE_KHR:
			swapchain_unavailable = true;
			break;
		default:
			// Other errors not part of rpcs3. This can be caused by 3rd party injectors with bad code, of which we have no control over.
			// Let the application attempt to recover instead of crashing outright.
			rsx_log.error("VkPresent returned unexpected error code %lld. Will attempt to recreate the swapchain. Please disable 3rd party injector tools.", static_cast<s64>(error));
			swapchain_unavailable = true;
			break;
		}
	}

	// Presentation image released; reset value
	ctx->present_image = -1;
}

void VKGSRender::advance_queued_frames()
{
	// Check all other frames for completion and clear resources
	check_present_status();

	// Run video memory balancer
	m_device->rebalance_memory_type_usage();
	vk::vmm_check_memory_usage();

	// m_rtts storage is double buffered and should be safe to tag on frame boundary
	m_rtts.trim(*m_current_command_buffer, vk::vmm_determine_memory_load_severity());

	// Texture cache is also double buffered to prevent use-after-free
	m_texture_cache.on_frame_end();
	m_samplers_dirty.store(true);
	if (g_spu_xfloat_fast_checks && vk::get_current_frame_id() % 600 == 0)
	{
		rsx_log.notice("SPU fast xfloat: %u operations checked, %u mismatches, %u took the regular path (since start)", g_spu_xfloat_fast_checks, g_spu_xfloat_fast_mismatches, g_spu_xfloat_fast_slow);
	}

	if (vk::live_ctl::get(11) && vk::get_current_frame_id() % 600 == 0)
	{
		rsx_log.notice("Pipeline reuse: %u clean reuses checked, %u mismatches, %u unchanged dirty states cleared (600 frames)",
			m_pipeline_reuse_checked, m_pipeline_reuse_mismatches, m_pipeline_reuse_clears);
		m_pipeline_reuse_checked = m_pipeline_reuse_mismatches = m_pipeline_reuse_clears = 0;
	}

	if (vk::live_ctl::get(12) && vk::get_current_frame_id() % 600 == 0)
	{
		namespace r = vk::descriptor_reuse;
		rsx_log.notice("Descriptor sets: per frame: %.0f reused, %.0f written, %.1f not keyed, %.1f written the regular way; destroyed per frame: %.1f views, %.1f samplers; "
			"%.1f entries dropped for them in %.1f passes; %u reuse pools retired in the interval",
			r::hits / 600., r::misses / 600., r::unkeyed / 600., r::fallbacks / 600., r::retired_views.exchange(0) / 600., r::retired_samplers.exchange(0) / 600.,
			r::dropped / 600., r::syncs / 600., static_cast<u32>(r::pools_retired));
		r::hits = r::misses = r::unkeyed = r::syncs = r::dropped = r::pools_retired = r::fallbacks = 0;
	}

	if (vk::live_ctl::get(10) && vk::get_current_frame_id() % 600 == 0)
	{
		auto& b = m_material_bindings;
		rsx_log.notice("Material bindings: %u lookups, %u hits, %u checked, %u mismatches, %u invalidations (600 frames); generation changes: texture cache %u, section release %u, surface cache %u (new surface pages %u, %u pages marked); stores %u, sharing pages with a surface %u",
			b.lookups, b.hits, b.checked, b.mismatches, b.invalidations, b.changed[0], b.changed[1], b.changed[2], b.new_surface_pages, b.surface_page_count(), b.stores, b.surface_dependent_stores);
		b.new_surface_pages = b.stores = b.surface_dependent_stores = 0;
		b.lookups = b.hits = b.checked = b.mismatches = b.invalidations = 0;
		b.changed[0] = b.changed[1] = b.changed[2] = 0;
	}

	vk::remove_unused_framebuffers();

	m_vertex_cache->purge();

	m_multiblock_vertex_cache.end_frame(vk::live_ctl::get(2) == 3);

	if (vk::pass_timing::enabled())
	{
		if (const auto report = vk::pass_timing::frame(vk::get_current_frame_id()); !report.empty())
		{
			rsx_log.notice("Pass timing per frame:%s", report);
		}
	}

	if (vk::live_ctl::get(9) == 3)
	{
		auto& v = m_fast_draw_verify;
		const u32 frame = static_cast<u32>(vk::get_current_frame_id());

		if (frame - m_fast_draw.report_frame >= 600)
		{
			rsx_log.notice("Fast draw check: %.0f qualifying draws per frame checked; mismatches by kind: %u %u %u %u %u %u %u %u",
				v.checked / f64(frame - m_fast_draw.report_frame), static_cast<u32>(v.mismatches[0]), static_cast<u32>(v.mismatches[1]), static_cast<u32>(v.mismatches[2]),
				static_cast<u32>(v.mismatches[3]), static_cast<u32>(v.mismatches[4]), static_cast<u32>(v.mismatches[5]), static_cast<u32>(v.mismatches[6]), static_cast<u32>(v.mismatches[7]));
			v.checked = 0;
			std::memset(v.mismatches, 0, sizeof(v.mismatches));
			m_fast_draw.report_frame = frame;
		}
	}

	if (fast_draw_enabled())
	{
		auto& fast = m_fast_draw;
		const u32 frame = static_cast<u32>(vk::get_current_frame_id());

		if (frame - fast.report_frame >= 600)
		{
			const f64 frames = frame - fast.report_frame;
			std::string stops, blockers;
			for (u32 i = 0; i < 17; i++)
			{
				if (i < 12 && fast.stops[i]) fmt::append(stops, " %u:%.0f", i, fast.stops[i] / frames);
				if (fast.not_armed[i]) fmt::append(blockers, " %u:%.0f", i, fast.not_armed[i] / frames);
			}

			rsx_log.notice("Fast draws: per frame: %.0f draws (%.0f of them inline arrays) in %.0f runs, %.1f sent to the complete path; run ends by reason:%s; blocked by reason:%s (state bits 0x%x)",
				fast.draws / frames, fast.inline_draws / frames, fast.batches / frames, fast.fallbacks / frames, stops, blockers, fast.blocking_state_bits);
			rsx_log.notice("Fast draws: per frame: %.0f with textures set up again, %.1f of those needed another shader variant, %.0f depth bias updates, %.0f semaphore releases, %.0f jumps/calls/returns, %.0f report commands; "
				"program checks in the interval: %u, mismatches %u",
				fast.texture_rebinds / frames, fast.texture_program_changes / frames, fast.depth_bias_updates / frames, fast.semaphores / frames, fast.flow_commands / frames, fast.reports / frames,
				static_cast<u32>(fast.texture_checks), static_cast<u32>(fast.texture_check_mismatches));

			// Why a draw's textures kept it on the complete path (blocker 14)
			rsx_log.notice("Fast draws: textures in the way per frame: dirty, missing or sampled while rendered to %.0f; render target assembled %.0f, bound %.0f, resolved %.0f, written since %.0f; other source %.0f; assembled %.0f",
				fast.texture_blockers[0] / frames, fast.texture_blockers[1] / frames, fast.texture_blockers[2] / frames, fast.texture_blockers[3] / frames, fast.texture_blockers[4] / frames,
				fast.texture_blockers[5] / frames, fast.texture_blockers[6] / frames);

			if (m_fast_draw_stop_methods)
			{
				// The methods that ended most runs
				std::string methods;
				for (u32 n = 0; n < 12; n++)
				{
					u32 best = 0;
					for (u32 reg = 1; reg < 0x4000; reg++)
					{
						if (m_fast_draw_stop_methods[reg] > m_fast_draw_stop_methods[best]) best = reg;
					}

					if (!m_fast_draw_stop_methods[best]) break;
					fmt::append(methods, " 0x%x:%.0f", best * 4, m_fast_draw_stop_methods[best] / frames);
					m_fast_draw_stop_methods[best] = 0;
				}

				std::memset(m_fast_draw_stop_methods.get(), 0, 0x4000 * sizeof(u32));
				rsx_log.notice("Fast draws: runs ended per frame by method:%s", methods);
			}

			fast = {};
			fast.report_frame = frame;
		}
	}

	m_geometry_cache.sequence.end_frame();

	if (vk::geometry_cache::suspended && static_cast<u32>(vk::get_current_frame_id()) - vk::geometry_cache::resume_frame < 0x80000000u)
	{
		vk::geometry_cache::suspended = false;
		m_geometry_cache.stats = {};
		m_geometry_cache.report_frame = static_cast<u32>(vk::get_current_frame_id());
	}

	if (vk::geometry_cache::mode())
	{
		// Periodic report (per-frame averages over the interval)
		auto& cache = m_geometry_cache;
		const u32 frame = static_cast<u32>(vk::get_current_frame_id());

		if (frame - cache.report_frame >= 600)
		{
			const auto& st = cache.stats;
			auto& watch = rsx::write_watch::state();
			const f64 frames = frame - cache.report_frame;
			rsx_log.notice("Geometry cache: per frame: vertex %.0f requests, %.0f static, %.0f same-frame, %.1f promoted, %.1f invalidated; "
				"index %.0f requests, %.0f static, %.1f promoted, %.1f invalidated; %.2f MB reused; "
				"scans %.1f (%.0f us), %.0f written pages; watched %u MB, heaps %u/%u MB, %u resets, lost chunks %u, %u blocks set dynamic; checks %u, mismatches %u",
				st.vertex_requests / frames, st.vertex_static_hits / frames, st.vertex_ring_hits / frames, st.vertex_promotions / frames, st.vertex_invalidations / frames,
				st.index_requests / frames, st.index_static_hits / frames, st.index_promotions / frames, st.index_invalidations / frames,
				st.bytes_reused / frames / 1048576., watch.scans / frames, watch.scan_ns / frames / 1000., watch.written_pages / frames,
				watch.watched_chunks, cache.vertex_heap.used >> 20, cache.index_heap.used >> 20, static_cast<u32>(st.heap_resets), static_cast<u32>(watch.lost_chunks), static_cast<u32>(st.dynamic_blocks),
				static_cast<u32>(st.verify_checks), static_cast<u32>(st.verify_mismatches));

			// Under a twentieth of the requests answered: not worth its lookups (mode 2 checks contents and stays on)
			if (const u64 requests = st.vertex_requests + st.index_requests; vk::geometry_cache::mode() == 1 && requests > 600 * 50 &&
				(st.vertex_static_hits + st.index_static_hits) * 20 < requests)
			{
				vk::geometry_cache::suspended = true;
				vk::geometry_cache::resume_frame = frame + 6000;
				rsx_log.notice("Geometry cache: suspended for 6000 frames, %.1f%% of the requests were static", (st.vertex_static_hits + st.index_static_hits) * 100. / requests);
			}

			cache.stats = {};
			watch.scans = watch.scan_ns = watch.written_pages = watch.lost_chunks = 0;
			cache.report_frame = frame;
		}
	}
	m_current_frame->tag_frame_end();

	m_queued_frames.push_back(m_current_frame);
	ensure(m_queued_frames.size() <= m_max_async_frames);

	m_current_queue_index = (m_current_queue_index + 1) % m_max_async_frames;
	m_current_frame = &m_frame_context_storage[m_current_queue_index];
	m_current_frame->flags |= frame_context_state::dirty;

	vk::advance_frame_counter();
}

void VKGSRender::queue_swap_request()
{
	// Live control for experiments: re-read the control file about once a second.
	if (static const char* ctl = std::getenv("RPCS3_VK_LIVE_CTL"); ctl)
	{
		static u64 last_poll_us = 0;
		if (const u64 now = get_system_time(); now - last_poll_us > 1'000'000)
		{
			last_poll_us = now;
			if (FILE* f = std::fopen(ctl, "r"))
			{
				unsigned long long value = 0;
				for (u32 i = 0; i < std::size(vk::live_ctl::values) && std::fscanf(f, "%llu", &value) == 1; i++)
				{
					if (vk::live_ctl::values[i].exchange(value) != value)
					{
						rsx_log.notice("Live control %u set to %u", i, value);
					}
				}
				std::fclose(f);
				m_periodic_submit_us = vk::live_ctl::get(0);
			}
		}
	}

	// The completed flip is the exact budget boundary used by GPU diagnostics.
	if (m_draw_prefix_submit_enabled)
	{
		m_draw_prefix_armed = false;
		m_draw_prefix_submitted = false;
		m_draw_prefix_begin_us = 0;
		m_draw_prefix_draws = 0;
	}

	ensure(!m_current_frame->swap_command_buffer);
	m_current_frame->swap_command_buffer = m_current_command_buffer;

	if (m_swapchain->is_headless())
	{
		m_swapchain->end_frame(*m_current_command_buffer, m_current_frame->present_image);
		close_and_submit_command_buffer(nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, "queue_swap_headless");
	}
	else
	{
		close_and_submit_command_buffer(nullptr,
			m_current_frame->acquire_signal_semaphore,
			m_current_frame->present_wait_semaphore,
			VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, "queue_swap_present");
	}

	// Set up a present request for this frame as well
	{ vk_frame_interval_trace::PresentBinding trace_binding; present(m_current_frame); }

	diagnostic_frame_end();

	// Grab next cb in line and make it usable
	m_current_command_buffer = m_primary_cb_list.next();
	m_current_command_buffer->reset();
	m_current_command_buffer->begin();
	diagnostic_command_begin();

	// Set up new pointers for the next frame
	advance_queued_frames();
}

void VKGSRender::frame_context_cleanup(vk::frame_context_t *ctx)
{
	ensure(ctx->swap_command_buffer);

	// Perform hard swap here
	if (ctx->swap_command_buffer->wait(FRAME_PRESENT_TIMEOUT) != VK_SUCCESS)
	{
		// Lost surface/device, release swapchain
		swapchain_unavailable = true;
	}

	// Resource cleanup.
	{
		if (m_overlay_manager && m_overlay_manager->has_dirty())
		{
			auto ui_renderer = vk::get_overlay_pass<vk::ui_overlay_renderer>();
			m_overlay_manager->lock_shared();

			std::vector<u32> uids_to_dispose;
			uids_to_dispose.reserve(m_overlay_manager->get_dirty().size());

			for (const auto& view : m_overlay_manager->get_dirty())
			{
				ui_renderer->remove_temp_resources(view->uid);
				uids_to_dispose.push_back(view->uid);
			}

			m_overlay_manager->unlock_shared();
			m_overlay_manager->dispose(uids_to_dispose);
		}

		vk::get_resource_manager()->trim();

		vk::reset_global_resources();

		if (ctx->last_frame_sync_time > m_last_heap_sync_time)
		{
			m_last_heap_sync_time = ctx->last_frame_sync_time;

			// Heap cleanup; deallocates memory consumed by the frame if it is still held
			vk::data_heap_manager::restore_snapshot(ctx->heap_snapshot);
		}
	}

	ctx->swap_command_buffer = nullptr;

	// Remove from queued list
	while (!m_queued_frames.empty())
	{
		auto frame = m_queued_frames.front();
		m_queued_frames.pop_front();

		if (frame == ctx)
		{
			break;
		}
	}

	vk::advance_completed_frame_counter();
}

vk::viewable_image* VKGSRender::get_present_source(/* inout */ vk::present_surface_info* info, const rsx::avconf& avconfig)
{
	vk::viewable_image* image_to_flip = nullptr;

	// @FIXME: This entire function needs to be rewritten to go through the texture cache's "upload_texture" routine.
	// That method is not a 1:1 replacement due to handling of insets that is done differently here.

	// Check the surface store first
	const auto format_bpp = rsx::get_format_block_size_in_bytes(info->format);
	const auto overlap_info = m_rtts.get_merged_texture_memory_region(*m_current_command_buffer,
		info->address, info->width, info->height, info->pitch, format_bpp, rsx::surface_access::transfer_read);

	if (!overlap_info.empty())
	{
		const auto& section = overlap_info.back();
		auto surface = vk::as_rtt(section.surface);
		bool viable = false;

		if (section.base_address >= info->address)
		{
			const auto surface_width = surface->get_surface_width<rsx::surface_metrics::samples>();
			const auto surface_height = surface->get_surface_height<rsx::surface_metrics::samples>();

			if (section.base_address == info->address)
			{
				// Check for fit or crop
				viable = (surface_width >= info->width && surface_height >= info->height);
			}
			else
			{
				// Check for borders and letterboxing
				const u32 inset_offset = section.base_address - info->address;
				const u32 inset_y = inset_offset / info->pitch;
				const u32 inset_x = (inset_offset % info->pitch) / format_bpp;

				const u32 full_width = surface_width + inset_x + inset_x;
				const u32 full_height = surface_height + inset_y + inset_y;

				viable = (full_width == info->width && full_height == info->height);
			}

			if (viable)
			{
				image_to_flip = section.surface->get_surface(rsx::surface_access::transfer_read);

				std::tie(info->width, info->height) = rsx::apply_resolution_scale<true>(
					resolution_scaling_config,
					std::min(surface_width, info->width),
					std::min(surface_height, info->height));
			}
		}
	}
	else if (auto surface = m_texture_cache.find_texture_from_dimensions<true>(info->address, info->format);
			 surface && surface->get_width() >= info->width && surface->get_height() >= info->height)
	{
		// Hack - this should be the first location to check for output
		// The render might have been done offscreen or in software and a blit used to display
		image_to_flip = dynamic_cast<vk::viewable_image*>(surface->get_raw_texture());
	}

	// The correct output format is determined by the AV configuration set in CellVideoOutConfigure by the game.
	// 99.9% of the time, this will match the backbuffer fbo format used in rendering/compositing the output.
	// But in some cases, let's just say some devs are creative.
	const auto expected_format = RSX_display_format_to_vk_format(avconfig.format);

	if (!image_to_flip) [[ unlikely ]]
	{
		// Read from cell
		const auto range = utils::address_range32::start_length(info->address, info->pitch * info->height);
		const u32  lookup_mask = rsx::texture_upload_context::blit_engine_dst | rsx::texture_upload_context::framebuffer_storage;
		const auto overlap = m_texture_cache.find_texture_from_range<true>(range, 0, lookup_mask);

		for (const auto & section : overlap)
		{
			if (!section->is_synchronized())
			{
				section->copy_texture(*m_current_command_buffer, true);
			}
		}

		if (m_current_command_buffer->flags & vk::command_buffer::cb_has_dma_transfer)
		{
			// Submit for processing to lower hard fault penalty
			flush_command_queue(false, false, "present_source_dma");
		}

		m_texture_cache.invalidate_range(*m_current_command_buffer, range, rsx::invalidation_cause::read);
		image_to_flip = m_texture_cache.upload_image_simple(*m_current_command_buffer, expected_format, info->address, info->width, info->height, info->pitch);
	}
	else if (image_to_flip->format() != expected_format)
	{
		// Devs are being creative. Force-cast this to the proper pixel layout.
		auto dst_img = m_texture_cache.create_temporary_subresource_storage(
			RSX_FORMAT_CLASS_COLOR, expected_format, info->width, info->height, 1, 1, 1,
			VK_IMAGE_TYPE_2D, 0, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

		if (dst_img)
		{
			const areai src_rect = { 0, 0, static_cast<int>(info->width), static_cast<int>(info->height) };
			const areai dst_rect = src_rect;

			dst_img->change_layout(*m_current_command_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

			if (vk::formats_are_bitcast_compatible(dst_img.get(), image_to_flip))
			{
				vk::copy_image(*m_current_command_buffer, image_to_flip, dst_img.get(), src_rect, dst_rect);
			}
			else
			{
				vk::copy_image_typeless(*m_current_command_buffer, image_to_flip, dst_img.get(), src_rect, dst_rect);
			}

			image_to_flip = dst_img.get();
			m_texture_cache.dispose_reusable_image(dst_img);
		}
	}

	return image_to_flip;
}

void VKGSRender::flip(const rsx::display_flip_info_t& info)
{
	vk_frame_interval_trace::Scope interval_trace(info.emu_flip, info.skip_frame, info.buffer);
	vk::gpu_pass_profile::frame(*m_current_command_buffer);
	// Check swapchain condition/status
	if (!m_swapchain->supports_automatic_wm_reports())
	{
		if (m_swapchain_dims.width != m_frame->client_width() + 0u ||
			m_swapchain_dims.height != m_frame->client_height() + 0u)
		{
			swapchain_unavailable = true;
		}
	}

	if (m_vsync_mode != g_cfg.video.vsync)
	{
		swapchain_unavailable = true;
	}

	if (swapchain_unavailable || should_reinitialize_swapchain)
	{
		// Reinitializing the swapchain is a failable operation. However, not all failures are fatal (e.g minimized window).
		// In the worst case, we can have the driver refuse to create the swapchain while we already deleted the previous one.
		// In such scenarios, we have to retry a few times before giving up as we cannot proceed without a swapchain.
		for (int i = 0; i < 10; ++i)
		{
			if (reinitialize_swapchain() || m_current_frame)
			{
				// If m_current_frame exists, then the initialization failure is non-fatal. Proceed as usual.
				break;
			}

			if (Emu.IsStopped())
			{
				m_frame->flip(m_context);
				rsx::thread::flip(info);
				interval_trace.complete(swapchain_unavailable, 2);
				return;
			}

			std::this_thread::sleep_for(100ms);
		}
	}

	m_profiler.start();

	ensure(m_current_frame, "Invalid swapchain setup. Resizing the game window failed.");

	if (m_current_frame == &m_aux_frame_context)
	{
		m_current_frame = &m_frame_context_storage[m_current_queue_index];
		if (m_current_frame->swap_command_buffer)
		{
			// Its possible this flip request is triggered by overlays and the flip queue is in undefined state
			frame_context_cleanup(m_current_frame);
		}

		// Swap aux storage and current frame; aux storage should always be ready for use at all times
		m_current_frame->grab_resources(m_aux_frame_context);
	}
	else if (m_current_frame->swap_command_buffer)
	{
		if (info.stats.draw_calls > 0)
		{
			// This can be 'legal' if the window was being resized and no polling happened because of swapchain_unavailable flag
			rsx_log.error("Possible data corruption on frame context storage detected");
		}

		// There were no draws and back-to-back flips happened
		frame_context_cleanup(m_current_frame);
	}

	if (info.skip_frame || swapchain_unavailable)
	{
		if (!info.skip_frame)
		{
			ensure(swapchain_unavailable);

			// Perform a mini-flip here without invoking present code
			m_current_frame->swap_command_buffer = m_current_command_buffer;
			flush_command_queue(true, false, "flip_swapchain_unavailable");
			vk::advance_frame_counter();
			frame_context_cleanup(m_current_frame);
		}

		m_frame->flip(m_context);
		rsx::thread::flip(info);
		interval_trace.complete(swapchain_unavailable, 1);
		return;
	}

	u32 buffer_width = display_buffers[info.buffer].width;
	u32 buffer_height = display_buffers[info.buffer].height;
	u32 buffer_pitch = display_buffers[info.buffer].pitch;

	u32 av_format;
	const auto& avconfig = g_fxo->get<rsx::avconf>();

	if (!buffer_width)
	{
		buffer_width = avconfig.resolution_x;
		buffer_height = avconfig.resolution_y;
	}

	if (avconfig.state)
	{
		av_format = avconfig.get_compatible_gcm_format();
		if (!buffer_pitch)
			buffer_pitch = buffer_width * avconfig.get_bpp();

		const size2u video_frame_size = avconfig.video_frame_size();
		buffer_width = std::min(buffer_width, video_frame_size.width);
		buffer_height = std::min(buffer_height, video_frame_size.height);
	}
	else
	{
		av_format = CELL_GCM_TEXTURE_A8R8G8B8;
		if (!buffer_pitch)
			buffer_pitch = buffer_width * 4;
	}

	// Scan memory for required data. This is done early to optimize waiting for the driver image acquire below.
	vk::viewable_image* image_to_flip = nullptr;
	vk::viewable_image* image_to_flip2 = nullptr;

	if (info.buffer < display_buffers_count && buffer_width && buffer_height)
	{
		vk::present_surface_info present_info
		{
			.address = rsx::get_address(display_buffers[info.buffer].offset, CELL_GCM_LOCATION_LOCAL),
			.format = av_format,
			.width = buffer_width,
			.height = buffer_height,
			.pitch = buffer_pitch,
			.eye = 0
		};
		image_to_flip = get_present_source(&present_info, avconfig);

		if (avconfig.stereo_enabled) [[unlikely]]
		{
			const auto [unused, min_expected_height] = rsx::apply_resolution_scale<true>(resolution_scaling_config, RSX_SURFACE_DIMENSION_IGNORED, buffer_height + 30);
			if (image_to_flip->height() < min_expected_height)
			{
				// Get image for second eye
				const u32 image_offset = (buffer_height + 30) * buffer_pitch + display_buffers[info.buffer].offset;
				present_info.width = buffer_width;
				present_info.height = buffer_height;
				present_info.address = rsx::get_address(image_offset, CELL_GCM_LOCATION_LOCAL);
				present_info.eye = 1;

				image_to_flip2 = get_present_source(&present_info, avconfig);
			}
			else
			{
				// Account for possible insets
				const auto [unused2, scaled_buffer_height] = rsx::apply_resolution_scale<true>(resolution_scaling_config, RSX_SURFACE_DIMENSION_IGNORED, buffer_height);
				buffer_height = std::min<u32>(image_to_flip->height() - min_expected_height, scaled_buffer_height);
			}
		}

		buffer_width = present_info.width;
		buffer_height = present_info.height;
	}

	if (info.emu_flip)
	{
		evaluate_cpu_usage_reduction_limits();
	}

	// Prepare surface for new frame. Set no timeout here so that we wait for the next image if need be
	ensure(m_current_frame->present_image == umax);
	ensure(m_current_frame->swap_command_buffer == nullptr);

	u64 timeout = m_swapchain->get_swap_image_count() <= 2? 0ull: 100000000ull;
	while (VkResult status = m_swapchain->acquire_next_swapchain_image(m_current_frame->acquire_signal_semaphore, timeout, &m_current_frame->present_image))
	{
		switch (status)
		{
		case VK_TIMEOUT:
		case VK_NOT_READY:
		{
			// In some cases, after a fullscreen switch, the driver only allows N-1 images to be acquirable, where N = number of available swap images.
			// This means that any acquired images have to be released
			// before acquireNextImage can return successfully. This is despite the driver reporting 2 swap chain images available
			// This makes fullscreen performance slower than windowed performance as throughput is lowered due to losing one presentable image
			// Found on AMD Crimson 17.7.2


			// Whatever returned from status, this is now a spin
			timeout = 0ull;
			check_present_status();
			continue;
		}
		case VK_SUBOPTIMAL_KHR:
			should_reinitialize_swapchain = true;
			break;
		case VK_ERROR_OUT_OF_DATE_KHR:
			rsx_log.warning("vkAcquireNextImageKHR failed with VK_ERROR_OUT_OF_DATE_KHR. Flip request ignored until surface is recreated.");
			swapchain_unavailable = true;
			reinitialize_swapchain();
			ensure(m_current_frame, "Could not reinitialize swapchain after VK_ERROR_OUT_OF_DATE_KHR signal!");
			continue;
		default:
			vk::die_with_error(status);
		}

		if (should_reinitialize_swapchain)
		{
			// Image is valid, new swapchain will be generated later
			break;
		}
	}

	// Confirm that the driver did not silently fail
	ensure(m_current_frame->present_image != umax);

	// Calculate output dimensions. Done after swapchain acquisition in case it was recreated.
	areai aspect_ratio;
	if (!g_cfg.video.stretch_to_display_area)
	{
		const auto converted = avconfig.aspect_convert_region({ buffer_width, buffer_height }, m_swapchain_dims);
		aspect_ratio = static_cast<areai>(converted);
	}
	else
	{
		aspect_ratio = { 0, 0, s32(m_swapchain_dims.width), s32(m_swapchain_dims.height) };
	}

	// Blit contents to screen..
	VkImage target_image = m_swapchain->get_image(m_current_frame->present_image);
	const auto present_layout = m_swapchain->get_optimal_present_layout();

	const VkImageSubresourceRange subresource_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	VkImageLayout target_layout = present_layout;

	VkRenderPass single_target_pass = VK_NULL_HANDLE;
	vk::framebuffer_holder* direct_fbo = nullptr;
	rsx::simple_array<vk::viewable_image*> calibration_src;

	const bool has_overlay = (m_overlay_manager && m_overlay_manager->has_visible());
	const bool user_asked_for_screenshot = g_user_asked_for_screenshot.exchange(false);
	const bool user_is_recording = (g_recording_mode != recording_mode::stopped && m_frame->can_consume_frame());
	const bool need_media_capture = user_asked_for_screenshot || user_is_recording;

	const auto render_overlays = [&](vk::framebuffer_holder* fbo, const areau& area)
	{
		if (!has_overlay) return;

		// Lock to avoid modification during run-update chain
		auto ui_renderer = vk::get_overlay_pass<vk::ui_overlay_renderer>();
		std::lock_guard lock(*m_overlay_manager);

		const areau display_area = {0, 0, static_cast<u32>(m_swapchain_dims.width), static_cast<u32>(m_swapchain_dims.height)};
		for (const auto& view : m_overlay_manager->get_views())
		{
			const areau render_area = view->use_window_space ? display_area : area;
			ui_renderer->run(*m_current_command_buffer, render_area, fbo, single_target_pass, m_texture_upload_buffer_ring_info, *view.get());
		}
	};

	// WARNING: We have to do this here. We cannot touch the acquired image on the CB and then do a hard sync on it before it is submitted to the presentation engine.
	// That introduces a WRITE_AFTER_PRESENT (from the previous present) when we later try to present on a different CB
	if (image_to_flip && need_media_capture)
	{
		const usz sshot_size = buffer_height * buffer_width * 4;

		vk::buffer sshot_vkbuf(*m_device, utils::align(sshot_size, 0x100000), m_device->get_memory_mapping().host_visible_coherent,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0, VMM_ALLOCATION_POOL_UNDEFINED);

		VkBufferImageCopy copy_info{};
		copy_info.bufferOffset = 0;
		copy_info.bufferRowLength = 0;
		copy_info.bufferImageHeight = 0;
		copy_info.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy_info.imageSubresource.baseArrayLayer = 0;
		copy_info.imageSubresource.layerCount = 1;
		copy_info.imageSubresource.mipLevel = 0;
		copy_info.imageOffset.x = 0;
		copy_info.imageOffset.y = 0;
		copy_info.imageOffset.z = 0;
		copy_info.imageExtent.width = buffer_width;
		copy_info.imageExtent.height = buffer_height;
		copy_info.imageExtent.depth = 1;

		vk::image* image_to_copy = image_to_flip;

		if (g_cfg.video.record_with_overlays && has_overlay)
		{
			const auto key = vk::get_renderpass_key(m_swapchain->get_surface_format());
			single_target_pass = vk::get_renderpass(*m_device, key);
			ensure(single_target_pass != VK_NULL_HANDLE);

			if (m_overlay_recording_img)
			{
				// Validate
				if (m_overlay_recording_img->format() != image_to_flip->format() ||
					m_overlay_recording_img->width() != image_to_flip->width() ||
					m_overlay_recording_img->height() != image_to_flip->height())
				{
					// Dispose correctly
					vk::remove_framebuffers_with_image(m_overlay_recording_img.get());
					vk::get_resource_manager()->dispose(m_overlay_recording_img);
				}
			}

			if (!m_overlay_recording_img)
			{
				m_overlay_recording_img = std::make_unique<vk::image>(*m_device, m_device->get_memory_mapping().device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					VK_IMAGE_TYPE_2D, image_to_flip->format(), image_to_flip->width(), image_to_flip->height(), 1, 1, 1, VK_SAMPLE_COUNT_1_BIT,
					VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
					0, VMM_ALLOCATION_POOL_SYSTEM);
			}

			m_overlay_recording_img->change_layout(*m_current_command_buffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			image_to_flip->push_layout(*m_current_command_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

			const areai rect = areai(0, 0, buffer_width, buffer_height);
			vk::copy_image(*m_current_command_buffer, image_to_flip, m_overlay_recording_img.get(), rect, rect);

			image_to_flip->pop_layout(*m_current_command_buffer);
			m_overlay_recording_img->change_layout(*m_current_command_buffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

			vk::framebuffer_holder* sshot_fbo = vk::get_framebuffer(*m_device, buffer_width, buffer_height, VK_FALSE, single_target_pass, { m_overlay_recording_img.get() });
			sshot_fbo->add_ref();
			render_overlays(sshot_fbo, areau(rect));
			sshot_fbo->release();

			image_to_copy = m_overlay_recording_img.get();
		}

		image_to_copy->push_layout(*m_current_command_buffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vk::copy_image_to_buffer(*m_current_command_buffer, image_to_copy, &sshot_vkbuf, copy_info);
		image_to_copy->pop_layout(*m_current_command_buffer);

		flush_command_queue(true, false, "flip_screenshot_readback");
		const auto src = sshot_vkbuf.map(0, sshot_size);
		std::vector<u8> sshot_frame(sshot_size);
		memcpy(sshot_frame.data(), src, sshot_size);
		sshot_vkbuf.unmap();

		const bool is_bgra = image_to_copy->format() == VK_FORMAT_B8G8R8A8_UNORM;

		if (user_asked_for_screenshot)
		{
			m_frame->take_screenshot(std::move(sshot_frame), buffer_width, buffer_height, is_bgra);
		}
		else
		{
			m_frame->present_frame(std::move(sshot_frame), buffer_width * 4, buffer_width, buffer_height, is_bgra);
		}
	}

	if (!image_to_flip || aspect_ratio.x1 || aspect_ratio.y1)
	{
		// Clear the window background to black
		target_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		VkClearColorValue clear_black {};

		vk::change_image_layout(*m_current_command_buffer, target_image, present_layout, target_layout, subresource_range);
		vkCmdClearColorImage(*m_current_command_buffer, target_image, target_layout, &clear_black, 1, &subresource_range);

		// Prevent WAW on transfer writes
		vk::insert_image_memory_barrier(
			*m_current_command_buffer,
			target_image,
			target_layout,
			target_layout,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT,
			subresource_range
		);
	}

	const output_scaling_mode output_scaling = g_cfg.video.output_scaling.get();

	if (!m_upscaler || m_output_scaling != output_scaling)
	{
		m_output_scaling = output_scaling;

		if (m_output_scaling == output_scaling_mode::nearest)
		{
			m_upscaler = std::make_unique<vk::nearest_upscale_pass>();
		}
		else if (m_output_scaling == output_scaling_mode::fsr)
		{
			m_upscaler = std::make_unique<vk::fsr_upscale_pass>();
		}
		else
		{
			m_upscaler = std::make_unique<vk::bilinear_upscale_pass>();
		}
	}

	if (image_to_flip)
	{
		const bool use_full_rgb_range_output = g_cfg.video.full_rgb_range_output.get();

		if (!use_full_rgb_range_output || !rsx::fcmp(avconfig.gamma, 1.f) || avconfig.stereo_enabled) [[unlikely]]
		{
			if (image_to_flip) calibration_src.push_back(image_to_flip);
			if (image_to_flip2) calibration_src.push_back(image_to_flip2);

			if (m_output_scaling == output_scaling_mode::fsr && !avconfig.stereo_enabled) // 3D will be implemented later
			{
				// Run upscaling pass before the rest of the output effects pipeline
				// This can be done with all upscalers but we already get bilinear upscaling for free if we just out the filters directly
				VkImageBlit request = {};
				request.srcSubresource = { image_to_flip->aspect(), 0, 0, 1 };
				request.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
				request.srcOffsets[0] = { 0, 0, 0 };
				request.srcOffsets[1] = { s32(buffer_width), s32(buffer_height), 1 };
				request.dstOffsets[0] = { 0, 0, 0 };
				request.dstOffsets[1] = { aspect_ratio.width(), aspect_ratio.height(), 1 };

				for (unsigned i = 0; i < calibration_src.size(); ++i)
				{
					const rsx::flags32_t mode = (i == 0) ? UPSCALE_LEFT_VIEW : UPSCALE_RIGHT_VIEW;
					calibration_src[i] = m_upscaler->scale_output(*m_current_command_buffer, image_to_flip, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, request, mode);
				}
			}

			vk::change_image_layout(*m_current_command_buffer, target_image, target_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, subresource_range);
			target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

			const auto key = vk::get_renderpass_key(m_swapchain->get_surface_format());
			single_target_pass = vk::get_renderpass(*m_device, key);
			ensure(single_target_pass != VK_NULL_HANDLE);

			direct_fbo = vk::get_framebuffer(*m_device, m_swapchain_dims.width, m_swapchain_dims.height, VK_FALSE, single_target_pass, m_swapchain->get_surface_format(), target_image);
			direct_fbo->add_ref();

			vk::get_overlay_pass<vk::video_out_calibration_pass>()->run(
				*m_current_command_buffer, areau(aspect_ratio), direct_fbo, calibration_src,
				avconfig.gamma, !use_full_rgb_range_output, avconfig.stereo_enabled, single_target_pass);

			direct_fbo->release();
		}
		else
		{
			// Do raw transfer here as there is no image object associated with textures owned by the driver (TODO)
			VkImageBlit rgn = {};
			rgn.srcSubresource = { image_to_flip->aspect(), 0, 0, 1 };
			rgn.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
			rgn.srcOffsets[0] = { 0, 0, 0 };
			rgn.srcOffsets[1] = { s32(buffer_width), s32(buffer_height), 1 };
			rgn.dstOffsets[0] = { aspect_ratio.x1, aspect_ratio.y1, 0 };
			rgn.dstOffsets[1] = { aspect_ratio.x2, aspect_ratio.y2, 1 };

			if (target_layout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
			{
				vk::change_image_layout(*m_current_command_buffer, target_image, target_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, subresource_range);
				target_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			}

			m_upscaler->scale_output(*m_current_command_buffer, image_to_flip, target_image, target_layout, rgn, UPSCALE_AND_COMMIT | UPSCALE_DEFAULT_VIEW);
		}
	}

	if (g_cfg.video.debug_overlay || has_overlay)
	{
		if (target_layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
		{
			// Change the image layout whilst setting up a dependency on waiting for the blit op to finish before we start writing
			VkImageMemoryBarrier barrier = {};
			barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			barrier.oldLayout = target_layout;
			barrier.image = target_image;
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
			barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barrier.subresourceRange = subresource_range;
			vkCmdPipelineBarrier(*m_current_command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0, nullptr, 0, nullptr, 1, &barrier);

			target_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
		}

		if (!direct_fbo)
		{
			const auto key = vk::get_renderpass_key(m_swapchain->get_surface_format());
			single_target_pass = vk::get_renderpass(*m_device, key);
			ensure(single_target_pass != VK_NULL_HANDLE);

			direct_fbo = vk::get_framebuffer(*m_device, m_swapchain_dims.width, m_swapchain_dims.height, VK_FALSE, single_target_pass, m_swapchain->get_surface_format(), target_image);
		}

		direct_fbo->add_ref();

		render_overlays(direct_fbo, areau(aspect_ratio));

		if (g_cfg.video.debug_overlay)
		{
			const auto num_dirty_textures = m_texture_cache.get_unreleased_textures_count();
			const auto texture_memory_size = m_texture_cache.get_texture_memory_in_use() / (1024 * 1024);
			const auto tmp_texture_memory_size = m_texture_cache.get_temporary_memory_in_use() / (1024 * 1024);
			const auto num_flushes = m_texture_cache.get_num_flush_requests();
			const auto num_mispredict = m_texture_cache.get_num_cache_mispredictions();
			const auto num_speculate = m_texture_cache.get_num_cache_speculative_writes();
			const auto num_misses = m_texture_cache.get_num_cache_misses();
			const auto num_unavoidable = m_texture_cache.get_num_unavoidable_hard_faults();
			const auto cache_miss_ratio = static_cast<u32>(ceil(m_texture_cache.get_cache_miss_ratio() * 100));
			const auto num_texture_upload = m_texture_cache.get_texture_upload_calls_this_frame();
			const auto num_texture_upload_miss = m_texture_cache.get_texture_upload_misses_this_frame();
			const auto texture_upload_miss_ratio = m_texture_cache.get_texture_upload_miss_percentage();
			const auto texture_copies_ellided = m_texture_cache.get_texture_copies_ellided_this_frame();
			const auto vertex_cache_hit_count = (info.stats.vertex_cache_request_count - info.stats.vertex_cache_miss_count);
			const auto vertex_cache_hit_ratio = info.stats.vertex_cache_request_count
				? (vertex_cache_hit_count * 100) / info.stats.vertex_cache_request_count
				: 0;
			const auto program_cache_lookups = info.stats.program_cache_lookups_total;
			const auto program_cache_ellided = info.stats.program_cache_lookups_ellided;
			const auto program_cache_ellision_rate = program_cache_lookups
				? (program_cache_ellided * 100) / program_cache_lookups
				: 0;

			rsx::overlays::set_debug_overlay_text(fmt::format(
				"Internal Resolution:      %s\n"
				"RSX Load:                 %3d%%\n"
				"draw calls: %17d\n"
				"submits: %20d\n"
				"draw call setup: %12dus\n"
				"vertex upload time: %9dus\n"
				"texture upload time: %8dus\n"
				"draw call execution: %8dus\n"
				"submit and flip: %12dus\n"
				"Unreleased textures: %8d\n"
				"Texture cache memory: %7dM\n"
				"Temporary texture memory: %3dM\n"
				"Flush requests: %13d  = %2d (%3d%%) hard faults, %2d unavoidable, %2d misprediction(s), %2d speculation(s)\n"
				"Texture uploads: %12u (%u from CPU - %02u%%, %u copies avoided)\n"
				"Vertex cache hits: %10u/%u (%u%%)\n"
				"Program cache lookup ellision: %u/%u (%u%%)",
				info.stats.framebuffer_stats.to_string(resolution_scaling_config, !backend_config.supports_hw_msaa),
				get_load(), info.stats.draw_calls, info.stats.submit_count, info.stats.setup_time, info.stats.vertex_upload_time,
				info.stats.textures_upload_time, info.stats.draw_exec_time, info.stats.flip_time,
				num_dirty_textures, texture_memory_size, tmp_texture_memory_size,
				num_flushes, num_misses, cache_miss_ratio, num_unavoidable, num_mispredict, num_speculate,
				num_texture_upload, num_texture_upload_miss, texture_upload_miss_ratio, texture_copies_ellided,
				vertex_cache_hit_count, info.stats.vertex_cache_request_count, vertex_cache_hit_ratio,
				program_cache_ellided, program_cache_lookups, program_cache_ellision_rate)
			);
		}

		direct_fbo->release();
	}

	if (target_layout != present_layout)
	{
		vk::change_image_layout(*m_current_command_buffer, target_image, target_layout, present_layout, subresource_range);
	}

	queue_swap_request();

	m_frame_stats.flip_time = m_profiler.duration();

	m_frame->flip(m_context);
	rsx::thread::flip(info);
	interval_trace.complete(swapchain_unavailable, 0);

	// Data sync
	const rsx::surface_scaling_config_t active_res_scaling_config =
	{
		.scale_percent = static_cast<u16>(g_cfg.video.resolution_scale_percent),
		.min_scalable_dimension = static_cast<u16>(g_cfg.video.min_scalable_dimension),
	};

	if (active_res_scaling_config != this->resolution_scaling_config)
	{
		// First, try to reclaim any memory since the res scale upgrade is so memory intensive
		if (const auto severity = vk::vmm_determine_memory_load_severity();
			severity > rsx::problem_severity::low && m_rtts.handle_memory_pressure(*m_current_command_buffer, severity))
		{
			flush_command_queue(true, false, "flip_rescale_memory_pressure_before");
		}

		// Then apply the change
		m_rtts.sync_scaling_config(*m_current_command_buffer, active_res_scaling_config);
		this->resolution_scaling_config = active_res_scaling_config;

		// Finally reclaim any unused resources
		if (const auto severity = vk::vmm_determine_memory_load_severity();
			severity > rsx::problem_severity::low && m_rtts.handle_memory_pressure(*m_current_command_buffer, severity))
		{
			flush_command_queue(true, false, "flip_rescale_memory_pressure_after");
		}
	}
}
