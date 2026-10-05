#include "stdafx.h"
#include "VKProgramPipeline.h"
#include "VKResourceManager.h"
#include "vkutils/descriptors.h"
#include "vkutils/descriptor_reuse.h"
#include "VKLiveCtl.hpp"
#include "vkutils/device.h"
#include "vkutils/image.h"
#include "vkutils/sampler.h"

#include "../Program/SPIRVCommon.h"

namespace vk
{
	namespace glsl
	{
		using namespace ::glsl;

		bool operator == (const descriptor_slot_t& a, const VkDescriptorImageInfoEx& b)
		{
			const auto ptr = std::get_if<VkDescriptorImageInfoEx>(&a);
			return !!ptr &&
				ptr->resourceId == b.resourceId &&
				ptr->imageView == b.imageView &&
				ptr->sampler == b.sampler &&
				ptr->imageLayout == b.imageLayout;
		}

		bool operator == (const descriptor_slot_t& a, const VkDescriptorBufferInfoEx& b)
		{
			const auto ptr = std::get_if<VkDescriptorBufferInfoEx>(&a);
			return !!ptr &&
				ptr->resourceId == b.resourceId &&
				ptr->buffer == b.buffer &&
				ptr->offset == b.offset &&
				ptr->range == b.range;
		}

		bool operator == (const descriptor_slot_t& a, const VkDescriptorBufferViewEx& b)
		{
			const auto ptr = std::get_if<VkDescriptorBufferViewEx>(&a);
			return !!ptr && ptr->resourceId == b.resourceId;
		}

		bool operator == (const descriptor_slot_t& a, const std::span<const VkDescriptorImageInfoEx>& b)
		{
			const auto ptr = std::get_if<descriptor_image_array_t>(&a);
			return !!ptr && ptr->size() == b.size() && !std::memcmp(ptr->data(), b.data(), b.size_bytes());
		}

		VkDescriptorType to_descriptor_type(program_input_type type)
		{
			switch (type)
			{
			case input_type_uniform_buffer:
				return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			case input_type_texel_buffer:
				return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
			case input_type_texture:
				return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			case input_type_storage_buffer:
				return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
			case input_type_storage_texture:
				return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			case input_type_attachment:
				return VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
			default:
				fmt::throw_exception("Unexpected program input type %d", static_cast<int>(type));
			}
		}

		VkShaderStageFlags to_shader_stage_flags(::glsl::program_domain domain)
		{
			switch (domain)
			{
			case glsl_vertex_program:
				return VK_SHADER_STAGE_VERTEX_BIT;
			case glsl_fragment_program:
				return VK_SHADER_STAGE_FRAGMENT_BIT;
			case glsl_compute_program:
				return VK_SHADER_STAGE_COMPUTE_BIT;
			default:
				fmt::throw_exception("Unexpected domain %d", static_cast<int>(domain));
			}
		}

		const char* to_string(::glsl::program_domain domain)
		{
			switch (domain)
			{
			case glsl_vertex_program:
				return "vertex";
			case glsl_fragment_program:
				return "fragment";
			case glsl_compute_program:
				return "compute";
			default:
				fmt::throw_exception("Unexpected domain %d", static_cast<int>(domain));
			}
		}

		void shader::create(::glsl::program_domain domain, const std::string& source)
		{
			type     = domain;
			m_source = source;
		}

		VkShaderModule shader::compile()
		{
			ensure(m_handle == VK_NULL_HANDLE);

			if (!spirv::compile_glsl_to_spv(m_compiled, m_source, type, ::glsl::glsl_rules_vulkan))
			{
				rsx_log.notice("%s", m_source);
				fmt::throw_exception("Failed to compile %s shader", to_string(type));
			}

			VkShaderModuleCreateInfo vs_info;
			vs_info.codeSize = m_compiled.size() * sizeof(u32);
			vs_info.pNext    = nullptr;
			vs_info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			vs_info.pCode    = m_compiled.data();
			vs_info.flags    = 0;

			vkCreateShaderModule(*g_render_device, &vs_info, nullptr, &m_handle);

			return m_handle;
		}

		void shader::destroy()
		{
			m_source.clear();
			m_compiled.clear();

			if (m_handle)
			{
				vkDestroyShaderModule(*g_render_device, m_handle, nullptr);
				m_handle = nullptr;
			}
		}

