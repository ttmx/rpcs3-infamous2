#pragma once
#include "VKVertexBatchReuse.hpp"
#include <unordered_map>
#include "NativeLateCounterValues.hpp"
#include "NativeTextureCounterValues.hpp"

#include "upscalers/upscaling.h"

#include "vkutils/descriptors.h"
#include "vkutils/data_heap.h"
#include "vkutils/ex.h"
#include "vkutils/instance.h"
#include "vkutils/sync.h"
#include "vkutils/swapchain.h"

#include "VKGSRenderTypes.hpp"
#include "VKTextureCache.h"
#include "VKRenderTargets.h"
#include "VKFormats.h"
#include "VKOverlays.h"
#include "VKProgramBuffer.h"
#include "VKFramebuffer.h"
#include "VKShaderInterpreter.h"
#include "VKQueryPool.h"
#include "VKGeometryCache.hpp"
#include "VKMaterialBindings.hpp"

#include "Emu/RSX/GSRender.h"
#include "Emu/RSX/Host/RSXDMAWriter.h"
#include <functional>
#include <initializer_list>
#include <mutex>

using namespace vk::vmm_allocation_pool_; // clang workaround.
using namespace vk::upscaling_flags_;     // ditto

using vs_binding_table_t = decltype(VKVertexProgram::binding_table);
using fs_binding_table_t = decltype(VKFragmentProgram::binding_table);

namespace vk
{
	using host_data_t = rsx::host_gpu_context_t;
	class timestamp_diagnostics;
}

class VKGSRender : public GSRender, public ::rsx::reports::ZCULL_control
{
private:
	enum frame_context_state : u32
	{
		dirty = 1
	};

	enum flush_queue_state : u32
	{
		ok = 0,
		flushing = 1,
		deadlock = 2
	};

private:
	const VKFragmentProgram *m_fragment_prog = nullptr;
	const VKVertexProgram *m_vertex_prog = nullptr;
	vk::glsl::program *m_program = nullptr;
	vk::glsl::program *m_prev_program = nullptr;
	vk::pipeline_props m_pipeline_properties;

	const vs_binding_table_t* m_vs_binding_table = nullptr;
	const fs_binding_table_t* m_fs_binding_table = nullptr;

	vk::texture_cache m_texture_cache;
	vk::surface_cache m_rtts;

	std::unique_ptr<vk::buffer> null_buffer;
	std::unique_ptr<vk::buffer_view> null_buffer_view;

	std::unique_ptr<vk::upscaler> m_upscaler;
	output_scaling_mode m_output_scaling{output_scaling_mode::bilinear};

	std::unique_ptr<vk::buffer> m_cond_render_buffer;
	u64 m_cond_render_sync_tag = 0;

	shared_mutex m_sampler_mutex;
	atomic_t<bool> m_samplers_dirty = { true };
	std::unique_ptr<vk::sampler> m_stencil_mirror_sampler;
	std::array<vk::sampler*, rsx::limits::fragment_textures_count> fs_sampler_handles{};
	std::array<vk::sampler*, rsx::limits::vertex_textures_count> vs_sampler_handles{};
	vk::material_bindings m_material_bindings;
	u64 m_pipeline_reuse_checked = 0, m_pipeline_reuse_mismatches = 0, m_pipeline_reuse_clears = 0;

	std::unique_ptr<vk::buffer_view> m_persistent_attribute_storage;
	std::unique_ptr<vk::buffer_view> m_volatile_attribute_storage;

	VkDependencyInfoKHR m_async_compute_dependency_info {};
	VkMemoryBarrier2KHR m_async_compute_memory_barrier {};

	std::pair<const vs_binding_table_t*, const fs_binding_table_t*> get_binding_table() const;

public:
	//vk::fbo draw_fbo;
	std::unique_ptr<vk::vertex_cache> m_vertex_cache;
	std::unique_ptr<vk::shader_cache> m_shaders_cache;

private:
	std::unique_ptr<vk::program_cache> m_prog_buffer;

	std::unique_ptr<vk::swapchain_base> m_swapchain;
	vk::instance m_instance;
	vk::render_device *m_device;

