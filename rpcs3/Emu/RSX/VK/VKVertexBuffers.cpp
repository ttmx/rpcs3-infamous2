#include "stdafx.h"
#include "VKLiveCtl.hpp"
#include "VKGSRender.h"
#include "VKReadbackCopy.hpp"
#include "util/sysinfo.hpp"
#include <unordered_map>
#include "Emu/RSX/Common/spu_upload.h"
#include "VKUploadDiagnostics.hpp"
#include "VKGeomTrace.hpp"
#include "../Common/geometry_sync.h"
#include "VKVertexShadowProbe.hpp"
#include "VKVertexBatchReuse.hpp"
#include "../Core/RSXReservationLock.hpp"
#include "Emu/Memory/vm.h"
#include "../Common/BufferUtils.h"
#include "../rsx_methods.h"
#include "vkutils/buffer_object.h"

#include <span>

namespace vk
{
	std::pair<VkPrimitiveTopology, bool> get_appropriate_topology(rsx::primitive_type mode)
	{
		switch (mode)
		{
		case rsx::primitive_type::lines:
			return { VK_PRIMITIVE_TOPOLOGY_LINE_LIST, false };
		case rsx::primitive_type::line_loop:
			return { VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, true };
		case rsx::primitive_type::line_strip:
			return { VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, false };
		case rsx::primitive_type::points:
			return { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, false };
		case rsx::primitive_type::triangles:
			return { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, false };
		case rsx::primitive_type::triangle_strip:
		case rsx::primitive_type::quad_strip:
			return { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, false };
		case rsx::primitive_type::triangle_fan:
#ifndef __APPLE__
			return { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, false };
#endif
		case rsx::primitive_type::quads:
		case rsx::primitive_type::polygon:
			return { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, true };
		default:
			fmt::throw_exception("Unsupported primitive topology 0x%x", static_cast<u8>(mode));
		}
	}

	bool is_primitive_native(rsx::primitive_type mode)
	{
		return !get_appropriate_topology(mode).second;
	}

	VkIndexType get_index_type(rsx::index_array_type type)
	{
		switch (type)
		{
		case rsx::index_array_type::u32:
			return VK_INDEX_TYPE_UINT32;
		case rsx::index_array_type::u16:
			return VK_INDEX_TYPE_UINT16;
		}
		fmt::throw_exception("Invalid index array type (%u)", static_cast<u8>(type));
	}
}

namespace
{
	std::tuple<u32, std::tuple<VkDeviceSize, VkIndexType>> generate_emulating_index_buffer(
		const rsx::draw_clause& clause, u32 vertex_count,
		vk::data_heap& m_index_buffer_ring_info)
	{
		u32 index_count = get_index_count(clause.primitive, vertex_count);
		u32 upload_size = index_count * sizeof(u16);

		VkDeviceSize offset_in_index_buffer = m_index_buffer_ring_info.alloc<256>(upload_size);
		void* buf = m_index_buffer_ring_info.map(offset_in_index_buffer, upload_size);

		g_fxo->get<rsx::dma_manager>().emulate_as_indexed(buf, clause.primitive, vertex_count);

		m_index_buffer_ring_info.unmap();
		return std::make_tuple(
			index_count, std::make_tuple(offset_in_index_buffer, VK_INDEX_TYPE_UINT16));
	}

	struct vertex_input_state
	{
		VkPrimitiveTopology native_primitive_type;
		bool index_rebase;
		u32 min_index;
		u32 max_index;
		u32 vertex_draw_count;
		u32 vertex_index_offset;
		std::optional<std::tuple<VkDeviceSize, VkIndexType>> index_info;
		bool static_indices = false;
		bool spu_indices = false;
	};

	constexpr u32 geometry_cache_vertex_bytes = 64 * 0x100000;
	constexpr u32 geometry_cache_index_bytes = 32 * 0x100000;

	struct draw_command_visitor
	{
		draw_command_visitor(vk::data_heap& index_buffer_ring_info, rsx::vertex_input_layout& layout, vk::geometry_cache* cache, u32 frame, bool spu_indices)
			: m_index_buffer_ring_info(index_buffer_ring_info)
			, m_vertex_layout(layout)
			, m_cache(cache)
			, m_frame(frame)
			, m_spu_indices(spu_indices)
		{
		}

		vertex_input_state operator()(const rsx::draw_array_command& /*command*/)
		{
			const auto [prims, primitives_emulated] = vk::get_appropriate_topology(rsx::method_registers.current_draw_clause.primitive);
			const u32 vertex_count = rsx::method_registers.current_draw_clause.get_elements_count();
			const u32 min_index = rsx::method_registers.current_draw_clause.min_index();
			const u32 max_index = (min_index + vertex_count) - 1;

			if (primitives_emulated)
			{
				u32 index_count;
				std::optional<std::tuple<VkDeviceSize, VkIndexType>> index_info;

				std::tie(index_count, index_info) =
					generate_emulating_index_buffer(rsx::method_registers.current_draw_clause,
						vertex_count, m_index_buffer_ring_info);

				return{ prims, false, min_index, max_index, index_count, 0, index_info };
			}

			return{ prims, false, min_index, max_index, vertex_count, 0, {} };
		}