		const std::string& shader::get_source() const
		{
			return m_source;
		}

		const std::vector<u32> shader::get_compiled() const
		{
			return m_compiled;
		}

		VkShaderModule shader::get_handle() const
		{
			return m_handle;
		}

		void program::init()
		{
			m_linked = false;
		}

		program::program(VkDevice dev, const VkGraphicsPipelineCreateInfo& create_info, const std::vector<program_input> &vertex_inputs, const std::vector<program_input>& fragment_inputs)
			: m_device(dev), m_info(create_info)
		{
			init();

			load_uniforms(vertex_inputs);
			load_uniforms(fragment_inputs);
		}

		program::program(VkDevice dev, const VkComputePipelineCreateInfo& create_info, const std::vector<program_input>& compute_inputs)
			: m_device(dev), m_info(create_info)
		{
			init();

			load_uniforms(compute_inputs);
		}

		program::~program()
		{
			vkDestroyPipeline(m_device, m_pipeline, nullptr);

			if (m_pipeline_layout)
			{
				vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);

				for (auto& set : m_sets)
				{
					set.destroy();
				}
			}
		}

		program& program::load_uniforms(const std::vector<program_input>& inputs)
		{
			ensure(!m_linked); // "Cannot change uniforms in already linked program!"

			for (auto &item : inputs)
			{
				ensure(item.set < binding_set_index_max_enum);                         // Ensure we have a valid set id
				ensure(item.location < 128u || item.type == input_type_push_constant); // Arbitrary limit but useful to catch possibly uninitialized values
				m_sets[item.set].m_inputs[item.type].push_back(item);
			}

			return *this;
		}

		program& program::link(VkPipelineCache pipeline_cache, bool separate_objects)
		{
			auto p_graphics_info = std::get_if<VkGraphicsPipelineCreateInfo>(&m_info);
			auto p_compute_info = !p_graphics_info ? std::get_if<VkComputePipelineCreateInfo>(&m_info) : nullptr;
			const bool is_graphics_pipe = p_graphics_info != nullptr;

			if (!is_graphics_pipe) [[ likely ]]
			{
				// We only support compute and graphics, so disable this for compute
				separate_objects = false;
			}

			if (!separate_objects)
			{
				// Collapse all sets into set 0 if validation passed
				auto& sink = m_sets[0];
				for (auto& set : m_sets)
				{
					if (&set == &sink)
					{
						continue;
					}

					for (auto& type_arr : set.m_inputs)
					{
						if (type_arr.empty())
						{
							continue;
						}

						auto type = type_arr.front().type;
						auto& dst = sink.m_inputs[type];
						dst.insert(dst.end(), type_arr.begin(), type_arr.end());

						// Clear
						type_arr.clear();
					}
				}

				sink.validate();
				sink.init(m_device);
			}
			else
			{
				for (auto& set : m_sets)
				{
					for (auto& type_arr : set.m_inputs)
					{
						if (type_arr.empty())
						{
							continue;
						}

						// Real set
						set.validate();
						set.init(m_device);
						break;
					}
				}
			}

			create_pipeline_layout();
			ensure(m_pipeline_layout);

			if (is_graphics_pipe)
			{
				VkGraphicsPipelineCreateInfo create_info = *p_graphics_info;
				create_info.layout = m_pipeline_layout;
				CHECK_RESULT(vkCreateGraphicsPipelines(m_device, pipeline_cache, 1, &create_info, nullptr, &m_pipeline));
			}
			else
			{
				VkComputePipelineCreateInfo create_info = *p_compute_info;
				create_info.layout = m_pipeline_layout;
				CHECK_RESULT(vkCreateComputePipelines(m_device, pipeline_cache, 1, &create_info, nullptr, &m_pipeline));
			}

			m_linked = true;
			return *this;
		}

		bool program::has_uniform(program_input_type type, std::string_view uniform_name)
		{
			for (auto& set : m_sets)
			{
				const auto& uniform = set.m_inputs[type];
				return std::any_of(uniform.cbegin(), uniform.cend(), [&uniform_name](const auto& u)
				{
					return u.name == uniform_name;
				});
			}

			return false;
		}