	//Vulkan internals
	std::unique_ptr<vk::timestamp_diagnostics> m_gpu_timestamp_diagnostics;
	bool m_draw_attribution_enabled = false;
	void diagnostic_command_begin();
	void diagnostic_frame_end();
	u32 diagnostic_draw_begin(u32 vertices, u32 subdraw, bool indexed, u32 primitive, u32 passes);
	void diagnostic_draw_end(u32 token);
	std::unique_ptr<vk::query_pool_manager> m_occlusion_query_manager;
	bool m_occlusion_query_active = false;
	rsx::reports::occlusion_query_info *m_active_query_info = nullptr;
	std::vector<vk::occlusion_data> m_occlusion_map;

	shared_mutex m_secondary_cb_guard;
	vk::command_pool m_secondary_command_buffer_pool;
	vk::command_buffer_chain<VK_MAX_ASYNC_CB_COUNT> m_secondary_cb_list;

	vk::command_pool m_command_buffer_pool;
	vk::command_buffer_chain<VK_MAX_ASYNC_CB_COUNT> m_primary_cb_list;
	vk::command_buffer_chunk* m_current_command_buffer = nullptr;

	std::unique_ptr<vk::buffer> m_host_object_data;
	vk::framebuffer_holder* m_draw_fbo = nullptr;

	sizeu m_swapchain_dims{};
	bool swapchain_unavailable = false;
	bool should_reinitialize_swapchain = false;

	u64 m_last_heap_sync_time = 0;
	u32 m_texbuffer_view_size = 0;

	vk::data_heap m_attrib_ring_info;                         // Vertex data
	vk::data_heap m_fragment_constants_ring_info;             // Fragment program constants
	vk::data_heap m_transform_constants_ring_info;            // Transform program constants
	vk::data_heap m_fragment_env_ring_info;                   // Fragment environment params
	vk::data_heap m_vertex_env_ring_info;                     // Vertex environment params
	vk::data_heap m_fragment_texture_params_ring_info;        // Fragment texture params
	vk::data_heap m_vertex_layout_ring_info;                  // Vertex layout structure
	vk::data_heap m_index_buffer_ring_info;                   // Index data
	vk::data_heap m_texture_upload_buffer_ring_info;          // Texture upload heap
	vk::data_heap m_raster_env_ring_info;                     // Raster control such as polygon and line stipple
	vk::data_heap m_instancing_buffer_ring_info;              // Instanced rendering data (constants indirection table + instanced constants)

	vk::data_heap m_fragment_instructions_buffer;             // Interpreter FP block
	vk::data_heap m_vertex_instructions_buffer;               // Interpreter VP block

	rsx::simple_array<vk::data_heap*> m_flushable_data_heaps; // List of heaps that can be 'dirty' and need manual flush

	VkDescriptorBufferInfoEx m_vertex_env_buffer_info {};
	VkDescriptorBufferInfoEx m_fragment_env_buffer_info {};
	VkDescriptorBufferInfoEx m_vertex_layout_stream_info {};
	VkDescriptorBufferInfoEx m_vertex_constants_buffer_info {};
	VkDescriptorBufferInfoEx m_fragment_constants_buffer_info {};
	VkDescriptorBufferInfoEx m_fragment_texture_params_buffer_info {};
	VkDescriptorBufferInfoEx m_raster_env_buffer_info {};
	VkDescriptorBufferInfoEx m_instancing_indirection_buffer_info {};
	VkDescriptorBufferInfoEx m_instancing_constants_array_buffer_info{};

	VkDescriptorBufferInfoEx m_vertex_instructions_buffer_info {};
	VkDescriptorBufferInfoEx m_fragment_instructions_buffer_info {};

	rsx::simple_array<u8> m_multidraw_parameters_buffer;
	u64 m_xform_constants_dynamic_offset = 0;          // We manage transform_constants dynamic offset manually to alleviate performance penalty of doing a hot-patch of constants.
	u64 m_vertex_env_dynamic_offset = 0;
	u64 m_vertex_layout_dynamic_offset = 0;
	u64 m_fragment_constants_dynamic_offset = 0;
	u64 m_fragment_env_dynamic_offset = 0;
	u64 m_texture_parameters_dynamic_offset = 0;
	u64 m_stipple_array_dynamic_offset = 0;

	std::unique_ptr<rsx::data_heap::bulk_allocator<256, 96>> m_vertex_env_allocator;
	std::unique_ptr<rsx::data_heap::bulk_allocator<256, 16>> m_transform_constants_allocator;
	std::unique_ptr<rsx::data_heap::bulk_allocator<256, 16>> m_fragment_constants_allocator;

	std::vector<vk::frame_context_t> m_frame_context_storage;
	u32 m_max_async_frames = 0u;
	// Temp frame context to use if the real frame queue is overburdened. Only used for storage
	vk::frame_context_t m_aux_frame_context;