		vertex_input_state operator()(const rsx::draw_indexed_array_command& command)
		{
			const auto primitive = rsx::method_registers.current_draw_clause.primitive;
			const auto [prims, primitives_emulated] = vk::get_appropriate_topology(primitive);
			const bool emulate_restart = rsx::method_registers.restart_index_enabled() && vk::emulate_primitive_restart(primitive);

			rsx::index_array_type index_type = rsx::method_registers.current_draw_clause.is_immediate_draw ?
				rsx::index_array_type::u32 :
				rsx::method_registers.index_type();

			u32 type_size = get_index_type_size(index_type);

			u32 index_count = rsx::method_registers.current_draw_clause.get_elements_count();
			if (primitives_emulated)
				index_count = get_index_count(primitive, index_count);
			u32 upload_size = index_count * type_size;

			if (emulate_restart) upload_size *= 2;

			// An index list that an SPU job wrote and published already converted (spu_upload.h): nothing to copy
			if (m_spu_indices && index_type == rsx::index_array_type::u16 && !primitives_emulated && !rsx::method_registers.restart_index_enabled() &&
				!rsx::method_registers.current_draw_clause.is_immediate_draw && upload_size >= 2)
			{
				const auto [address, mapped] = vm::try_get_addr(command.raw_index_buffer.data());
				u32 min_index = 0, max_index = 0;

				if (const u64 offset = mapped && u64{address} + upload_size <= rsx::constants::local_mem_base && command.raw_index_buffer.size() >= upload_size
					? rsx::spu_upload::find_indices(address, upload_size, min_index, max_index) : u64{umax}; offset != umax)
				{
					if (rsx::spu_upload::mode() == 2) [[unlikely]]
					{
						// Check: the published list against guest memory
						const u8* twin = rsx::spu_upload::g_state.twin.load() + offset;
						const auto* guest = reinterpret_cast<const u8*>(command.raw_index_buffer.data());
						bool same = true;

						for (u32 i = 0; i + 1 < upload_size && same; i += 2)
						{
							same = twin[i] == guest[i + 1] && twin[i + 1] == guest[i];
						}

						rsx::spu_upload::g_state.index_differing += !same;
					}

					if (min_index > max_index || (min_index == max_index && primitive != rsx::primitive_type::points))
					{
						return{ prims, false, 0, 0, 0, 0, {} };
					}

					return { prims, true, min_index, max_index, index_count, rsx::method_registers.vertex_data_base_index(),
						std::make_tuple(VkDeviceSize{offset}, VK_INDEX_TYPE_UINT16), false, true };
				}
			}

			// Static index data: use the converted copy for as long as its guest pages stay unwritten
			vk::geometry_cache::slot_t* slot = nullptr;
			u32 static_offset = umax;

			if (m_cache && upload_size && !rsx::method_registers.current_draw_clause.is_immediate_draw)
			{
				const auto [address, mapped] = vm::try_get_addr(command.raw_index_buffer.data());

				if (mapped && u64{address} + command.raw_index_buffer.size() <= 0x100000000ull)
				{
					vk::geometry_cache::key_t key;
					key.ranges[0] = { address, ::size32(command.raw_index_buffer) };
					key.range_count = 1;
					key.params = (1u << 31) | static_cast<u32>(index_type) | (static_cast<u32>(primitive) << 8) |
						(u32{rsx::method_registers.restart_index_enabled()} << 16);
					key.restart_index = rsx::method_registers.restart_index_enabled() ? rsx::method_registers.restart_index() : 0;

					rsx::write_watch::sync();

					auto& heap = m_cache->index_heap;
					bool found = false;
					bool promote = false;
					slot = m_cache->indices.probe(key, m_frame, found);
					m_cache->sequence.note(slot, address);
					m_cache->stats.index_requests++;

					if (slot && found)
					{
						const bool has_copy = slot->heap_generation && slot->heap_generation == heap.generation;

						if (has_copy && vk::geometry_cache::is_clean(*slot))
						{
							if (vk::geometry_cache::mode() == 2)
							{
								m_cache->stats.verify_checks++;
								if (vk::geometry_cache::content_hash(key) != m_cache->indices.cold(slot).content)
								{
									m_cache->stats.verify_mismatches++;
									m_cache->suspect(*slot, m_cache->indices.cold(slot), false, m_frame, nullptr);
								}
							}

							slot->last_frame = m_frame;
							m_cache->stats.index_static_hits++;
							m_cache->stats.bytes_reused += upload_size;

							return { prims, true, slot->min_index, slot->max_index, slot->index_count, rsx::method_registers.vertex_data_base_index(),
								std::make_tuple(VkDeviceSize{slot->offset}, vk::get_index_type(index_type)), true };
						}

						auto& cold = m_cache->indices.cold(slot);

						if (has_copy)
						{
							m_cache->stats.index_invalidations++;
							m_cache->invalidated(key, m_frame);
							if (++cold.invalidations >= 3) cold.volatile_until = m_frame + 600;
						}

						slot->heap_generation = 0;
						promote = slot->last_frame != m_frame && m_frame >= cold.volatile_until && m_cache->promotable(key, m_frame);
						slot->last_frame = m_frame;
					}
					else if (slot)
					{
						*slot = {};
						slot->key = key;
						slot->last_frame = m_frame;
						m_cache->indices.cold(slot) = {};
						m_cache->indices.cold(slot).first_frame = m_frame;
					}

					// Tracking starts before the source is read
					if (promote && vk::geometry_cache::watch(key))
					{
						static_offset = heap.alloc(upload_size, 64);

						if (static_offset == umax && vk::geometry_cache::refill(heap, geometry_cache_index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, m_frame))
						{
							m_cache->stats.heap_resets++;
							static_offset = heap.alloc(upload_size, 64);
						}
					}
				}
			}

			const bool to_ring = static_offset == umax;
			VkDeviceSize offset_in_index_buffer = to_ring ? m_index_buffer_ring_info.alloc<64>(upload_size) : static_offset;
			void* buf = to_ring ? m_index_buffer_ring_info.map(offset_in_index_buffer, upload_size) : m_cache->index_heap.mapped + static_offset;

			std::span<std::byte> dst;
			stx::single_ptr<std::byte[]> tmp;
			if (emulate_restart)
			{
				tmp = stx::make_single<std::byte[], false, 64>(upload_size);
				dst = std::span<std::byte>(tmp.get(), upload_size);
			}
			else
			{
				dst = std::span<std::byte>(static_cast<std::byte*>(buf), upload_size);
			}

			/**
			* Upload index (and expands it if primitive type is not natively supported).
			*/
			u32 min_index, max_index;
			std::tie(min_index, max_index, index_count) = write_index_array_data_to_buffer(
				dst,
				command.raw_index_buffer, index_type,
				rsx::method_registers.current_draw_clause.primitive,
				rsx::method_registers.restart_index_enabled(),
				rsx::method_registers.restart_index(),
				[](auto prim) { return !vk::is_primitive_native(prim); });

			if (min_index > max_index ||
				(min_index == max_index && primitive != rsx::primitive_type::points))
			{
				// empty set, do not draw
				if (to_ring) m_index_buffer_ring_info.unmap();
				return{ prims, false, 0, 0, 0, 0, {} };
			}

			if (emulate_restart)
			{
				if (index_type == rsx::index_array_type::u16)
				{
					index_count = rsx::remove_restart_index(static_cast<u16*>(buf), reinterpret_cast<u16*>(tmp.get()), index_count, u16{umax});
				}
				else
				{
					index_count = rsx::remove_restart_index(static_cast<u32*>(buf), reinterpret_cast<u32*>(tmp.get()), index_count, u32{umax});
				}
			}

			if (to_ring)
			{
				m_index_buffer_ring_info.unmap();
			}
			else
			{
				slot->heap_generation = m_cache->index_heap.generation;
				slot->seq = rsx::write_watch::seq();
				slot->offset = static_offset;
				slot->min_index = min_index;
				slot->max_index = max_index;
				slot->index_count = index_count;
				m_cache->indices.cold(slot).content = vk::geometry_cache::content_hash(slot->key);
				m_cache->stats.index_promotions++;
			}

			std::optional<std::tuple<VkDeviceSize, VkIndexType>> index_info =
				std::make_tuple(offset_in_index_buffer, vk::get_index_type(index_type));

			const auto index_offset = rsx::method_registers.vertex_data_base_index();
			return {prims, true, min_index, max_index, index_count, index_offset, index_info, !to_ring};
		}