		std::pair<u32, u32> program::get_uniform_location(::glsl::program_domain domain, program_input_type type, std::string_view uniform_name)
		{
			for (unsigned i = 0; i < ::size32(m_sets); ++i)
			{
				const auto& type_arr = m_sets[i].m_inputs[type];
				const auto result = std::find_if(type_arr.cbegin(), type_arr.cend(), [&](const auto& u)
				{
					return u.domain == domain && u.name == uniform_name;
				});

				if (result != type_arr.end())
				{
					return { i, result->location };
				}
			}

			return { umax, umax };
		}

		void program::bind_uniform(const VkDescriptorImageInfoEx& image_descriptor, u32 set_id, u32 binding_point)
		{
			if (m_sets[set_id].m_descriptor_slots[binding_point] == image_descriptor)
			{
				return;
			}

			m_sets[set_id].notify_descriptor_slot_updated(binding_point, image_descriptor);
		}

		void program::bind_uniform(const VkDescriptorBufferInfoEx &buffer_descriptor, u32 set_id, u32 binding_point)
		{
			if (m_sets[set_id].m_descriptor_slots[binding_point] == buffer_descriptor)
			{
				return;
			}

			m_sets[set_id].notify_descriptor_slot_updated(binding_point, buffer_descriptor);
		}

		void program::bind_uniform(const VkDescriptorBufferViewEx& buffer_view, u32 set_id, u32 binding_point)
		{
			if (m_sets[set_id].m_descriptor_slots[binding_point] == buffer_view)
			{
				return;
			}

			m_sets[set_id].notify_descriptor_slot_updated(binding_point, buffer_view);
		}

		void program::bind_uniform_array(const std::span<const VkDescriptorImageInfoEx>& image_descriptors, u32 set_id, u32 binding_point)
		{
			if (m_sets[set_id].m_descriptor_slots[binding_point] == image_descriptors)
			{
				return;
			}

			m_sets[set_id].notify_descriptor_slot_updated(binding_point, image_descriptors);
		}

		void program::create_pipeline_layout()
		{
			ensure(!m_linked);
			ensure(m_pipeline_layout == VK_NULL_HANDLE);

			rsx::simple_array<VkPushConstantRange> push_constants{};
			rsx::simple_array<VkDescriptorSetLayout> set_layouts{};

			for (auto& set : m_sets)
			{
				if (!set.m_device)
				{
					continue;
				}

				set.create_descriptor_set_layout();
				set_layouts.push_back(set.m_descriptor_set_layout);

				for (const auto& input : set.m_inputs[input_type_push_constant])
				{
					const auto& range = input.as_push_constant();
					push_constants.push_back({
						.stageFlags = to_shader_stage_flags(input.domain),
						.offset = range.offset,
						.size = range.size
						});
				}
			}

			VkPipelineLayoutCreateInfo create_info
			{
				.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
				.flags = 0,
				.setLayoutCount = set_layouts.size(),
				.pSetLayouts = set_layouts.data(),
				.pushConstantRangeCount = push_constants.size(),
				.pPushConstantRanges = push_constants.data()
			};
			CHECK_RESULT(vkCreatePipelineLayout(m_device, &create_info, nullptr, &m_pipeline_layout));
		}

		program& program::bind(const vk::command_buffer& cmd, VkPipelineBindPoint bind_point)
		{
			VkDescriptorSet bind_sets[binding_set_index_max_enum];
			unsigned count = 0;

			for (auto& set : m_sets)
			{
				if (!set.m_device)
				{
					continue;
				}

				bind_sets[count++] = set.commit();   // Commit variable changes and return handle to the new set
			}

			cmd.bind_pipeline(m_pipeline, bind_point);
			cmd.bind_descriptor_sets({ bind_sets, count }, bind_point, m_pipeline_layout);
			return *this;
		}

		void descriptor_table_t::destroy()
		{
			if (!m_device)
			{
				return;
			}

			if (m_descriptor_set_layout)
			{
				vkDestroyDescriptorSetLayout(m_device, m_descriptor_set_layout, nullptr);
			}

			if (m_descriptor_pool)
			{
				m_descriptor_pool->destroy();
				m_descriptor_pool.reset();
			}

			if (m_reuse_pool)
			{
				vkDestroyDescriptorPool(m_device, m_reuse_pool, nullptr);
				m_reuse_pool = VK_NULL_HANDLE;
			}

			m_device = VK_NULL_HANDLE;
		}