	u32 m_current_queue_index = 0;
	vk::frame_context_t* m_current_frame = nullptr;
	std::deque<vk::frame_context_t*> m_queued_frames;

	VkViewport m_viewport {};
	VkRect2D m_scissor {};

	std::vector<u8> m_draw_buffers;

	// Optional foreign-fault admission. Never acquired by the RSX or DMA offloader.
	std::mutex m_foreign_readback_admission_mutex;
	shared_mutex m_flush_queue_mutex;
	vk::flush_request_task m_flush_requests;

	ullong m_last_cond_render_eval_hint = 0;

	// Default-off bounded submission of a completed late draw prefix. RSX-thread only.
	bool m_draw_prefix_submit_enabled = false;
	bool m_draw_prefix_armed = false;
	bool m_draw_prefix_submitted = false;
	u64 m_draw_prefix_begin_us = 0;
	u32 m_draw_prefix_draws = 0;

	// Default-off periodic submission of completed draws so the GPU can start on a frame
	// while it is still being recorded. RSX-thread only. 0 = disabled.
	u64 m_periodic_submit_us = 0;
	u64 m_last_submit_us = 0;
	u64 m_periodic_submit_count = 0;
	u32 m_periodic_submit_draws = 0;

	// Same-frame vertex upload reuse for multi-block layouts. Purged with the stock vertex cache.
	struct multiblock_vertex_cache_t
	{
		static constexpr u32 slot_count = 8192;
		static constexpr u32 max_entries = 6144;

		struct slot_t
		{
			u64 h1 = 0;
			u64 h2 = 0;
			u32 generation = 0;
			u32 offset = 0;
		};

		std::unique_ptr<slot_t[]> table;
		u32 generation = 1;
		u32 entries = 0;

		// Per-frame hit statistics. Fingerprinting every draw only pays when enough draws repeat.
		u32 lookups = 0;
		u32 hits = 0;
		u32 disabled_frames = 0;

		// Returns the slot holding the fingerprint (found = true) or the free slot to fill, or null when full.
		slot_t* probe(u64 h1, u64 h2, bool& found)
		{
			found = false;
			if (!table) table = std::make_unique<slot_t[]>(slot_count);

			for (u32 index = static_cast<u32>(h1) & (slot_count - 1), checked = 0; checked < 64; checked++, index = (index + 1) & (slot_count - 1))
			{
				auto& slot = table[index];
				if (slot.generation != generation)
				{
					return entries < max_entries ? &slot : nullptr;
				}

				if (slot.h1 == h1 && slot.h2 == h2)
				{
					found = true;
					return &slot;
				}
			}

			return nullptr;
		}

		void clear()
		{
			generation++;
			entries = 0;
		}

		// Experiment only (live control 2 == 3): keep entries across frames with no change detection,
		// to measure the upper bound of a cross-frame cache. Animated geometry will be stale.
		u64 last_offset = 0;

		// Called once per frame
		void end_frame(bool keep_entries = false)
		{
			if (keep_entries)
			{
				lookups = hits = 0;
				disabled_frames = 0;
				if (entries >= max_entries) clear();
				return;
			}

			if (disabled_frames)
			{
				disabled_frames--;
			}
			else if (lookups >= 256 && hits * 2 < lookups)
			{
				// Fewer than half of the draws repeated: skip the cache for a while, then sample again
				disabled_frames = 120;
			}

			lookups = hits = 0;

			if (entries)
			{
				clear();
			}
		}
	} m_multiblock_vertex_cache;

	// Static vertex/index data kept across frames (live control 8)
	vk::geometry_cache m_geometry_cache;

	// Repeat draws consumed straight from the FIFO (live control 9)
	struct fast_draw_t
	{
		bool in_batch = false;

		// Statistics since the last report
		u64 draws = 0;
		u64 batches = 0;
		u64 fallbacks = 0;
		u64 texture_rebinds = 0;
		u64 texture_program_changes = 0;
		u64 texture_checks = 0;
		u64 texture_check_mismatches = 0;
		u64 depth_bias_updates = 0;
		u64 semaphores = 0;
		u64 flow_commands = 0;
		u64 reports = 0;
		u64 not_armed[17]{};
		u64 stops[12]{};
		u32 report_frame = 0;
		mutable u32 blocking_state_bits = 0;
	} m_fast_draw;