		vertex_input_state operator()(const rsx::draw_inlined_array& /*command*/)
		{
			auto &draw_clause = rsx::method_registers.current_draw_clause;
			const auto [prims, primitives_emulated] = vk::get_appropriate_topology(draw_clause.primitive);

			const auto stream_length = rsx::method_registers.current_draw_clause.inline_vertex_array.size();
			const u32 vertex_count = u32(stream_length * sizeof(u32)) / m_vertex_layout.interleaved_blocks[0]->attribute_stride;

			if (!primitives_emulated)
			{
				return{ prims, false, 0, vertex_count - 1, vertex_count, 0, {} };
			}

			u32 index_count;
			std::optional<std::tuple<VkDeviceSize, VkIndexType>> index_info;
			std::tie(index_count, index_info) = generate_emulating_index_buffer(draw_clause, vertex_count, m_index_buffer_ring_info);
			return{ prims, false, 0, vertex_count - 1, index_count, 0, index_info };
		}

	private:
		vk::data_heap& m_index_buffer_ring_info;
		rsx::vertex_input_layout& m_vertex_layout;
		vk::geometry_cache* m_cache;
		u32 m_frame;
		bool m_spu_indices;
	};
}

vk::vertex_upload_info VKGSRender::upload_vertex_data()
{
	rsx::geometry_sync::draws++;
	vk::live_ctl::probe_delay(3);

	// Frame number for the geometry cache; never 0, which marks a free slot
	const u32 cache_frame = static_cast<u32>(vk::get_current_frame_id()) + 1;
	vk::geometry_cache* geometry_cache = vk::geometry_cache::mode() ? &m_geometry_cache : nullptr;

	// Live control 23, bit 4: index lists are converted by this thread as before
	const bool spu_indices = m_spu_index_buffer && rsx::spu_upload::mode() != 1 && rsx::spu_upload::mode() != 3 && !(vk::live_ctl::get(23) & 16);
	draw_command_visitor visitor(m_index_buffer_ring_info, m_vertex_layout, geometry_cache, cache_frame, spu_indices);

	if (geometry_cache && geometry_cache->suspect_count)
	{
		rsx::write_watch::sync();
		geometry_cache->report_suspects([&](const vk::geometry_cache::suspect_t& entry, bool tracked)
		{
			rsx_log.error("Geometry cache: %s data at 0x%x+%u (%u ranges) differed from its copy in frame %u (now %u): %s; "
				"%u bytes differ, first at 0x%x; equal again right after: %d; copied at scan %u, checked at scan %u, first seen in frame %u; "
				"last sync point kind %u, %u draws before the check, params 0x%x, surfaces 0x%x/0x%x; sync counters put %u self-jump %u semaphore %u empty %u",
				entry.vertex ? "vertex" : "index", entry.key.ranges[0].address, entry.key.ranges[0].length, entry.key.range_count, entry.frame, cache_frame,
				tracked ? "the write was tracked and is seen by the next scan" : "NO tracked write",
				entry.differing_bytes, entry.first_difference, +entry.rehash_equal, entry.created_seq, entry.seq, entry.first_frame,
				entry.last_kind, entry.draws_since_point, entry.params, m_surface_info[0].address, m_depth_surface_info.address,
				static_cast<u32>(rsx::geometry_sync::counters[rsx::geometry_sync::put_changed]), static_cast<u32>(rsx::geometry_sync::counters[rsx::geometry_sync::self_jump]),
				static_cast<u32>(rsx::geometry_sync::counters[rsx::geometry_sync::semaphore]), static_cast<u32>(rsx::geometry_sync::counters[rsx::geometry_sync::fifo_empty]));
		});
	}
	const auto draw_command = m_draw_processor.get_draw_command(rsx::method_registers);
	auto result = std::visit(visitor, draw_command);

	const u32 vertex_count = (result.max_index - result.min_index) + 1;
	u32 vertex_base = result.min_index;
	u32 index_base = 0;

	if (result.index_rebase)
	{
		vertex_base = rsx::get_index_from_base(vertex_base, rsx::method_registers.vertex_data_base_index());
		index_base = result.min_index;
	}

	//Do actual vertex upload
	auto required = calculate_memory_requirements(m_vertex_layout, vertex_base, vertex_count);
	u32 persistent_range_base = -1, volatile_range_base = -1;
	usz persistent_offset = -1, volatile_offset = -1;

	// Observe every attribute allocation, even draws outside native batch eligibility.
	const auto note_batch_allocation = [&](usz offset, usz requested)
	{
		if (!vk::vertex_batch_reuse::enabled()) return;
		const auto* upload = m_attrib_ring_info.diagnostic_upload_buffer();
		vk::vertex_batch_reuse::instance().allocation(vk::get_current_frame_id(),
			m_attrib_ring_info.heap ? m_attrib_ring_info.heap->uid() : 0,
			upload ? upload->uid() : 0, offset, utils::align(requested, 256));
	};

	bool static_vertices = false;
	bool geometry_cache_used = false;
	bool geometry_promote = false;
	vk::geometry_cache::slot_t* geometry_slot = nullptr;
	s8 spu_window = -1;

	// Written by SPU jobs and already in the SPU upload heap (Common/spu_upload.h): nothing to copy. A draw that has
	// such a block and others that no job wrote (a static stream of the model) gets those copied into the heap by this
	// thread, so that all of its blocks are in one window; the layout then points at each block where it is.
	std::array<u32, 8> spu_block_offsets{};

	if (!m_spu_upload_buffer && rsx::spu_upload::g_state.wanted.load(std::memory_order_relaxed) && !m_spu_upload_window) [[unlikely]]
	{
		m_spu_upload_window = 1; // tried
		create_spu_upload_heap();
	}

	if (required.first > 0 && m_spu_upload_buffer && rsx::spu_upload::mode() != 1 && rsx::spu_upload::mode() != 3)
	{
		auto& stats = rsx::spu_upload::g_state;
		const auto& blocks = m_vertex_layout.interleaved_blocks;

		if (blocks.size() <= spu_block_offsets.size() && rsx::method_registers.current_draw_clause.command != rsx::draw_command::inlined_array)
		{
			struct span_t { u32 address, length; u64 offset; };
			std::array<span_t, 8> spans;
			u32 total = 0, found = 0, appended_blocks = 0, appended_bytes = 0;
			bool usable = true;

			for (u32 i = 0; i < blocks.size(); i++)
			{
				const auto range = blocks[i]->calculate_required_range(vertex_base, vertex_count);
				const u64 address = u64{blocks[i]->real_offset_address} + u64{range.first} * blocks[i]->attribute_stride;
				const u32 length = range.second * blocks[i]->attribute_stride;

				if (!length || address + length > 0x100000000ull)
				{
					usable = false;
					break;
				}

				spans[i] = {static_cast<u32>(address), length, address + length <= rsx::constants::local_mem_base ? rsx::spu_upload::find(static_cast<u32>(address), length) : u64{umax}};
				found += spans[i].offset != umax;
				total += length;
			}

			usable = usable && found && total == required.first;
			s64 window = -1;

			for (u32 i = 0; usable && i < blocks.size(); i++)
			{
				auto& span = spans[i];

				if (span.offset == umax)
				{
					span.offset = rsx::spu_upload::append(vm::get_super_ptr<const u8>(span.address), span.length, [](u8* to, const void* from, u32 bytes)
					{
						if (bytes >= 1024 && utils::has_avx512())
						{
							vk::readback_copy::stream(to, from, bytes);
						}
						else
						{
							std::memcpy(to, from, bytes);
						}
					});

					appended_blocks++;
					appended_bytes += span.length;
				}

				if (span.offset == umax || (window >= 0 && static_cast<s64>(span.offset / m_spu_upload_window) != window))
				{
					usable = false;
					break;
				}

				if (rsx::spu_upload::mode() == 2)
				{
					stats.verified++;
					if (std::memcmp(stats.mapped.load() + span.offset, vm::get_super_ptr<const u8>(span.address), span.length)) stats.differing++;
				}

				window = static_cast<s64>(span.offset / m_spu_upload_window);
				spu_block_offsets[i] = static_cast<u32>(span.offset % m_spu_upload_window);
			}

			if (usable)
			{
				spu_window = static_cast<s8>(window);
				persistent_range_base = 0;
				stats.hits++;
				stats.hit_bytes += total;
				stats.appended += appended_blocks;
				stats.appended_bytes += appended_bytes;
			}
			else
			{
				stats.miss_bytes += required.first;
			}
		}
		else
		{
			stats.multi_block++;
			stats.miss_bytes += required.first;
		}

		if (static u64 printed = 0; vk::get_current_frame_id() - printed >= 300)
		{
			printed = vk::get_current_frame_id();
			rsx::spu_upload::print_stats();
		}
	}

	if (required.first > 0 && spu_window < 0)
	{
		//Check if cacheable
		//Only data in the 'persistent' block may be cached
		//TODO: make vertex cache keep local data beyond frame boundaries and hook notify command
		bool in_cache = false;
		bool to_store = false;
		u32  storage_address = -1;

		m_frame_stats.vertex_cache_request_count++;

		// Static vertex data: the packed persistent stream is a function of the ordered source ranges only
		if (geometry_cache && m_vertex_layout.interleaved_blocks.size() <= vk::geometry_cache::max_ranges &&
			rsx::method_registers.current_draw_clause.command != rsx::draw_command::inlined_array)
		{
			vk::geometry_cache::key_t key;
			u64 packed = 0;
			bool cacheable = true;

			for (auto* block : m_vertex_layout.interleaved_blocks)
			{
				const auto range = block->calculate_required_range(vertex_base, vertex_count);
				const u64 address = u64{block->real_offset_address} + u64{range.first} * block->attribute_stride;
				const u64 length = u64{range.second} * block->attribute_stride;

				if (!length || address + length > 0x100000000ull)
				{
					cacheable = false;
					break;
				}

				key.ranges[key.range_count++] = { static_cast<u32>(address), static_cast<u32>(length) };
				packed += length;
			}

			if (cacheable && packed == required.first)
			{
				geometry_cache_used = true;
				rsx::write_watch::sync();

				auto& heap = geometry_cache->vertex_heap;
				bool found = false;
				geometry_slot = geometry_cache->vertices.probe(key, cache_frame, found);
				geometry_cache->sequence.note(geometry_slot, key.ranges[0].address);
				geometry_cache->stats.vertex_requests++;

				if (geometry_slot && found)
				{
					auto* slot = geometry_slot;
					const bool has_copy = slot->heap_generation && slot->heap_generation == heap.generation;

					if (has_copy && vk::geometry_cache::is_clean(*slot))
					{
						if (vk::geometry_cache::mode() == 2)
						{
							geometry_cache->stats.verify_checks++;
							if (vk::geometry_cache::content_hash(key) != geometry_cache->vertices.cold(slot).content)
							{
								geometry_cache->stats.verify_mismatches++;
								geometry_cache->suspect(*slot, geometry_cache->vertices.cold(slot), true, cache_frame, heap.mapped + slot->offset);
							}
						}

						in_cache = true;
						static_vertices = true;
						persistent_range_base = slot->offset;
						geometry_cache->stats.vertex_static_hits++;
						geometry_cache->stats.bytes_reused += required.first;
					}
					else
					{
						auto& cold = geometry_cache->vertices.cold(slot);

						if (has_copy)
						{
							geometry_cache->stats.vertex_invalidations++;
							geometry_cache->invalidated(key, cache_frame);
							if (++cold.invalidations >= 3) cold.volatile_until = cache_frame + 600;
						}

						slot->heap_generation = 0;

						if (cold.ring_frame == cache_frame && cold.ring_generation == m_attrib_ring_info.generation())
						{
							// Copied earlier in this frame; same trust as the stock per-frame vertex cache
							in_cache = true;
							persistent_range_base = cold.ring_offset;
							geometry_cache->stats.vertex_ring_hits++;
						}
						else
						{
							geometry_promote = slot->last_frame != cache_frame && cache_frame >= cold.volatile_until && geometry_cache->promotable(key, cache_frame);
						}
					}

					slot->last_frame = cache_frame;
				}
				else if (geometry_slot)
				{
					*geometry_slot = {};
					geometry_slot->key = key;
					geometry_slot->last_frame = cache_frame;
					geometry_cache->vertices.cold(geometry_slot) = {};
					geometry_cache->vertices.cold(geometry_slot).first_frame = cache_frame;
				}
			}
		}

		// Same-frame reuse for multi-block layouts (live control 2). Same trust model as the stock
		// single-block weak cache below: identical source ranges and layout within a frame are not re-copied.
		// Entries are identified by a 128-bit fingerprint of the ordered block and attribute layout.
		multiblock_vertex_cache_t::slot_t* multiblock_slot = nullptr;
		u64 multiblock_h1 = 0, multiblock_h2 = 0;

		if (!geometry_cache_used && vk::live_ctl::get(2) && (!m_multiblock_vertex_cache.disabled_frames || vk::live_ctl::get(2) == 2) &&
			m_vertex_layout.interleaved_blocks.size() > 1 && m_vertex_layout.interleaved_blocks.size() <= 16 &&
			rsx::method_registers.current_draw_clause.command != rsx::draw_command::inlined_array)
		{
			u64 h1 = 0x9e3779b97f4a7c15ull, h2 = 0xc2b2ae3d27d4eb4full;
			const auto mix = [&](u64 value)
			{
				h1 = (h1 ^ value) * 0x100000001b3ull;
				h1 ^= h1 >> 29;
				h2 = (std::rotl(h2, 23) + value) * 0xff51afd7ed558ccdull;
			};

			mix(u64(rsx::method_registers.current_draw_clause.command) | (u64(m_vertex_layout.attribute_mask) << 8) | (u64(m_vertex_layout.interleaved_blocks.size()) << 40));

			u64 packed = 0;
			bool cacheable = true;

			for (auto* block : m_vertex_layout.interleaved_blocks)
			{
				const auto range = block->calculate_required_range(vertex_base, vertex_count);
				const u64 address = u64(block->real_offset_address) + u64(range.first) * block->attribute_stride;
				const u64 length = u64(range.second) * block->attribute_stride;

				if (!length || address + length > 0x100000000ull)
				{
					cacheable = false;
					break;
				}

				mix(address | (length << 32));
				mix(u64(block->base_offset) | (u64(block->attribute_stride) << 32) | (u64(block->memory_location) << 40) |
					(u64(block->single_vertex) << 48) | (u64(block->interleaved) << 49) | (u64(block->locations.size()) << 52));

				for (const auto& attr : block->locations)
				{
					mix(u64(attr.index) | (u64(attr.frequency) << 8) | (u64(attr.modulo) << 40));
				}

				packed += length;
			}

			if (cacheable && packed == required.first)
			{
				multiblock_h1 = h1;
				multiblock_h2 = h2;

				bool found = false;
				multiblock_slot = m_multiblock_vertex_cache.probe(h1, h2, found);
				m_multiblock_vertex_cache.lookups++;

				if (found)
				{
					m_multiblock_vertex_cache.hits++;
					in_cache = true;
					persistent_range_base = multiblock_slot->offset;
					multiblock_slot = nullptr;
				}
			}
		}

		if (!geometry_cache_used && m_vertex_layout.interleaved_blocks.size() == 1 &&
			rsx::method_registers.current_draw_clause.command != rsx::draw_command::inlined_array)
		{
			const auto data_offset = (vertex_base * m_vertex_layout.interleaved_blocks[0]->attribute_stride);
			storage_address = m_vertex_layout.interleaved_blocks[0]->real_offset_address + data_offset;

			if (auto cached = m_vertex_cache->find_vertex_range(storage_address, required.first))
			{
				ensure(cached->local_address == storage_address);

				in_cache = true;
				persistent_range_base = cached->offset_in_heap;
			}
			else
			{
				to_store = true;
			}
		}

		u32 static_offset = umax;

		// Promotion to the persistent buffer. Tracking starts before the source is read.
		if (!in_cache && geometry_promote && vk::geometry_cache::watch(geometry_slot->key))
		{
			auto& heap = geometry_cache->vertex_heap;
			static_offset = heap.alloc(required.first, 256);

			if (static_offset == umax && vk::geometry_cache::refill(heap, std::min<u32>(geometry_cache_vertex_bytes, m_texbuffer_view_size),
				VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT, cache_frame))
			{
				geometry_cache->stats.heap_resets++;
				static_offset = heap.alloc(required.first, 256);
			}

			if (static_offset != umax)
			{
				m_frame_stats.vertex_cache_miss_count++;
				m_draw_processor.write_vertex_data_to_memory(m_vertex_layout, vertex_base, vertex_count, heap.mapped + static_offset, nullptr);

				geometry_slot->heap_generation = heap.generation;
				geometry_slot->seq = rsx::write_watch::seq();
				geometry_slot->offset = static_offset;
				geometry_cache->vertices.cold(geometry_slot).content = vk::geometry_cache::content_hash(geometry_slot->key);
				geometry_cache->stats.vertex_promotions++;

				// Nothing is left to write for the persistent stream
				in_cache = true;
				static_vertices = true;
				persistent_range_base = static_offset;
			}
		}

		if (!in_cache)
		{
			m_frame_stats.vertex_cache_miss_count++;

			persistent_offset = static_cast<u32>(m_attrib_ring_info.alloc<256>(required.first));

			if (geometry_slot)
			{
				auto& cold = geometry_cache->vertices.cold(geometry_slot);
				cold.ring_frame = cache_frame;
				cold.ring_generation = m_attrib_ring_info.generation();
				cold.ring_offset = static_cast<u32>(persistent_offset);
			}
			note_batch_allocation(persistent_offset, required.first);

			// Entries kept across frames are only valid until the ring wraps over them
			if (persistent_offset < m_multiblock_vertex_cache.last_offset && m_multiblock_vertex_cache.entries)
			{
				m_multiblock_vertex_cache.clear();
				multiblock_slot = nullptr;
			}
			m_multiblock_vertex_cache.last_offset = persistent_offset;
			persistent_range_base = static_cast<u32>(persistent_offset);

			if (to_store)
			{
				//store ref in vertex cache
				m_vertex_cache->store_range(storage_address, required.first, static_cast<u32>(persistent_offset));
			}

			if (multiblock_slot)
			{
				multiblock_slot->h1 = multiblock_h1;
				multiblock_slot->h2 = multiblock_h2;
				multiblock_slot->generation = m_multiblock_vertex_cache.generation;
				multiblock_slot->offset = static_cast<u32>(persistent_offset);
				m_multiblock_vertex_cache.entries++;
			}
		}
	}

	if (required.second > 0)
	{
		volatile_offset = static_cast<u32>(m_attrib_ring_info.alloc<256>(required.second));
		note_batch_allocation(volatile_offset, required.second);
		volatile_range_base = static_cast<u32>(volatile_offset);
	}

	if (vk::geom_trace::active(vk::get_current_frame_id()))
	{
		auto& trace = vk::geom_trace::state();
		const auto frame = vk::get_current_frame_id();
		const auto draw = trace.draw++;
		if (static u64 last_frame = umax; last_frame != frame)
		{
			last_frame = frame;
			const auto& c = rsx::geometry_sync::counters;
			std::fprintf(trace.file, "S %llu %llu %llu %llu %llu %llu %llu %llu\n", static_cast<unsigned long long>(frame), c[0], c[1], c[2], c[3], c[4], c[5], c[6]);
		}
		std::fprintf(trace.file, "E %llu\n", rsx::geometry_sync::epoch);
		{
			// FIFO words dispatched since the previous traced draw
			static std::vector<std::pair<unsigned, unsigned>> fifo_words;
			if (!rsx::geometry_sync::fifo_log)
			{
				rsx::geometry_sync::fifo_log = &fifo_words;
			}
			else
			{
				std::fputs("F", trace.file);
				for (const auto& [reg, value] : fifo_words) std::fprintf(trace.file, " %x:%x", reg, value);
				std::fputc('\n', trace.file);
			}
			fifo_words.clear();
		}
		{
			// Register and transform-constant changes since the previous traced draw
			static std::array<u32, 0x10000 / 4> last_registers{};
			static std::array<u32[4], 512> last_constants{};
			const auto& regs = rsx::method_registers.registers;
			u32 changed = 0;
			std::fputs("R", trace.file);
			for (u32 i = 0; i < regs.size(); i++)
			{
				if (regs[i] != last_registers[i])
				{
					if (changed++ < 96) std::fprintf(trace.file, " %x:%x", i * 4, regs[i]);
					last_registers[i] = regs[i];
				}
			}
			std::fprintf(trace.file, " n=%u\nC", changed);
			changed = 0;
			for (u32 i = 0; i < 512; i++)
			{
				if (std::memcmp(rsx::method_registers.transform_constants[i], last_constants[i], 16))
				{
					std::memcpy(last_constants[i], rsx::method_registers.transform_constants[i], 16);
					const u32* v = rsx::method_registers.transform_constants[i];
					if (changed++ < 40) std::fprintf(trace.file, " %u:%08x,%08x,%08x,%08x", i, v[0], v[1], v[2], v[3]);
				}
			}
			std::fprintf(trace.file, " n=%u\n", changed);

			// Each distinct vertex program once
			if (!trace.dump_dir.empty())
			{
				const auto& vp = current_vertex_program;
				const u64 h = vk::geom_trace::hash(vp.data.data(), vp.data.size() * 4);
				if (trace.dumped.insert(h).second)
				{
					char name[64];
					std::snprintf(name, sizeof(name), "/vp-%016llx.bin", static_cast<unsigned long long>(h));
					if (FILE* f = std::fopen((trace.dump_dir + name).c_str(), "wb"))
					{
						std::fwrite(vp.data.data(), 4, vp.data.size(), f);
						std::fclose(f);
					}
				}
			}
		}
		const auto& clause = rsx::method_registers.current_draw_clause;
		u32 index_address = 0, index_bytes = 0;
		u64 index_hash = 0;
		if (const auto* indexed = std::get_if<rsx::draw_indexed_array_command>(&draw_command); indexed && !clause.is_immediate_draw)
		{
			index_address = vm::try_get_addr(indexed->raw_index_buffer.data()).first;
			index_bytes = ::size32(indexed->raw_index_buffer);
			index_hash = vk::geom_trace::hash(indexed->raw_index_buffer.data(), index_bytes);
			vk::geom_trace::dump('i', frame, index_address, indexed->raw_index_buffer.data(), index_bytes, index_hash);
		}
		const auto& program = current_vertex_program;
		std::fprintf(trace.file, "D %llu %llu %u %u %u %u %u %u %u %x %u %llx %llx %x %x %x %u %u %u %u %x\n",
			static_cast<unsigned long long>(frame), static_cast<unsigned long long>(draw), static_cast<u32>(clause.command), static_cast<u32>(clause.primitive),
			vertex_base, vertex_count, result.vertex_draw_count, index_base, static_cast<u32>(rsx::method_registers.index_type()),
			index_address, index_bytes, static_cast<unsigned long long>(index_hash),
			static_cast<unsigned long long>(vk::geom_trace::hash(program.data.data(), program.data.size() * 4)), program.output_mask,
			m_surface_info[0].address, m_depth_surface_info.address, ::size32(m_vertex_layout.interleaved_blocks),
			required.first, required.second, static_cast<u32>(clause.is_immediate_draw), rsx::method_registers.vertex_data_base_offset());
		u32 ordinal = 0;
		if (required.first > 0) for (auto* block : m_vertex_layout.interleaved_blocks)
		{
			const auto range = block->calculate_required_range(vertex_base, vertex_count);
			const u32 address = block->real_offset_address + range.first * block->attribute_stride;
			const u32 length = range.second * block->attribute_stride;
			u64 content = 0;
			if (length && vm::check_addr(address, vm::page_readable, length))
			{
				content = vk::geom_trace::hash(vm::base(address), length);
				vk::geom_trace::dump('v', frame, address, vm::base(address), length, content);
			}
			std::fprintf(trace.file, "B %llu %llu %u %x %u %u %u %x %u %u %llx ", static_cast<unsigned long long>(frame), static_cast<unsigned long long>(draw),
				ordinal++, address, length, u32(block->attribute_stride), u32(block->memory_location), block->base_offset,
				u32(block->interleaved), u32(block->single_vertex), static_cast<unsigned long long>(content));
			for (const auto& attr : block->locations)
			{
				const auto& info = rsx::method_registers.vertex_arrays_info[attr.index];
				std::fprintf(trace.file, "%u:%u:%u:%x:%u:%u;", u32(attr.index), static_cast<u32>(info.type()), u32(info.size()), info.offset(), u32(attr.frequency), u32(attr.modulo));
			}
			std::fputc('\n', trace.file);
		}
	}

	// Capture exact ordered layout/source tuples. This does not inspect or reuse VM bytes.
	if (vk::upload_diagnostics::active())
	{
		static u64 diagnostic_draw = 0;
		const auto draw = ++diagnostic_draw;
		const auto frame = vk::get_current_frame_id();
		const auto stamp = vk::upload_diagnostics::now_ns();
		const auto* upload_heap = m_attrib_ring_info.diagnostic_upload_buffer();
		const auto heap_uid = upload_heap ? upload_heap->uid() : 0;
		const auto type = upload_heap && upload_heap->memory ? upload_heap->memory->diagnostic_memory_type() : ~0u;
		const auto flags = type < VK_MAX_MEMORY_TYPES ? m_device->gpu().get_memory_properties().memoryTypes[type].propertyFlags : 0;
		const bool eligible = m_vertex_layout.interleaved_blocks.size() == 1 &&
			rsx::method_registers.current_draw_clause.command != rsx::draw_command::inlined_array;
		const bool hit = required.first && persistent_offset == umax;
		const u32 copied = required.first && persistent_offset != umax ? required.first : 0;
		std::ostringstream out;
		out << "vertex," << stamp << ',' << frame << ',' << draw << ",0,0,0,0,0," << m_vertex_layout.attribute_mask
			<< ",," << heap_uid << ',' << type << ',' << flags << ',' << required.first << ',' << required.second
			<< ',' << copied << ',' << copied << ',' << required.second << ',' << eligible << ',' << hit << ','
			<< static_cast<u32>(rsx::method_registers.current_draw_clause.command) << ',' << bool(g_cfg.video.strict_rendering_mode)
			<< ',' << bool(g_cfg.video.multithreaded_rsx) << ",0\n";
		u32 ordinal = 0;
		if (required.first > 0) for (auto* block : m_vertex_layout.interleaved_blocks)
		{
			const auto range = block->calculate_required_range(vertex_base, vertex_count);
			const u32 address = block->real_offset_address + range.first * block->attribute_stride;
			const u32 length = range.second * block->attribute_stride;
			out << "span," << stamp << ',' << frame << ',' << draw << ',' << ordinal++ << ',' << address << ',' << length
				<< ',' << u32(block->attribute_stride) << ',' << u32(block->memory_location) << ',' << m_vertex_layout.attribute_mask << ',';
			out << block->interleaved << ":" << block->single_vertex << ":" << block->base_offset << "|";
			for (const auto& attr : block->locations) out << u32(attr.index) << ':' << attr.frequency << ':' << attr.modulo << ';';
			out << ',' << heap_uid << ',' << type << ',' << flags << ",0,0,0,0,0,0,0,0,0,0,0\n";
		}
		vk::upload_diagnostics::writer().write(out.str(), 1 + ordinal);
	}

	// Diagnostic-only: observe the stock copied CPU upload bytes before unmap.
	const auto observe_packed_persistent = [&](const void* mapped_data)
	{
		if (!vk::vertex_shadow_probe::enabled() || !required.first ||
			m_vertex_layout.interleaved_blocks.size() <= 1 ||
			rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array ||
			!vk::vertex_shadow_probe::instance().ready()) return;
		const auto* upload = m_attrib_ring_info.diagnostic_upload_buffer();
		const auto type = upload && upload->memory ? upload->memory->diagnostic_memory_type() : ~0u;
		const auto flags = type < VK_MAX_MEMORY_TYPES ? m_device->gpu().get_memory_properties().memoryTypes[type].propertyFlags : 0;
		std::ostringstream tuple;
		tuple << static_cast<u32>(rsx::method_registers.current_draw_clause.command) << ':' << m_vertex_layout.attribute_mask << '|';
		for (auto* block : m_vertex_layout.interleaved_blocks)
		{
			const auto range = block->calculate_required_range(vertex_base, vertex_count);
			tuple << block->real_offset_address + range.first * block->attribute_stride << ':'
				<< range.second * block->attribute_stride << ':' << u32(block->attribute_stride) << ':'
				<< u32(block->memory_location) << ':' << block->base_offset << ':'
				<< block->interleaved << ':' << block->single_vertex << '[';
			for (const auto& attr : block->locations) tuple << u32(attr.index) << ':' << attr.frequency << ':' << attr.modulo << ';';
			tuple << "]|";
		}
		vk::vertex_shadow_probe::instance().observe(vk::get_current_frame_id(),
			m_attrib_ring_info.heap ? m_attrib_ring_info.heap->uid() : 0,
			upload ? upload->uid() : 0, flags, bool(g_cfg.video.strict_rendering_mode),
			bool(g_cfg.video.multithreaded_rsx), persistent_offset, tuple.str(), mapped_data, required.first);
	};

	const auto write_vertex_stage = [&](void* persistent, void* transient)
	{
		if (!vk::vertex_batch_reuse::enabled() || !persistent || !required.first || required.first < vk::vertex_batch_reuse::minimum_bytes() ||
			m_vertex_layout.interleaved_blocks.size() <= 1 || m_vertex_layout.interleaved_blocks.size() > 16 ||
			rsx::method_registers.current_draw_clause.command == rsx::draw_command::inlined_array)
		{
			m_draw_processor.write_vertex_data_to_memory(m_vertex_layout, vertex_base, vertex_count, persistent, transient);
			if (persistent) observe_packed_persistent(persistent);
			return;
		}
		const auto* upload = m_attrib_ring_info.diagnostic_upload_buffer();
		const auto type = upload && upload->memory ? upload->memory->diagnostic_memory_type() : ~0u;
		const auto flags = type < VK_MAX_MEMORY_TYPES ? m_device->gpu().get_memory_properties().memoryTypes[type].propertyFlags : 0;
		const bool eligible = vk::vertex_batch_reuse::enabled() && persistent && required.first &&
			m_vertex_layout.interleaved_blocks.size() > 1 && m_vertex_layout.interleaved_blocks.size() <= 16 &&
			rsx::method_registers.current_draw_clause.command != rsx::draw_command::inlined_array &&
			bool(g_cfg.video.strict_rendering_mode) && !bool(g_cfg.video.multithreaded_rsx) &&
			!m_attrib_ring_info.has_shadow() && m_attrib_ring_info.heap && upload && upload->memory &&
			(flags & 14) == 14 && persistent_offset <= m_attrib_ring_info.size() &&
			required.first <= m_attrib_ring_info.size() - persistent_offset;
		vk::vertex_batch_reuse::key key;
		bool valid_key = eligible;
		if (eligible)
		{
			key.command = static_cast<u32>(rsx::method_registers.current_draw_clause.command);
			key.attribute_mask = m_vertex_layout.attribute_mask;
			key.block_count = ::size32(m_vertex_layout.interleaved_blocks);
			u64 packed = 0;
			for (u32 i = 0; i < key.block_count; ++i)
			{
				auto* block = m_vertex_layout.interleaved_blocks[i];
				const auto range = block->calculate_required_range(vertex_base, vertex_count);
				const u64 address = u64(block->real_offset_address) + u64(range.first) * block->attribute_stride;
				const u64 length = u64(range.second) * block->attribute_stride;
				if (!length || address > UINT32_MAX || length > UINT32_MAX || address + length > 0x100000000ull ||
					key.attribute_count + block->locations.size() > 16)
				{
					valid_key = false;
					break;
				}
				key.blocks[i] = {u32(address), u32(length), block->base_offset,
					u32(block->attribute_stride) | (u32(block->memory_location) << 8) | (u32(block->single_vertex) << 16) |
					(u32(block->interleaved) << 17) | (::size32(block->locations) << 24)};
				for (const auto& attr : block->locations)
					key.attributes[key.attribute_count++] = u32(attr.index) | (u32(attr.frequency) << 8) | (u32(attr.modulo) << 24);
				packed += length;
			}
			valid_key &= packed == required.first;
		}
		if (!valid_key)
		{
			m_draw_processor.write_vertex_data_to_memory(m_vertex_layout, vertex_base, vertex_count, persistent, transient);
			if (persistent) observe_packed_persistent(persistent);
			return;
		}
		// Keep stock transient-before-persistent order. All original allocations already occurred.
		if (transient) m_draw_processor.write_vertex_data_to_memory(m_vertex_layout, vertex_base, vertex_count, nullptr, transient);
		auto& cache = vk::vertex_batch_reuse::instance();
		const auto sync_scope = [&]
		{
			const auto* current_upload = m_attrib_ring_info.diagnostic_upload_buffer();
			cache.scope(vk::get_current_frame_id(), m_attrib_ring_info.heap->uid(), current_upload->uid());
		};
		sync_scope();
		const auto before_copy = cache.generation();
		const auto span_copy = [&](u32 index, const std::byte* expected, std::byte* destination, usz length)
		{
			auto* source = vm::_ptr<char>(key.blocks[index].address);
			const u32 vm_address = vm::try_get_addr(source).first;
			rsx::reservation_lock<true, 1> lock(vm_address, static_cast<u32>(length), g_cfg.video.strict_rendering_mode && vm_address);
			if (expected && std::memcmp(expected, source, length) == 0) return true;
			std::memcpy(destination, source, length);
			return false;
		};
		const auto outcome = cache.attempt(key, persistent, required.first, persistent_offset, span_copy, sync_scope);
		if (!outcome.handled)
			m_draw_processor.write_vertex_data_to_memory(m_vertex_layout, vertex_base, vertex_count, persistent, nullptr);
		if (outcome.reused)
		{
			persistent_range_base = static_cast<u32>(outcome.absolute_offset);
			static std::atomic_flag reported{};
			if (!reported.test_and_set(std::memory_order_relaxed)) rsx_log.notice("Exact native multi-block vertex upload reuse active");
			// Any optional byte diagnostic observes the actual reused GPU input while its direct mapping is valid.
			const auto mapped_base = reinterpret_cast<uptr>(persistent) - persistent_offset;
			observe_packed_persistent(reinterpret_cast<const void*>(mapped_base + outcome.absolute_offset));
		}
		else
		{
			sync_scope();
			if (cache.generation() == before_copy) cache.stock_copy(key, persistent, required.first, persistent_offset);
			observe_packed_persistent(persistent);
		}
	};

	//Write all the data once if possible
	if (required.first && required.second && volatile_offset > persistent_offset)
	{
		//Do this once for both to save time on map/unmap cycles
		const usz block_end = (volatile_offset + required.second);
		const usz block_size = block_end - persistent_offset;
		const usz volatile_offset_in_block = volatile_offset - persistent_offset;

		void *block_mapping = m_attrib_ring_info.map(persistent_offset, block_size);
		write_vertex_stage(block_mapping, static_cast<char*>(block_mapping) + volatile_offset_in_block);
		m_attrib_ring_info.unmap();
	}
	else
	{
		if (required.first > 0 && persistent_offset != umax)
		{
			void *persistent_mapping = m_attrib_ring_info.map(persistent_offset, required.first);
			write_vertex_stage(persistent_mapping, nullptr);
			m_attrib_ring_info.unmap();
		}

		if (required.second > 0)
		{
			void *volatile_mapping = m_attrib_ring_info.map(volatile_offset, required.second);
			write_vertex_stage(nullptr, volatile_mapping);
			m_attrib_ring_info.unmap();
		}
	}

	if (vk::test_status_interrupt(vk::heap_changed))
	{
		// Check for validity
		if (m_persistent_attribute_storage &&
			m_persistent_attribute_storage->info.buffer != m_attrib_ring_info.heap->value)
		{
			vk::get_resource_manager()->dispose(m_persistent_attribute_storage);
		}

		if (m_volatile_attribute_storage &&
			m_volatile_attribute_storage->info.buffer != m_attrib_ring_info.heap->value)
		{
			vk::get_resource_manager()->dispose(m_volatile_attribute_storage);
		}

		m_vertex_env_buffer_info = { *m_vertex_env_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_vertex_constants_buffer_info = { *m_transform_constants_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_fragment_env_buffer_info = { *m_fragment_env_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_fragment_texture_params_buffer_info = { *m_fragment_texture_params_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_raster_env_buffer_info = { *m_raster_env_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_vertex_layout_stream_info = { *m_vertex_layout_ring_info.heap, 0, VK_WHOLE_SIZE };
		m_fragment_constants_buffer_info = { *m_fragment_constants_ring_info.heap, 0, VK_WHOLE_SIZE };

		vk::clear_status_interrupt(vk::heap_changed);
	}

	if (persistent_range_base != umax && !static_vertices && spu_window < 0)
	{
		if (!m_persistent_attribute_storage || !m_persistent_attribute_storage->in_range(persistent_range_base, required.first, persistent_range_base))
		{
			ensure(m_texbuffer_view_size >= required.first); // "Incompatible driver (MacOS?)"
			vk::get_resource_manager()->dispose(m_persistent_attribute_storage);

			//View 64M blocks at a time (different drivers will only allow a fixed viewable heap size, 64M should be safe)
			const usz view_size = (persistent_range_base + m_texbuffer_view_size) > m_attrib_ring_info.size() ? m_attrib_ring_info.size() - persistent_range_base : m_texbuffer_view_size;
			m_persistent_attribute_storage = std::make_unique<vk::buffer_view>(*m_device, m_attrib_ring_info.heap->value, VK_FORMAT_R8_UINT, persistent_range_base, view_size);
			persistent_range_base = 0;
		}
	}

	if (volatile_range_base != umax)
	{
		if (!m_volatile_attribute_storage || !m_volatile_attribute_storage->in_range(volatile_range_base, required.second, volatile_range_base))
		{
			ensure(m_texbuffer_view_size >= required.second); // "Incompatible driver (MacOS?)"
			vk::get_resource_manager()->dispose(m_volatile_attribute_storage);

			const usz view_size = (volatile_range_base + m_texbuffer_view_size) > m_attrib_ring_info.size() ? m_attrib_ring_info.size() - volatile_range_base : m_texbuffer_view_size;
			m_volatile_attribute_storage = std::make_unique<vk::buffer_view>(*m_device, m_attrib_ring_info.heap->value, VK_FORMAT_R8_UINT, volatile_range_base, view_size);
			volatile_range_base = 0;
		}
	}

	return{ result.native_primitive_type,                 // Primitive
			result.vertex_draw_count,                     // Vertex count
			vertex_count,                                 // Allocated vertex count
			vertex_base,                                  // First vertex in stream
			index_base,                                   // Index of vertex at data location 0
			result.vertex_index_offset,                   // Index offset
			persistent_range_base, volatile_range_base,   // Binding range
			result.index_info,                            // Index buffer info
			static_vertices, result.static_indices,       // Geometry cache buffers in use
			result.spu_indices,                           // Index data in the SPU upload heap's twin
			spu_window, spu_block_offsets };              // SPU upload heap in use
}