		void descriptor_table_t::init(VkDevice dev)
		{
			m_device = dev;

			size_t bind_slots_count = 0;
			for (auto& type_arr : m_inputs)
			{
				if (type_arr.empty() || type_arr.front().type == input_type_push_constant)
				{
					continue;
				}

				bind_slots_count += type_arr.size();
			}

			m_descriptor_slots.resize(bind_slots_count);
			std::fill(m_descriptor_slots.begin(), m_descriptor_slots.end(), descriptor_slot_t{});

			m_descriptors_dirty.resize(bind_slots_count);
			std::fill(m_descriptors_dirty.begin(), m_descriptors_dirty.end(), false);
		}

		VkDescriptorSet descriptor_table_t::allocate_descriptor_set()
		{
			if (!m_descriptor_pool)
			{
				create_descriptor_pool();
			}

			return m_descriptor_pool->allocate(m_descriptor_set_layout);
		}

		void descriptor_table_t::create_descriptor_template()
		{
			auto push_descriptor_slot = [this](unsigned idx, VkWriteDescriptorSet& writer)
			{
				const auto& slot = m_descriptor_slots[idx];
				const VkDescriptorType type = m_descriptor_types[idx];

				writer.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				writer.dstBinding = idx;
				writer.descriptorCount = 1;
				writer.descriptorType = type;

				if (auto ptr = std::get_if<VkDescriptorImageInfoEx>(&slot))
				{
					m_descriptor_set.push(*ptr, type, idx);
					return;
				}

				if (auto ptr = std::get_if<VkDescriptorBufferInfoEx>(&slot))
				{
					m_descriptor_set.push(*ptr, type, idx);
					return;
				}

				if (auto ptr = std::get_if<VkDescriptorBufferViewEx>(&slot))
				{
					m_descriptor_set.push(ptr->view, type, idx);
					return;
				}

				if (auto ptr = std::get_if<descriptor_image_array_t>(&slot))
				{
					// We need to convert the VkDescriptorImageInfoEx entries back to the native vulkan variants since we're going to be flushing an array with no stride check
					auto vk_data = ptr->map(FN(static_cast<VkDescriptorImageInfo>(x)));

					writer.descriptorCount = ptr->size();
					m_descriptor_set.push(vk_data.data(), vk_data.size(), type, idx);
					return;
				}

				fmt::throw_exception("Unexpected descriptor structure at index %u", idx);
			};

			m_descriptor_template_typemask = 0u;
			m_descriptor_template.clear();

			for (unsigned i = 0; i < m_descriptor_slots.size(); ++i)
			{
				m_descriptor_template_typemask |= (1u << static_cast<u32>(m_descriptor_types[i]));
				VkWriteDescriptorSet _template{};

				push_descriptor_slot(i, _template);
				m_descriptors_dirty[i] = false;
				m_descriptor_template.push_back(_template);
			}

			m_descriptor_template_cache_id = umax;
			ensure(m_descriptor_template.size() == m_descriptor_slots.size());
		}