	// Diagnostic (live control 9 == 3): state before a draw that the fast path would have taken
	struct fast_draw_verify_t
	{
		bool previous_draw_completed = false;
		bool candidate = false;
		const void* program = nullptr;
		const void* command_buffer = nullptr;
		const void* framebuffer = nullptr;
		VkRenderPass render_pass = VK_NULL_HANDLE;
		vk::pipeline_props pipeline;
		u64 offsets[5]{};
		VkBuffer buffers[6]{};
		const void* views[16]{};
		u32 state = 0;
		u64 checked = 0;
		u64 mismatches[8]{};
	} m_fast_draw_verify;

	void fast_draw_verify_begin();
	void fast_draw_verify_end();

	// Diagnostic: which method ended a run of fast draws (per register)
	std::unique_ptr<u32[]> m_fast_draw_stop_methods;

	u32 fast_draw_blocker(u32 handled_state = 0, u32 handled_flags = 0) const;
	void load_occlusion_task();
	u32 fast_draw_run_blocker() const;
	bool fast_draw_textures_plain() const;
	bool fast_draw_rebind_textures();
	void fast_draw_batch();
	void update_transform_constants_buffer();
	void update_fragment_texture_params_buffer();
	void set_depth_bias_state();

	// Front face and cull mode as set in the current command buffer while they are dynamic state (umax = not set)
	u32 m_dynamic_face_state = umax;
	void set_dynamic_face_state(bool reload);

	// Live control 9: 2 = on, 4 = on with the program of every texture change checked against a full lookup,
	// 5 = on without texture and polygon offset changes inside a run (for comparisons)
	static bool fast_draw_enabled() { const auto mode = vk::live_ctl::get(9); return mode == 2 || mode == 4 || mode == 5 || mode == 6; }
	// Which buffer the persistent vertex stream is bound to: 0 = the ring, 1 = the geometry cache, 2 + n = window n of the SPU upload heap
	u32 m_persistent_source_bound = 0;
	static u32 persistent_source(const vk::vertex_upload_info& info) { return info.spu_window >= 0 ? 2u + info.spu_window : info.static_vertices ? 1u : 0u; }

	// Vertex data that SPU jobs write, appended by their threads (Common/spu_upload.h): a heap and views of its windows
	std::unique_ptr<vk::buffer> m_spu_upload_buffer;
	std::vector<std::unique_ptr<vk::buffer_view>> m_spu_upload_views;
	u64 m_spu_upload_window = 0;
	void create_spu_upload_heap();
	VkDescriptorBufferViewEx persistent_view(const vk::vertex_upload_info& info);
	bool m_native_late_counters_armed = false;
	u64 m_native_late_counter_generation = 0;
	u64 m_native_late_counter_begin_ns = 0;
	native_late_counters::values m_native_late_counter_initial{};
	void diagnostic_begin_native_late_phase();
	void diagnostic_finish_native_late_phase();
	u64 m_native_texture_generation = 0;
	native_texture_counters::values m_native_texture_counters{};
	native_texture_counters::values m_native_texture_initial{};
	std::array<std::array<u32, 18>, 16> m_native_texture_image_inputs{};
	std::array<bool, 16> m_native_texture_image_inputs_valid{};
	native_texture_counters::values* diagnostic_texture_values()
	{
		if (!native_texture_counters::enabled() || !rsx::profiling_timer::native_phase_diagnostics_enabled())
			return nullptr;
		if (m_native_texture_generation != m_native_stats_generation)
		{
			m_native_texture_generation = m_native_stats_generation;
			m_native_texture_counters = {};
		}
		return &m_native_texture_counters;
	}
	void on_frame_end(u32 buffer, bool forced = false) override;

	// Offloader thread deadlock recovery
	rsx::atomic_bitmask_t<flush_queue_state> m_queue_status;
	utils::address_range32 m_offloader_fault_range;
	rsx::invalidation_cause m_offloader_fault_cause;

	vk::draw_call_t m_current_draw {};
	u64 m_current_renderpass_key = 0;
	VkRenderPass m_cached_renderpass = VK_NULL_HANDLE;
	std::vector<vk::image*> m_fbo_images;

	std::unique_ptr<vk::image> m_overlay_recording_img;

	//Vertex layout
	rsx::vertex_input_layout m_vertex_layout;

	vk::shader_interpreter m_shader_interpreter;
	u32 m_interpreter_state;

#if defined(HAVE_X11) && defined(HAVE_VULKAN)
	Display *m_display_handle = nullptr;
#endif

public:
	u64 get_cycles() final;
	~VKGSRender() override;

