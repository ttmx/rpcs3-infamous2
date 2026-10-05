#pragma once

#include "VKTextureCache.h"
#include <array>
#include <bit>
#include <cstring>
#include <memory>

namespace vk
{
	// Completed compressed-material bindings, valid only within one texture/surface-cache generation.
	// The owning renderer synchronizes and accesses this cache under m_sampler_mutex.
	class material_bindings
	{
	public:
		using key_type = std::array<u32, 11>; // Eight texture words, control2, control3, shader BX2 bit.
		struct entry
		{
			key_type key{};
			texture_cache::sampled_image_descriptor descriptor;
			sampler* handle = nullptr;
			u64 epoch = 0;
			u64 surface_tag = 0; // Not 0: the texture shares pages with a surface and expires with the surface cache generation
			~entry()
			{
				if (handle) static_cast<cached_sampler_object_t*>(handle)->release();
			}
		};

		u64 lookups = 0, hits = 0, checked = 0, mismatches = 0, invalidations = 0;
		u64 changed[3]{};
		u64 new_surface_pages = 0, surface_dependent_stores = 0, stores = 0;

	private:
		static constexpr usz capacity = 4096;
		std::unique_ptr<entry[]> m_entries;
		std::array<u64, 3> m_generation{};
		u64 m_epoch = 1;

		// 64 KiB pages that held a render target or depth buffer at any time. The lookup of a texture outside of them
		// does not depend on the surface cache, so its binding outlives surface cache generations (live control 10 bit 2
		// off: every binding expires with the surface cache generation).
		static constexpr u32 page_shift = 16;
		std::unique_ptr<std::array<u64, 1024>> m_surface_pages;

		bool mark_surface(u32 start, u32 end)
		{
			bool added = false;
			for (u32 page = start >> page_shift; page <= end >> page_shift; page++)
			{
				u64& word = (*m_surface_pages)[page / 64];
				const u64 bit = 1ull << (page % 64);
				added |= !(word & bit);
				word |= bit;
			}
			return added;
		}

		static usz slot(const key_type& key)
		{
			u64 hash = 14695981039346656037ull;
			for (const u32 word : key) hash = (hash ^ word) * 1099511628211ull;
			return (hash ^ (hash >> 32)) & (capacity - 1);
		}

	public:
		void reset() { m_entries.reset(); }

		static void bind_sampler(sampler*& current, sampler* next)
		{
			if (current == next) return;
			if (current) static_cast<cached_sampler_object_t*>(current)->release();
			static_cast<cached_sampler_object_t*>(next)->add_ref();
			current = next;
		}

		void invalidate()
		{
			invalidations++;
			if (++m_epoch == 0)
			{
				m_entries.reset();
				m_epoch = 1;
			}
		}

		// surfaces(func) calls func(memory range) for every surface of the surface cache
		template <typename F>
		void synchronize(const std::array<u64, 3>& generation, bool track_surfaces, F&& surfaces)
		{
			if (generation == m_generation)
			{
				return;
			}

			for (u32 i = 0; i < 3; i++) changed[i] += generation[i] != m_generation[i];
			bool expired = generation[0] != m_generation[0] || generation[1] != m_generation[1] || !track_surfaces;

			if (track_surfaces)
			{
				if (!m_surface_pages)
				{
					m_surface_pages = std::make_unique<std::array<u64, 1024>>();
					m_surface_pages->fill(0);
					expired = true;
				}

				bool added = false;
				surfaces([&](const auto& range)
				{
					if (range.valid()) added |= mark_surface(range.start, range.end);
				});

				new_surface_pages += added;
				expired |= added;
			}
			else
			{
				m_surface_pages.reset();
			}

			if (expired) invalidate();
			m_generation = generation;
		}

		u32 surface_page_count() const
		{
			u32 count = 0;
			if (m_surface_pages) for (const u64 word : *m_surface_pages) count += std::popcount(word);
			return count;
		}

		// True if the range (with a page of margin on both sides) touches a page that held a surface
		bool on_surface_pages(u32 start, u64 length) const
		{
			if (!m_surface_pages) return true;
			const u64 last = std::min<u64>(0xffffffffull, u64{start} + std::max<u64>(length, 1) - 1) >> page_shift;
			const u64 first = start >> page_shift;

			for (u64 page = first ? first - 1 : 0; page <= std::min<u64>(last + 1, 0xffff); page++)
			{
				if ((*m_surface_pages)[page / 64] & (1ull << (page % 64))) return true;
			}

			return false;
		}

		const entry* find(const key_type& key)
		{
			lookups++;
			if (!m_entries) return nullptr;
			const auto& value = m_entries[slot(key)];
			if (value.epoch != m_epoch || value.key != key) return nullptr;
			if (value.surface_tag && value.surface_tag != m_generation[2]) return nullptr;
			hits++;
			return &value;
		}

		void store(const key_type& key, const texture_cache::sampled_image_descriptor& descriptor, sampler* handle, bool surface_dependent)
		{
			if (!m_entries) m_entries = std::make_unique<entry[]>(capacity);
			auto& value = m_entries[slot(key)];
			stores++;
			value.key = key;
			value.descriptor = descriptor;
			bind_sampler(value.handle, handle);
			value.epoch = m_epoch;
			value.surface_tag = surface_dependent ? std::max<u64>(m_generation[2], 1) : 0;
			surface_dependent_stores += surface_dependent;
		}

		static bool eligible(const texture_cache::sampled_image_descriptor& descriptor, sampler* handle)
		{
			return handle && descriptor.image_handle && descriptor.upload_context == rsx::texture_upload_context::shader_read &&
				!descriptor.is_cyclic_reference && descriptor.external_subresource_desc.op == rsx::deferred_request_command::nop &&
				!descriptor.external_subresource_desc.do_not_cache;
		}

		// Compare the values actually used for binding and shader environment generation. Do not compare padding
		// or inactive clamp bounds, which the descriptor constructors leave uninitialized.
		static bool matches(const entry& expected, const texture_cache::sampled_image_descriptor& actual, sampler* handle)
		{
			const auto& d = expected.descriptor;
			return eligible(actual, handle) && expected.handle == handle && d.image_handle == actual.image_handle &&
				d.image_type == actual.image_type && d.format_class == actual.format_class && d.samples == actual.samples &&
				d.ref_address == actual.ref_address && d.format_ex == actual.format_ex &&
				d.format_ex.texel_remap_control == actual.format_ex.texel_remap_control &&
				!std::memcmp(d.texcoord_xform.scale, actual.texcoord_xform.scale, sizeof(d.texcoord_xform.scale)) &&
				!std::memcmp(d.texcoord_xform.bias, actual.texcoord_xform.bias, sizeof(d.texcoord_xform.bias)) &&
				d.texcoord_xform.clamp == actual.texcoord_xform.clamp &&
				(!d.texcoord_xform.clamp ||
					(!std::memcmp(d.texcoord_xform.clamp_min, actual.texcoord_xform.clamp_min, sizeof(d.texcoord_xform.clamp_min)) &&
					 !std::memcmp(d.texcoord_xform.clamp_max, actual.texcoord_xform.clamp_max, sizeof(d.texcoord_xform.clamp_max))));
		}
	};
}