		void descriptor_table_t::update_descriptor_template()
		{
			auto update_descriptor_slot = [this](unsigned idx)
			{
				const auto& slot = m_descriptor_slots[idx];

				if (auto ptr = std::get_if<VkDescriptorImageInfoEx>(&slot))
				{
					m_descriptor_template[idx].pImageInfo = m_descriptor_set.store(*ptr);
					return;
				}

				if (auto ptr = std::get_if<VkDescriptorBufferInfoEx>(&slot))
				{
					m_descriptor_template[idx].pBufferInfo = m_descriptor_set.store(*ptr);
					return;
				}

				if (auto ptr = std::get_if<VkDescriptorBufferViewEx>(&slot))
				{
					m_descriptor_template[idx].pTexelBufferView = m_descriptor_set.store(ptr->view);
					return;
				}

				if (auto ptr = std::get_if<descriptor_image_array_t>(&slot))
				{
					auto vk_data = ptr->map(FN(static_cast<VkDescriptorImageInfo>(x))); // This can be optimized to update only changed ids but this is an interpreter-only feature for now
					ensure(m_descriptor_template[idx].descriptorCount == ptr->size());
					m_descriptor_template[idx].pImageInfo = m_descriptor_set.store(vk_data);
					return;
				}

				fmt::throw_exception("Unexpected descriptor structure at index %u", idx);
			};

			{
				std::lock_guard lock(m_descriptor_set);
				const bool cache_is_valid = m_descriptor_template_cache_id == m_descriptor_set.cache_id();

				for (unsigned i = 0; i < m_descriptor_slots.size(); ++i)
				{
					m_descriptor_template[i].dstSet = m_descriptor_set.value();
					if (!m_descriptors_dirty[i] && cache_is_valid)
					{
						continue;
					}

					// Update
					update_descriptor_slot(i);
					m_descriptors_dirty[i] = false;
				}
			}

			// Push
			m_descriptor_set.push(m_descriptor_template, m_descriptor_template_typemask);
			m_descriptor_template_cache_id = m_descriptor_set.cache_id();
		}

		// Everything a set is written with, slot by slot. False if a slot holds an image array (interpreter only).
		bool descriptor_table_t::make_reuse_key(u64& hash, u64& handle_classes)
		{
			m_reuse_key.clear();
			handle_classes = 0;

			for (const auto& slot : m_descriptor_slots)
			{
				if (auto ptr = std::get_if<VkDescriptorImageInfoEx>(&slot))
				{
					m_reuse_key.push_back(ptr->resourceId);
					m_reuse_key.push_back(reinterpret_cast<u64>(ptr->imageView));
					m_reuse_key.push_back(reinterpret_cast<u64>(ptr->sampler));
					m_reuse_key.push_back(u64{static_cast<u32>(ptr->imageLayout)} | (1ull << 60));
					handle_classes |= vk::descriptor_reuse::handle_bit(reinterpret_cast<u64>(ptr->imageView)) | vk::descriptor_reuse::handle_bit(reinterpret_cast<u64>(ptr->sampler));
				}
				else if (auto ptr = std::get_if<VkDescriptorBufferInfoEx>(&slot))
				{
					m_reuse_key.push_back(ptr->resourceId);
					m_reuse_key.push_back(reinterpret_cast<u64>(ptr->buffer));
					m_reuse_key.push_back(ptr->offset);
					m_reuse_key.push_back(ptr->range ^ (2ull << 60));
				}
				else if (auto ptr = std::get_if<VkDescriptorBufferViewEx>(&slot))
				{
					m_reuse_key.push_back(ptr->resourceId);
					m_reuse_key.push_back(reinterpret_cast<u64>(ptr->view));
					m_reuse_key.push_back(3ull << 60);
				}
				else
				{
					return false;
				}
			}

			hash = 14695981039346656037ull;
			for (const u64 word : m_reuse_key)
			{
				hash = (hash ^ word) * 1099511628211ull;
				hash ^= hash >> 29;
			}

			return true;
		}

		// Command buffers that are still in flight may use the sets, so the pool goes through deferred disposal
		void descriptor_table_t::retire_reuse_pool()
		{
			if (!m_reuse_pool)
			{
				return;
			}

			auto cleanup_obj = std::make_unique<gc_callback_t>([device = m_device, pool = m_reuse_pool]()
			{
				vkDestroyDescriptorPool(device, pool, nullptr);
			});
			vk::get_gc()->dispose(cleanup_obj);

			for (u32 i = 0; m_reuse_entries && i < reuse_entry_count; i++)
			{
				m_reuse_entries[i].set = VK_NULL_HANDLE;
			}

			m_reuse_pool = VK_NULL_HANDLE;
			m_reuse_pool_used = 0;
			m_reuse_pool_sets = std::min(m_reuse_pool_sets * 2, reuse_pool_max_sets);
			m_reuse_free_sets.clear();
			vk::descriptor_reuse::pools_retired++;
		}