	VKGSRender(utils::serial* ar) noexcept;
	VKGSRender() noexcept : VKGSRender(nullptr) {}

private:
	void prepare_rtts(rsx::framebuffer_creation_context context);

	void close_and_submit_command_buffer(
		vk::fence* fence = nullptr,
		VkSemaphore wait_semaphore = VK_NULL_HANDLE,
		VkSemaphore signal_semaphore = VK_NULL_HANDLE,
		VkPipelineStageFlags pipeline_stage_flags = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		const char* diagnostic_reason = "direct_unspecified");

	void flush_command_queue(bool hard_sync = false, bool do_not_switch = false, const char* diagnostic_reason = "flush_unspecified");
	void queue_swap_request();
	void maybe_submit_draw_prefix();
	void maybe_periodic_submit();
	void frame_context_cleanup(vk::frame_context_t *ctx);
	void advance_queued_frames();
	void present(vk::frame_context_t *ctx);
	bool reinitialize_swapchain();

	vk::viewable_image* get_present_source(vk::present_surface_info* info, const rsx::avconf& avconfig);

	void begin_render_pass();
	void close_render_pass();
	VkRenderPass get_render_pass();
	void invalidate_render_pass();

	void update_draw_state();
	void check_present_status();

	vk::vertex_upload_info upload_vertex_data();
	rsx::simple_array<u8> m_scratch_mem;

	bool load_program();
	void load_program_env();
	void update_vertex_env(u32 id, const vk::vertex_upload_info& vertex_info);
	void upload_transform_constants(const rsx::io_buffer& buffer);

	void load_texture_env();
	bool bind_texture_env();
	bool bind_interpreter_texture_env();

public:
	void init_buffers(rsx::framebuffer_creation_context context, bool skip_reading = false);
	void set_viewport();
	void set_scissor(bool clip_viewport);
	void bind_viewport();

	// Sync
	void write_barrier(u32 address, u32 range) override;
	void sync_hint(rsx::FIFO::interrupt_hint hint, rsx::reports::sync_hint_payload_t payload) override;
	bool release_GCM_label(u32 type, u32 address, u32 data) override;

	void begin_occlusion_query(rsx::reports::occlusion_query_info* query) override;
	void end_occlusion_query(rsx::reports::occlusion_query_info* query) override;
	bool check_occlusion_query_status(rsx::reports::occlusion_query_info* query) override;
	void get_occlusion_query_result(rsx::reports::occlusion_query_info* query) override;
	void discard_occlusion_query(rsx::reports::occlusion_query_info* query) override;

	// External callback in case we need to suddenly submit a commandlist unexpectedly, e.g in a violation handler
	void emergency_query_cleanup(vk::command_buffer* commands);

	// External callback to handle out of video memory problems
	bool on_vram_exhausted(rsx::problem_severity severity);

	// Handle pool creation failure due to fragmentation
	void on_descriptor_pool_fragmentation(bool is_fatal);

	// Conditional rendering
	void begin_conditional_rendering(const std::vector<rsx::reports::occlusion_query_info*>& sources) override;
	void end_conditional_rendering() override;

	// Host sync object
	std::pair<volatile vk::host_data_t*, VkBuffer> map_host_object_data() const;
	void on_guest_texture_read(const vk::command_buffer& cmd);
	void diagnostic_readback_begin(const vk::command_buffer& cmd, u32 address, u32 length,
		u32 width, u32 height, u32 pitch, u64 cookie, const vk::image* image, bool armed);

	// GRAPH backend
	void patch_transform_constants(rsx::context* ctx, u32 index, u32 count) override;

	// Misc
	bool is_current_program_interpreted() const override;

protected:
	void clear_surface(u32 mask) override;
	void begin() override;
	void end() override;
	void emit_geometry(u32 sub_index) override;

	void on_init_thread() override;
	void on_exit() override;
	void flip(const rsx::display_flip_info_t& info) override;

	void renderctl(u32 request_code, void* args) override;

	void do_local_task(rsx::FIFO::state state) override;
	bool scaled_image_from_memory(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst, bool interpolate) override;
	void notify_tile_unbound(u32 tile) override;

	bool on_access_violation(u32 address, bool is_writing) override;
	void on_invalidate_memory_range(const utils::address_range32 &range, rsx::invalidation_cause cause) override;
	void on_semaphore_acquire_wait() override;
};