		// VK_NULL_HANDLE if the pool cannot provide one; the caller then writes a regular set
		VkDescriptorSet descriptor_table_t::allocate_reusable_set()
		{
			if (m_reuse_free_sets.empty())
			{
				constexpr u32 batch = 64;

				if (m_reuse_pool && m_reuse_pool_used + batch > m_reuse_pool_sets)
				{
					retire_reuse_pool();
				}

				if (!m_reuse_pool)
				{
					auto sizes = m_descriptor_pool_sizes.map([count = m_reuse_pool_sets](const VkDescriptorPoolSize& size)
					{
						auto ret = size;
						ret.descriptorCount *= count;
						return ret;
					});

					VkDescriptorPoolCreateInfo info = {};
					info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
					info.flags = vk::g_render_device->get_descriptor_update_after_bind_support() ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
					info.maxSets = m_reuse_pool_sets;
					info.poolSizeCount = ::size32(sizes);
					info.pPoolSizes = sizes.data();

					if (vkCreateDescriptorPool(m_device, &info, nullptr, &m_reuse_pool) != VK_SUCCESS)
					{
						m_reuse_pool = VK_NULL_HANDLE;
						return VK_NULL_HANDLE;
					}
				}

				VkDescriptorSetLayout layouts[batch];
				std::fill_n(layouts, batch, m_descriptor_set_layout);

				VkDescriptorSetAllocateInfo alloc_info = {};
				alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
				alloc_info.descriptorPool = m_reuse_pool;
				alloc_info.descriptorSetCount = batch;
				alloc_info.pSetLayouts = layouts;

				m_reuse_free_sets.resize(batch);
				if (vkAllocateDescriptorSets(m_device, &alloc_info, m_reuse_free_sets.data()) != VK_SUCCESS)
				{
					m_reuse_free_sets.clear();
					m_reuse_pool_used = m_reuse_pool_sets;
					return VK_NULL_HANDLE;
				}

				m_reuse_pool_used += batch;
			}

			return m_reuse_free_sets.pop_back();
		}

		VkDescriptorSet descriptor_table_t::commit()
		{
			if (!m_descriptor_set)
			{
				m_any_descriptors_dirty = true;
				std::fill(m_descriptors_dirty.begin(), m_descriptors_dirty.end(), false);
			}

			// Check if we need to actually open a new set
			if (!m_any_descriptors_dirty)
			{
				return m_descriptor_set.value();
			}

			// A set that was already written with exactly these contents is bound again instead of writing another one.
			// Tables that rarely change are left alone.
			reuse_entry_t* reuse_entry = nullptr;
			u64 reuse_hash = 0, reuse_classes = 0;
			VkDescriptorSet new_set = VK_NULL_HANDLE;

			if (vk::live_ctl::get(12) == 1 && !m_descriptor_template.empty() && ++m_reuse_commits > 64)
			{
				m_reuse_commits = 65;

				if (make_reuse_key(reuse_hash, reuse_classes))
				{
					if (!m_reuse_entries)
					{
						m_reuse_entries = std::make_unique<reuse_entry_t[]>(reuse_entry_count);
						m_reuse_retire_tag = vk::descriptor_reuse::retire_count.load(std::memory_order_acquire);
					}

					if (vk::descriptor_reuse::retire_count.load(std::memory_order_acquire) != m_reuse_retire_tag)
					{
						// A destroyed view or sampler may give its handle value to another object
						u64 handles[16], retired = 0;
						u32 handle_count = 0;
						const bool listed = vk::descriptor_reuse::retired_since(m_reuse_retire_tag, handles, 16, handle_count, retired);

						for (u32 i = 0; i < reuse_entry_count; i++)
						{
							auto& entry = m_reuse_entries[i];
							if (!entry.set || !(entry.handle_classes & retired))
							{
								continue;
							}

							// Same class: look for the value itself (any word of the key, which errs on the side of dropping)
							bool drop = !listed;
							for (u32 n = 0; n < handle_count && !drop; n++)
							{
								drop = std::find(entry.key.begin(), entry.key.end(), handles[n]) != entry.key.end();
							}

							if (drop)
							{
								entry.set = VK_NULL_HANDLE;
								vk::descriptor_reuse::dropped++;
							}
						}

						vk::descriptor_reuse::syncs++;
					}

					reuse_entry = &m_reuse_entries[reuse_hash % reuse_entry_count];

					if (reuse_entry->set && reuse_entry->hash == reuse_hash && reuse_entry->key == m_reuse_key)
					{
						// The slots that changed stay marked: the write template still has their previous values
						m_descriptor_set = reuse_entry->set;
						m_any_descriptors_dirty = false;
						vk::descriptor_reuse::hits++;
						return reuse_entry->set;
					}

					// Retiring the pool clears the entries, this one included
					new_set = allocate_reusable_set();

					if (new_set)
					{
						reuse_entry->hash = reuse_hash;
						reuse_entry->handle_classes = reuse_classes;
						reuse_entry->set = new_set;
						reuse_entry->key = m_reuse_key;
						vk::descriptor_reuse::misses++;
					}
					else
					{
						vk::descriptor_reuse::fallbacks++;
					}
				}
				else
				{
					vk::descriptor_reuse::unkeyed++;
				}
			}

			m_descriptor_set = new_set ? new_set : allocate_descriptor_set();

			if (!m_descriptor_template.empty()) [[ likely ]]
			{
				// Run pointer updates. Optimized for cached back-to-back updates which are quite frequent.
				update_descriptor_template();
			}
			else
			{
				// Creating the template also seeds initial values
				create_descriptor_template();
			}

			m_descriptor_set.on_bind();
			m_any_descriptors_dirty = false;

			return m_descriptor_set.value();
		}

		void descriptor_table_t::create_descriptor_set_layout()
		{
			ensure(m_descriptor_set_layout == VK_NULL_HANDLE);

			rsx::simple_array<VkDescriptorSetLayoutBinding> bindings;
			bindings.reserve(16);

			m_descriptor_pool_sizes.clear();
			m_descriptor_pool_sizes.reserve(input_type_max_enum);

			std::unordered_map<u32, VkDescriptorType> descriptor_type_map;

			auto descriptor_count = [](std::string_view name) -> u32
			{
				const auto start = name.find_last_of("[");
				if (start == std::string::npos)
				{
					return 1;
				}

				const auto end = name.find_last_of("]");
				ensure(end != std::string::npos && start < end, "Invalid variable name");

				const std::string_view array_size = name.substr(start + 1, end - start - 1);
				if (const auto count = std::atoi(array_size.data());
					count > 0)
				{
					return count;
				}

				return 1;
			};

			for (const auto& type_arr : m_inputs)
			{
				if (type_arr.empty() || type_arr.front().type == input_type_push_constant)
				{
					continue;
				}

				VkDescriptorType type = to_descriptor_type(type_arr.front().type);
				m_descriptor_pool_sizes.push_back({ .type = type });

				for (const auto& input : type_arr)
				{
					VkDescriptorSetLayoutBinding binding
					{
						.binding = input.location,
						.descriptorType = type,
						.descriptorCount = descriptor_count(input.name),
						.stageFlags = to_shader_stage_flags(input.domain) | input.ex_stages
					};
					bindings.push_back(binding);

					descriptor_type_map[input.location] = type;
					m_descriptor_pool_sizes.back().descriptorCount += binding.descriptorCount;
				}
			}

			m_descriptor_types.resize(::size32(m_descriptors_dirty));

			for (u32 i = 0; i < ::size32(m_descriptors_dirty); ++i)
			{
				if (descriptor_type_map.find(i) == descriptor_type_map.end())
				{
					fmt::throw_exception("Invalid input structure. Some input bindings were not declared!");
				}
				m_descriptor_types[i] = descriptor_type_map[i];
			}

			m_descriptor_set_layout = vk::descriptors::create_layout(bindings);
		}

		void descriptor_table_t::create_descriptor_pool()
		{
			m_descriptor_pool = std::make_unique<descriptor_pool>();
			m_descriptor_pool->create(*vk::g_render_device, m_descriptor_pool_sizes, 16u, 4096u);
		}

		void descriptor_table_t::validate() const
		{
			// Check for overlapping locations
			std::set<u32> taken_locations;

			for (auto& type_arr : m_inputs)
			{
				if (type_arr.empty() ||
					type_arr.front().type == input_type_push_constant)
				{
					continue;
				}

				for (const auto& input : type_arr)
				{
					ensure(taken_locations.find(input.location) == taken_locations.end(), "Overlapping input locations found.");
					taken_locations.insert(input.location);
				}
			}
		}
	}
}
