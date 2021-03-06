/* Copyright (c) 2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "volk.h"
#include "device.hpp"
#include "fossilize.hpp"
#include "cli_parser.hpp"
#include "logging.hpp"
#include "file.hpp"
#include "path.hpp"
#include "fossilize_db.hpp"
#include "fossilize_external_replayer.hpp"
#include "fossilize_external_replayer_control_block.hpp"

#include <cinttypes>
#include <string>
#include <unordered_set>
#include <stdlib.h>
#include <string.h>
#include <chrono>	// VALVE
#include <queue>	// VALVE
#include <thread>	// VALVE
#include <mutex>	// VALVE
#include <condition_variable> // VALVE
#include <atomic>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <utility>

using namespace Fossilize;
using namespace std;

//#define SIMULATE_UNSTABLE_DRIVER
//#define SIMULATE_SPURIOUS_DEADLOCK

#ifdef SIMULATE_UNSTABLE_DRIVER
#include <random>
#ifdef _WIN32
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void simulate_crash(int *v)
{
	*v = 0;
}

#ifdef _WIN32
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static int simulate_divide_by_zero(int a, int b)
{
	return a / b;
}

static int simulate_stack_overflow()
{
	volatile char buffer[16 * 1024 * 1024];
	for (auto &b : buffer)
		b++;
	return buffer[6124];
}

void spurious_crash()
{
	std::uniform_int_distribution<int> dist(0, 15);
	auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	std::mt19937 rnd(ns);

	// Simulate fatal things like segfaults and aborts.
	int r = dist(rnd);

	if (r < 1)
	{
		LOGE("Simulating a crash ...\n");
		simulate_crash(nullptr);
		LOGE("Should not reach here ...\n");
	}

	if (r < 2)
	{
		LOGE("Simulating an abort ...\n");
		abort();
		LOGE("Should not reach here ...\n");
	}

	if (r < 3)
	{
		LOGE("Simulating divide by zero ...\n");
		r = simulate_divide_by_zero(1, 0);
		LOGE("Should not reach here ... Boop: %d\n", r);
	}

	if (r < 4)
	{
		LOGE("Creating a stack overflow ...\n");
		r = simulate_stack_overflow();
		LOGE("Should not reach here ... Boop: %d\n", r);
	}
}

void spurious_deadlock()
{
#ifdef SIMULATE_SPURIOUS_DEADLOCK
	std::uniform_int_distribution<int> dist(0, 15);
	auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	std::mt19937 rnd(ns);

	if (dist(rnd) < 4)
	{
		LOGE("Simulating a deadlock ...\n");
		std::this_thread::sleep_for(std::chrono::seconds(100));
	}
#endif
}
#endif

// Unstable, but deterministic.
template <typename BidirectionalItr, typename UnaryPredicate>
BidirectionalItr unstable_remove_if(BidirectionalItr first, BidirectionalItr last, UnaryPredicate &&p)
{
	while (first != last)
	{
		if (p(*first))
		{
			--last;
			std::swap(*first, *last);
		}
		else
			++first;
	}

	return first;
}

struct ThreadedReplayer : StateCreatorInterface
{
	struct Options
	{
		bool pipeline_cache = false;
		string on_disk_pipeline_cache_path;

		// VALVE: Add multi-threaded pipeline creation
		unsigned num_threads = thread::hardware_concurrency();

		// VALVE: --loop option for testing performance
		unsigned loop_count = 1;

		// Carve out a range of which pipelines to replay.
		// Used for multi-process replays where each process gets its own slice to churn through.
		unsigned start_graphics_index = 0;
		unsigned end_graphics_index = ~0u;
		unsigned start_compute_index = 0;
		unsigned end_compute_index = ~0u;

		SharedControlBlock *control_block = nullptr;

		void (*on_thread_callback)(void *userdata) = nullptr;
		void *on_thread_callback_userdata = nullptr;
	};

	struct DeferredGraphicsInfo
	{
		VkGraphicsPipelineCreateInfo *info;
		Hash hash;
		VkPipeline *pipeline;
		bool contributes_to_index;
	};

	struct DeferredComputeInfo
	{
		VkComputePipelineCreateInfo *info;
		Hash hash;
		VkPipeline *pipeline;
		bool contributes_to_index;
	};

	ThreadedReplayer(const VulkanDevice::Options &device_opts_, const Options &opts_)
		: opts(opts_),
		  num_worker_threads(opts.num_threads), loop_count(opts.loop_count),
		  device_opts(device_opts_)
	{
		// Cannot use initializers for atomics.
		graphics_pipeline_ns.store(0);
		compute_pipeline_ns.store(0);
		shader_module_ns.store(0);
		graphics_pipeline_count.store(0);
		compute_pipeline_count.store(0);
		shader_module_count.store(0);
		thread_total_ns.store(0);
		total_idle_ns.store(0);

		thread_current_graphics_index = opts.start_graphics_index;
		thread_current_compute_index = opts.start_compute_index;

		// Create a thread pool with the # of specified worker threads (defaults to thread::hardware_concurrency()).
		for (unsigned i = 0; i < num_worker_threads; i++)
			thread_pool.push_back(std::thread(&ThreadedReplayer::worker_thread, this));
	}

	void sync_worker_threads()
	{
		unique_lock<mutex> lock(pipeline_work_queue_mutex);
		work_done_condition.wait(lock, [&]() -> bool {
			return queued_count == completed_count;
		});
	}

	void worker_thread()
	{
		if (opts.on_thread_callback)
			opts.on_thread_callback(opts.on_thread_callback_userdata);

		uint64_t graphics_ns = 0;
		unsigned graphics_count = 0;

		uint64_t compute_ns = 0;
		unsigned compute_count = 0;

		uint64_t shader_ns = 0;
		unsigned shader_count = 0;

		uint64_t idle_ns = 0;
		auto thread_start_time = chrono::steady_clock::now();

		for (;;)
		{
			PipelineWorkItem work_item;
			auto idle_start_time = chrono::steady_clock::now();
			{
				unique_lock<mutex> lock(pipeline_work_queue_mutex);
				work_available_condition.wait(lock, [&]() -> bool {
					return shutting_down || !pipeline_work_queue.empty();
				});

				if (shutting_down)
					break;

				work_item = pipeline_work_queue.front();
				pipeline_work_queue.pop();
			}
			auto idle_end_time = chrono::steady_clock::now();
			auto duration_ns = chrono::duration_cast<chrono::nanoseconds>(idle_end_time - idle_start_time).count();
			idle_ns += duration_ns;

			switch (work_item.tag)
			{
			case RESOURCE_SHADER_MODULE:
			{
				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.shader_module != VK_NULL_HANDLE)
						vkDestroyShaderModule(device->get_device(), *work_item.hash_map_entry.shader_module, nullptr);
					*work_item.hash_map_entry.shader_module = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();
					if (vkCreateShaderModule(device->get_device(), work_item.create_info.shader_module_create_info,
					                         nullptr, work_item.output.shader_module) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();
						shader_ns += duration_ns;
						shader_count++;
						*work_item.hash_map_entry.shader_module = *work_item.output.shader_module;

						if (robustness)
							shader_module_to_hash[*work_item.output.shader_module] = work_item.hash;
					}
					else
					{
						LOGE("Failed to create shader module for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}
				break;
			}

			case RESOURCE_GRAPHICS_PIPELINE:
			{
				if (work_item.contributes_to_index)
					thread_current_graphics_index++;

				// Make sure to iterate the index so main thread and worker threads
				// have a coherent idea of replayer state.
				if (!work_item.create_info.graphics_create_info)
				{
					if (opts.control_block)
						opts.control_block->skipped_graphics.fetch_add(1, std::memory_order_relaxed);
					break;
				}

				if (robustness)
				{
					num_failed_module_hashes = work_item.create_info.graphics_create_info->stageCount;
					for (unsigned i = 0; i < work_item.create_info.graphics_create_info->stageCount; i++)
					{
						VkShaderModule module = work_item.create_info.graphics_create_info->pStages[i].module;
						failed_module_hashes[i] = shader_module_to_hash[module];
					}
				}

				if ((work_item.create_info.graphics_create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0)
				{
					// This pipeline failed for some reason, don't try to compile this one either.
					if (work_item.create_info.graphics_create_info->basePipelineHandle == VK_NULL_HANDLE)
					{
						*work_item.output.pipeline = VK_NULL_HANDLE;
						LOGE("Invalid derivative pipeline!\n");
						break;
					}
				}

				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
						vkDestroyPipeline(device->get_device(), *work_item.hash_map_entry.pipeline, nullptr);
					*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();

#ifdef SIMULATE_UNSTABLE_DRIVER
					spurious_crash();
#endif

					if (vkCreateGraphicsPipelines(device->get_device(), pipeline_cache, 1, work_item.create_info.graphics_create_info,
					                              nullptr, work_item.output.pipeline) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

						if (work_item.contributes_to_index)
						{
							graphics_ns += duration_ns;
							graphics_count++;
						}

						*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;

						if (opts.control_block && i == 0 && work_item.contributes_to_index)
							opts.control_block->successful_graphics.fetch_add(1, std::memory_order_relaxed);
					}
					else
					{
						LOGE("Failed to create graphics pipeline for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}
				break;
			}

			case RESOURCE_COMPUTE_PIPELINE:
			{
				if (work_item.contributes_to_index)
					thread_current_compute_index++;

				// Make sure to iterate the index so main thread and worker threads
				// have a coherent idea of replayer state.
				if (!work_item.create_info.compute_create_info)
				{
					if (opts.control_block)
						opts.control_block->skipped_compute.fetch_add(1, std::memory_order_relaxed);
					break;
				}

				if (robustness)
				{
					num_failed_module_hashes = 1;
					VkShaderModule module = work_item.create_info.compute_create_info->stage.module;
					failed_module_hashes[0] = shader_module_to_hash[module];
				}

				if ((work_item.create_info.compute_create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0)
				{
					// This pipeline failed for some reason, don't try to compile this one either.
					if (work_item.create_info.compute_create_info->basePipelineHandle == VK_NULL_HANDLE)
					{
						*work_item.output.pipeline = VK_NULL_HANDLE;
						break;
					}
				}

				for (unsigned i = 0; i < loop_count; i++)
				{
					// Avoid leak.
					if (*work_item.hash_map_entry.pipeline != VK_NULL_HANDLE)
						vkDestroyPipeline(device->get_device(), *work_item.hash_map_entry.pipeline, nullptr);
					*work_item.hash_map_entry.pipeline = VK_NULL_HANDLE;

					auto start_time = chrono::steady_clock::now();

#ifdef SIMULATE_UNSTABLE_DRIVER
					spurious_crash();
#endif

					if (vkCreateComputePipelines(device->get_device(), pipeline_cache, 1,
					                             work_item.create_info.compute_create_info,
					                             nullptr, work_item.output.pipeline) == VK_SUCCESS)
					{
						auto end_time = chrono::steady_clock::now();
						duration_ns = chrono::duration_cast<chrono::nanoseconds>(end_time - start_time).count();

						if (work_item.contributes_to_index)
						{
							compute_ns += duration_ns;
							compute_count++;
						}

						*work_item.hash_map_entry.pipeline = *work_item.output.pipeline;

						if (opts.control_block && i == 0 && work_item.contributes_to_index)
							opts.control_block->successful_compute.fetch_add(1, std::memory_order_relaxed);
					}
					else
					{
						LOGE("Failed to create compute pipeline for hash 0x%llx.\n",
						     static_cast<unsigned long long>(work_item.hash));
					}
				}
				break;
			}

			default:
				break;
			}

			idle_start_time = chrono::steady_clock::now();
			{

				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				completed_count++;
				if (completed_count == queued_count) // Makes sense to signal main thread now.
					work_done_condition.notify_one();

			}
			idle_end_time = chrono::steady_clock::now();
			duration_ns = chrono::duration_cast<chrono::nanoseconds>(idle_end_time - idle_start_time).count();
			idle_ns += duration_ns;
		}

		graphics_pipeline_count.fetch_add(graphics_count, std::memory_order_relaxed);
		graphics_pipeline_ns.fetch_add(graphics_ns, std::memory_order_relaxed);
		compute_pipeline_count.fetch_add(compute_count, std::memory_order_relaxed);
		compute_pipeline_ns.fetch_add(compute_ns, std::memory_order_relaxed);
		shader_module_count.fetch_add(shader_count, std::memory_order_relaxed);
		shader_module_ns.fetch_add(shader_ns, std::memory_order_relaxed);
		total_idle_ns.fetch_add(idle_ns, std::memory_order_relaxed);
		auto thread_end_time = chrono::steady_clock::now();
		thread_total_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(thread_end_time - thread_start_time).count(),
		                          std::memory_order_relaxed);
	}

	void flush_pipeline_cache()
	{
		if (device && pipeline_cache)
		{
			if (!opts.on_disk_pipeline_cache_path.empty())
			{
				size_t pipeline_cache_size = 0;
				if (vkGetPipelineCacheData(device->get_device(), pipeline_cache, &pipeline_cache_size, nullptr) == VK_SUCCESS)
				{
					vector<uint8_t> pipeline_buffer(pipeline_cache_size);
					if (vkGetPipelineCacheData(device->get_device(), pipeline_cache, &pipeline_cache_size, pipeline_buffer.data()) == VK_SUCCESS)
					{
						// This isn't safe to do in a signal handler, but it's unlikely to be a problem in practice.
						FILE *file = fopen(opts.on_disk_pipeline_cache_path.c_str(), "wb");
						if (!file || fwrite(pipeline_buffer.data(), 1, pipeline_buffer.size(), file) != pipeline_buffer.size())
							LOGE("Failed to write pipeline cache data to disk.\n");
						if (file)
							fclose(file);
					}
				}
			}
			vkDestroyPipelineCache(device->get_device(), pipeline_cache, nullptr);
			pipeline_cache = VK_NULL_HANDLE;
		}
	}

	void tear_down_threads()
	{
		// Signal that it's time for threads to die.
		{
			lock_guard<mutex> lock(pipeline_work_queue_mutex);
			shutting_down = true;
			work_available_condition.notify_all();
		}

		for (auto &thread : thread_pool)
			if (thread.joinable())
				thread.join();
		thread_pool.clear();
	}

	~ThreadedReplayer()
	{
		tear_down_threads();
		flush_pipeline_cache();

		for (auto &sampler : samplers)
			if (sampler.second)
				vkDestroySampler(device->get_device(), sampler.second, nullptr);
		for (auto &layout : layouts)
			if (layout.second)
				vkDestroyDescriptorSetLayout(device->get_device(), layout.second, nullptr);
		for (auto &pipeline_layout : pipeline_layouts)
			if (pipeline_layout.second)
				vkDestroyPipelineLayout(device->get_device(), pipeline_layout.second, nullptr);
		for (auto &shader_module : shader_modules)
			if (shader_module.second)
				vkDestroyShaderModule(device->get_device(), shader_module.second, nullptr);
		for (auto &render_pass : render_passes)
			if (render_pass.second)
				vkDestroyRenderPass(device->get_device(), render_pass.second, nullptr);
		for (auto &pipeline : compute_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
		for (auto &pipeline : graphics_pipelines)
			if (pipeline.second)
				vkDestroyPipeline(device->get_device(), pipeline.second, nullptr);
	}

	bool validate_pipeline_cache_header(const vector<uint8_t> &blob)
	{
		if (blob.size() < 16 + VK_UUID_SIZE)
		{
			LOGI("Pipeline cache header is too small.\n");
			return false;
		}

		const auto read_le = [&](unsigned offset) -> uint32_t {
			return uint32_t(blob[offset + 0]) |
				(uint32_t(blob[offset + 1]) << 8) |
				(uint32_t(blob[offset + 2]) << 16) |
				(uint32_t(blob[offset + 3]) << 24);
		};

		auto length = read_le(0);
		if (length != 16 + VK_UUID_SIZE)
		{
			LOGI("Length of pipeline cache header is not as expected.\n");
			return false;
		}

		auto version = read_le(4);
		if (version != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
		{
			LOGI("Version of pipeline cache header is not 1.\n");
			return false;
		}

		VkPhysicalDeviceProperties props = {};
		vkGetPhysicalDeviceProperties(device->get_gpu(), &props);
		if (props.vendorID != read_le(8))
		{
			LOGI("Mismatch of vendorID and cache vendorID.\n");
			return false;
		}

		if (props.deviceID != read_le(12))
		{
			LOGI("Mismatch of deviceID and cache deviceID.\n");
			return false;
		}

		if (memcmp(props.pipelineCacheUUID, blob.data() + 16, VK_UUID_SIZE) != 0)
		{
			LOGI("Mismatch between pipelineCacheUUID.\n");
			return false;
		}

		return true;
	}

	void set_application_info(const VkApplicationInfo *app, const VkPhysicalDeviceFeatures2 *features) override
	{
		// TODO: Could use this to create multiple VkDevices for replay as necessary if app changes.

		if (!device_was_init)
		{
			// Now we can init the device with correct app info.
			device_was_init = true;
			device.reset(new VulkanDevice);
			device_opts.application_info = app;
			device_opts.features = features;
			device_opts.need_disasm = false;
			auto start_device = chrono::steady_clock::now();
			if (!device->init_device(device_opts))
			{
				LOGE("Failed to create Vulkan device, bailing ...\n");
				exit(EXIT_FAILURE);
			}

			if (opts.pipeline_cache)
			{
				VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
				vector<uint8_t> on_disk_cache;

				// Try to load on-disk cache.
				if (!opts.on_disk_pipeline_cache_path.empty())
				{
					FILE *file = fopen(opts.on_disk_pipeline_cache_path.c_str(), "rb");
					if (file)
					{
						fseek(file, 0, SEEK_END);
						size_t len = ftell(file);
						rewind(file);

						if (len != 0)
						{
							on_disk_cache.resize(len);
							if (fread(on_disk_cache.data(), 1, len, file) == len)
							{
								if (validate_pipeline_cache_header(on_disk_cache))
								{
									info.pInitialData = on_disk_cache.data();
									info.initialDataSize = on_disk_cache.size();
								}
								else
									LOGI("Failed to validate pipeline cache. Creating a blank one.\n");
							}
						}
					}
				}

				if (vkCreatePipelineCache(device->get_device(), &info, nullptr, &pipeline_cache) != VK_SUCCESS)
				{
					LOGE("Failed to create pipeline cache, trying to create a blank one.\n");
					info.initialDataSize = 0;
					info.pInitialData = nullptr;
					if (vkCreatePipelineCache(device->get_device(), &info, nullptr, &pipeline_cache) != VK_SUCCESS)
					{
						LOGE("Failed to create pipeline cache.\n");
						pipeline_cache = VK_NULL_HANDLE;
					}
				}
			}

			auto end_device = chrono::steady_clock::now();
			long time_ms = chrono::duration_cast<chrono::milliseconds>(end_device - start_device).count();
			LOGI("Creating Vulkan device took: %ld ms\n", time_ms);

			if (app)
			{
				LOGI("Replaying for application:\n");
				LOGI("  apiVersion: %u.%u.%u\n",
				     VK_VERSION_MAJOR(app->apiVersion),
				     VK_VERSION_MINOR(app->apiVersion),
				     VK_VERSION_PATCH(app->apiVersion));
				LOGI("  engineVersion: %u\n", app->engineVersion);
				LOGI("  applicationVersion: %u\n", app->applicationVersion);
				if (app->pEngineName)
					LOGI("  engineName: %s\n", app->pEngineName);
				if (app->pApplicationName)
					LOGI("  applicationName: %s\n", app->pApplicationName);
			}
		}
	}

	bool enqueue_create_sampler(Hash index, const VkSamplerCreateInfo *create_info, VkSampler *sampler) override
	{
		// Playback in-order.
		if (vkCreateSampler(device->get_device(), create_info, nullptr, sampler) != VK_SUCCESS)
		{
			LOGE("Creating sampler %0" PRIX64 " Failed!\n", index);
			return false;
		}
		samplers[index] = *sampler;
		return true;
	}

	bool enqueue_create_descriptor_set_layout(Hash index, const VkDescriptorSetLayoutCreateInfo *create_info, VkDescriptorSetLayout *layout) override
	{
		// Playback in-order.
		if (vkCreateDescriptorSetLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating descriptor set layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_pipeline_layout(Hash index, const VkPipelineLayoutCreateInfo *create_info, VkPipelineLayout *layout) override
	{
		// Playback in-order.
		if (vkCreatePipelineLayout(device->get_device(), create_info, nullptr, layout) != VK_SUCCESS)
		{
			LOGE("Creating pipeline layout %0" PRIX64 " Failed!\n", index);
			return false;
		}
		pipeline_layouts[index] = *layout;
		return true;
	}

	bool enqueue_create_render_pass(Hash index, const VkRenderPassCreateInfo *create_info, VkRenderPass *render_pass) override
	{
		// Playback in-order.
		if (vkCreateRenderPass(device->get_device(), create_info, nullptr, render_pass) != VK_SUCCESS)
		{
			LOGE("Creating render pass %0" PRIX64 " Failed!\n", index);
			return false;
		}
		render_passes[index] = *render_pass;
		return true;
	}

	bool enqueue_create_shader_module(Hash hash, const VkShaderModuleCreateInfo *create_info, VkShaderModule *module) override
	{
		if (masked_shader_modules.count(hash))
		{
			*module = VK_NULL_HANDLE;
			return true;
		}

		PipelineWorkItem work_item;
		work_item.hash = hash;
		work_item.tag = RESOURCE_SHADER_MODULE;
		work_item.output.shader_module = module;
		// Pointer to value in std::unordered_map remains fixed per spec (node-based).
		work_item.hash_map_entry.shader_module = &shader_modules[hash];
		work_item.create_info.shader_module_create_info = create_info;

		{
			// Pipeline parsing with pipeline creation.
			lock_guard<mutex> lock(pipeline_work_queue_mutex);
			pipeline_work_queue.push(work_item);
			work_available_condition.notify_one();
			queued_count++;
		}

		return true;
	}

	bool enqueue_create_compute_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		bool derived = (create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0;
		if (derived && create_info->basePipelineHandle == VK_NULL_HANDLE)
			LOGE("Creating a derived pipeline with NULL handle.\n");

		if (derived)
		{
			// We don't have the appropriate base pipeline yet, so defer it.
			derived_compute.push_back({ const_cast<VkComputePipelineCreateInfo *>(create_info), hash, pipeline, true });
		}
		else if (compute_pipeline_index >= opts.start_compute_index &&
		         compute_pipeline_index < opts.end_compute_index)
		{
			PipelineWorkItem work_item = {};
			work_item.hash = hash;
			work_item.tag = RESOURCE_COMPUTE_PIPELINE;
			work_item.output.pipeline = pipeline;

			if (create_info->stage.module != VK_NULL_HANDLE)
			{
				// Pointer to value in std::unordered_map remains fixed per spec (node-based).
				work_item.hash_map_entry.pipeline = &compute_pipelines[hash];
				work_item.create_info.compute_create_info = create_info;
			}
			//else
			//	LOGE("Skipping replay of compute pipeline index %u.\n", compute_pipeline_index);

			{
				// Pipeline parsing with pipeline creation.
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				pipeline_work_queue.push(work_item);
				work_available_condition.notify_one();
				queued_count++;
			}
		}
		else
		{
			// We might have to replay this later if a derived pipeline required this
			// pipeline after all.
			if ((create_info->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) != 0)
			{
				potential_compute_parent[hash] = {
					const_cast<VkComputePipelineCreateInfo *>(create_info),
					hash,
					pipeline,
					false
				};
			}
			*pipeline = VK_NULL_HANDLE;
		}

		// Makes it so that derived pipelines are indexed last. We need a stable pipeline index order which also allows
		// us to pipeline parsing with pipeline compilation.
		if (!derived)
			compute_pipeline_index++;
		return true;
	}

	bool enqueue_create_graphics_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline) override
	{
		bool derived = (create_info->flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) != 0;
		if (derived && create_info->basePipelineHandle == VK_NULL_HANDLE)
			LOGE("Creating a derived pipeline with NULL handle.\n");

		if (derived)
		{
			// We don't have the appropriate base pipeline yet, so defer it.
			derived_graphics.push_back({ const_cast<VkGraphicsPipelineCreateInfo *>(create_info), hash, pipeline, true });
		}
		else if (graphics_pipeline_index >= opts.start_graphics_index &&
		         graphics_pipeline_index < opts.end_graphics_index)
		{
			bool valid_handles = true;
			for (uint32_t i = 0; i < create_info->stageCount; i++)
				if (create_info->pStages[i].module == VK_NULL_HANDLE)
					valid_handles = false;

			PipelineWorkItem work_item = {};
			work_item.hash = hash;
			work_item.tag = RESOURCE_GRAPHICS_PIPELINE;
			work_item.output.pipeline = pipeline;

			if (valid_handles)
			{
				// Pointer to value in std::unordered_map remains fixed per spec (node-based).
				work_item.hash_map_entry.pipeline = &graphics_pipelines[hash];
				work_item.create_info.graphics_create_info = create_info;
			}
			//else
			//	LOGE("Skipping replay of graphics pipeline index %u.\n", graphics_pipeline_index);

			{
				// Pipeline parsing with pipeline creation.
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				pipeline_work_queue.push(work_item);
				work_available_condition.notify_one();
				queued_count++;
			}
		}
		else
		{
			// We might have to replay this later if a derived pipeline requires this
			// pipeline after all.
			if ((create_info->flags & VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT) != 0)
			{
				potential_graphics_parent[hash] = {
					const_cast<VkGraphicsPipelineCreateInfo *>(create_info),
					hash,
					pipeline,
					false,
				};
			}
			*pipeline = VK_NULL_HANDLE;
		}

		// Makes it so that derived pipelines are indexed last. We need a stable pipeline index order which also allows
		// us to pipeline parsing with pipeline compilation.
		if (!derived)
			graphics_pipeline_index++;

		return true;
	}

	template <typename DerivedInfo>
	bool resolve_derived_pipelines(vector<DerivedInfo> &derived, unordered_map<Hash, DerivedInfo> &potential_parent,
	                               unordered_map<Hash, VkPipeline> &pipelines)
	{
		// Figure out which of the potential parent pipelines we really need.
		for (auto &d : derived)
		{
			auto itr = potential_parent.find((Hash)d.info->basePipelineHandle);
			if (itr != end(potential_parent))
			{
				// No dependencies, enqueue it, but it must not contribute to the pipeline index in any way, since
				// it lived outside our expected replay range.
				enqueue_pipeline(itr->second.hash, itr->second.info, itr->second.pipeline, itr->second.contributes_to_index);
				potential_parent.erase(itr);
			}
		}

		// We will have to potentially iterate a bit here.
		// It is technically possible to have a deep hierarchy of derived pipelines, but we don't optimize for that case here.
		while (!derived.empty())
		{
			// Go over all pipelines. If there are no further dependencies to resolve, we can go ahead and queue them up.
			// If an entry exists in graphics_pipelines, we have queued up that hash earlier, but it might not be done compiling yet.
			auto itr = unstable_remove_if(begin(derived), end(derived), [&](const DerivedInfo &info) -> bool {
				Hash hash = (Hash)info.info->basePipelineHandle;
				return pipelines.count(hash) != 0;
			});

			if (itr == end(derived)) // We cannot progress ... This shouldn't happen.
			{
				LOGE("Nothing more to do in resolve_derived_pipelines, but there are still pipelines left to replay.\n");
				return false;
			}

			sync_worker_threads();

			// Now we can enqueue with correct pipeline handles.
			for (auto i = itr; i != end(derived); ++i)
			{
				i->info->basePipelineHandle = pipelines[(Hash)i->info->basePipelineHandle];
				if (!enqueue_pipeline(i->hash, i->info, i->pipeline, i->contributes_to_index))
					return false;
			}

			// The pipelines are now in-flight, try resolving new dependencies in next iteration.
			derived.erase(itr, end(derived));
		}

		return true;
	}

	bool resolve_derived_compute_pipelines()
	{
		return resolve_derived_pipelines(derived_compute, potential_compute_parent, compute_pipelines);
	}

	bool resolve_derived_graphics_pipelines()
	{
		return resolve_derived_pipelines(derived_graphics, potential_graphics_parent, graphics_pipelines);
	}

	bool enqueue_pipeline(Hash hash, const VkComputePipelineCreateInfo *create_info, VkPipeline *pipeline,
	                      bool contributes_to_index)
	{
		if (!contributes_to_index ||
		    (compute_pipeline_index >= opts.start_compute_index &&
		     compute_pipeline_index < opts.end_compute_index))
		{
			PipelineWorkItem work_item = {};
			work_item.hash = hash;
			work_item.tag = RESOURCE_COMPUTE_PIPELINE;
			work_item.output.pipeline = pipeline;
			work_item.contributes_to_index = contributes_to_index;

			if (create_info->stage.module != VK_NULL_HANDLE)
			{
				// Pointer to value in std::unordered_map remains fixed per spec (node-based).
				work_item.hash_map_entry.pipeline = &compute_pipelines[hash];
				work_item.create_info.compute_create_info = create_info;
			}
			//else
			//	LOGE("Skipping replay of graphics pipeline index %u.\n", graphics_pipeline_index);

			{
				// Pipeline parsing with pipeline creation.
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				pipeline_work_queue.push(work_item);
				work_available_condition.notify_one();
				queued_count++;
			}
		}

		if (contributes_to_index)
			compute_pipeline_index++;

		return true;
	}

	bool enqueue_pipeline(Hash hash, const VkGraphicsPipelineCreateInfo *create_info, VkPipeline *pipeline,
	                      bool contributes_to_index)
	{
		if (!contributes_to_index ||
		    (graphics_pipeline_index >= opts.start_graphics_index &&
		     graphics_pipeline_index < opts.end_graphics_index))
		{
			bool valid_handles = true;
			for (uint32_t i = 0; i < create_info->stageCount; i++)
				if (create_info->pStages[i].module == VK_NULL_HANDLE)
					valid_handles = false;

			PipelineWorkItem work_item = {};
			work_item.hash = hash;
			work_item.tag = RESOURCE_GRAPHICS_PIPELINE;
			work_item.output.pipeline = pipeline;
			work_item.contributes_to_index = contributes_to_index;

			if (valid_handles)
			{
				// Pointer to value in std::unordered_map remains fixed per spec (node-based).
				work_item.hash_map_entry.pipeline = &graphics_pipelines[hash];
				work_item.create_info.graphics_create_info = create_info;
			}
			//else
			//	LOGE("Skipping replay of graphics pipeline index %u.\n", graphics_pipeline_index);

			{
				// Pipeline parsing with pipeline creation.
				lock_guard<mutex> lock(pipeline_work_queue_mutex);
				pipeline_work_queue.push(work_item);
				work_available_condition.notify_one();
				queued_count++;
			}
		}

		if (contributes_to_index)
			graphics_pipeline_index++;

		return true;
	}

	void sync_threads() override
	{
		sync_worker_threads();
	}

	// Support ignoring shader module which are known to cause crashes.
	void mask_shader_module(Hash hash)
	{
		masked_shader_modules.insert(hash);
	}

	const vector<thread> &get_threads() const
	{
		return thread_pool;
	}

	void emergency_teardown()
	{
#ifdef SIMULATE_UNSTABLE_DRIVER
		spurious_deadlock();
#endif
		flush_pipeline_cache();
		device.reset();
	}

	Options opts;
	unsigned graphics_pipeline_index = 0;
	unsigned compute_pipeline_index = 0;

	std::unordered_map<Hash, VkSampler> samplers;
	std::unordered_map<Hash, VkDescriptorSetLayout> layouts;
	std::unordered_map<Hash, VkPipelineLayout> pipeline_layouts;
	std::unordered_map<Hash, VkShaderModule> shader_modules;
	std::unordered_map<Hash, VkRenderPass> render_passes;
	std::unordered_map<Hash, VkPipeline> compute_pipelines;
	std::unordered_map<Hash, VkPipeline> graphics_pipelines;
	std::unordered_set<Hash> masked_shader_modules;
	std::unordered_map<VkShaderModule, Hash> shader_module_to_hash;
	VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

	// VALVE: multi-threaded work queue for replayer
	struct PipelineWorkItem
	{
		Hash hash = 0;
		ResourceTag tag = RESOURCE_COUNT;
		bool contributes_to_index = true;

		union
		{
			const VkGraphicsPipelineCreateInfo *graphics_create_info;
			const VkComputePipelineCreateInfo *compute_create_info;
			const VkShaderModuleCreateInfo *shader_module_create_info;
		} create_info = {};

		union
		{
			VkPipeline *pipeline;
			VkShaderModule *shader_module;
		} output = {};

		union
		{
			VkPipeline *pipeline;
			VkShaderModule *shader_module;
		} hash_map_entry = {};
	};

	unsigned num_worker_threads = 0;
	unsigned loop_count = 0;
	unsigned queued_count = 0;
	unsigned completed_count = 0;
	std::vector<std::thread> thread_pool;
	std::mutex pipeline_work_queue_mutex;
	std::queue<PipelineWorkItem> pipeline_work_queue;
	std::condition_variable work_available_condition;
	std::condition_variable work_done_condition;

	std::unordered_map<Hash, DeferredGraphicsInfo> potential_graphics_parent;
	std::unordered_map<Hash, DeferredComputeInfo> potential_compute_parent;
	std::vector<DeferredGraphicsInfo> derived_graphics;
	std::vector<DeferredComputeInfo> derived_compute;

	// Feed statistics from the worker threads.
	std::atomic<std::uint64_t> graphics_pipeline_ns;
	std::atomic<std::uint64_t> compute_pipeline_ns;
	std::atomic<std::uint64_t> shader_module_ns;
	std::atomic<std::uint64_t> total_idle_ns;
	std::atomic<std::uint64_t> thread_total_ns;
	std::atomic<std::uint32_t> graphics_pipeline_count;
	std::atomic<std::uint32_t> compute_pipeline_count;
	std::atomic<std::uint32_t> shader_module_count;

	bool shutting_down = false;

	unique_ptr<VulkanDevice> device;
	bool device_was_init = false;
	VulkanDevice::Options device_opts;

	// Crash recovery.
	Hash failed_module_hashes[6] = {};
	unsigned num_failed_module_hashes = 0;
	unsigned thread_current_graphics_index = 0;
	unsigned thread_current_compute_index = 0;
	bool robustness = false;
};

static void print_help()
{
#ifndef NO_ROBUST_REPLAYER
#ifdef _WIN32
#define EXTRA_OPTIONS \
	"\t[--slave-process]\n" \
	"\t[--master-process]\n" \
	"\t[--timeout <seconds>]\n" \
	"\t[--progress]\n" \
	"\t[--quiet-slave]\n" \
	"\t[--shm-name <name>]\n\t[--shm-mutex-name <name>]\n"
#else
#define EXTRA_OPTIONS \
	"\t[--slave-process]\n" \
	"\t[--master-process]\n" \
	"\t[--timeout <seconds>]\n" \
	"\t[--progress]\n" \
	"\t[--quiet-slave]\n" \
	"\t[--shm-fd <fd>]\n"
#endif
#else
#define EXTRA_OPTIONS ""
#endif
	LOGI("fossilize-replay\n"
	     "\t[--help]\n"
	     "\t[--device-index <index>]\n"
	     "\t[--enable-validation]\n"
	     "\t[--pipeline-cache]\n"
	     "\t[--num-threads <count>]\n"
	     "\t[--loop <count>]\n"
	     "\t[--on-disk-pipeline-cache <path>]\n"
	     "\t[--graphics-pipeline-range <start> <end>]\n"
	     "\t[--compute-pipeline-range <start> <end>]\n"
	     EXTRA_OPTIONS
	     "\t<Database>\n");
}

#ifndef NO_ROBUST_REPLAYER
static void log_progress(const ExternalReplayer::Progress &progress)
{
	LOGI("=================\n");
	LOGI(" Progress report:\n");
	LOGI("   Graphics %u / %u, skipped %u\n", progress.graphics.completed, progress.graphics.total, progress.graphics.skipped);
	LOGI("   Compute %u / %u, skipped %u\n", progress.compute.completed, progress.compute.total, progress.compute.skipped);
	LOGI("   Modules %u, skipped %u\n", progress.total_modules, progress.banned_modules);
	LOGI("   Clean crashes %u\n", progress.clean_crashes);
	LOGI("   Dirty crashes %u\n", progress.dirty_crashes);
	LOGI("=================\n");
}

static void log_faulty_modules(ExternalReplayer &replayer)
{
	size_t count;
	if (!replayer.get_faulty_spirv_modules(&count, nullptr))
		return;
	vector<Hash> hashes(count);
	if (!replayer.get_faulty_spirv_modules(&count, hashes.data()))
		return;

	for (auto &h : hashes)
		LOGI("Detected faulty SPIR-V module: %llx\n", static_cast<unsigned long long>(h));
}

static int run_progress_process(const VulkanDevice::Options &,
                                const ThreadedReplayer::Options &replayer_opts,
                                const string &db_path, int timeout)
{
	ExternalReplayer::Options opts = {};
	opts.on_disk_pipeline_cache = replayer_opts.on_disk_pipeline_cache_path.empty() ?
		nullptr : replayer_opts.on_disk_pipeline_cache_path.c_str();
	opts.pipeline_cache = replayer_opts.pipeline_cache;
	opts.num_threads = replayer_opts.num_threads;
	opts.quiet = true;
	opts.database = db_path.c_str();
	opts.external_replayer_path = nullptr;

	ExternalReplayer replayer;
	if (!replayer.start(opts))
	{
		LOGE("Failed to start external replayer.\n");
		return EXIT_FAILURE;
	}

	bool has_killed = false;
	auto start_time = std::chrono::steady_clock::now();

	for (;;)
	{
		if (!has_killed && timeout > 0)
		{
			auto current_time = std::chrono::steady_clock::now();
			auto delta = current_time - start_time;
			if (std::chrono::duration_cast<std::chrono::seconds>(delta).count() >= timeout)
			{
				LOGE("Killing process due to timeout.\n");
				replayer.kill();
				has_killed = true;
			}
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		ExternalReplayer::Progress progress = {};
		auto result = replayer.poll_progress(progress);

		if (replayer.is_process_complete(nullptr))
		{
			if (result != ExternalReplayer::PollResult::ResultNotReady)
				log_progress(progress);
			log_faulty_modules(replayer);
			return replayer.wait();
		}

		switch (result)
		{
		case ExternalReplayer::PollResult::Error:
			return EXIT_FAILURE;

		case ExternalReplayer::PollResult::ResultNotReady:
			break;

		case ExternalReplayer::PollResult::Complete:
		case ExternalReplayer::PollResult::Running:
			log_progress(progress);
			if (result == ExternalReplayer::PollResult::Complete)
			{
				log_faulty_modules(replayer);
				return replayer.wait();
			}
			break;
		}
	}
}
#endif

static int run_normal_process(ThreadedReplayer &replayer, const string &db_path)
{
	auto start_time = chrono::steady_clock::now();
	auto start_create_archive = chrono::steady_clock::now();
	auto resolver = unique_ptr<DatabaseInterface>(create_database(db_path.c_str(), DatabaseMode::ReadOnly));
	auto end_create_archive = chrono::steady_clock::now();

	auto start_prepare = chrono::steady_clock::now();
	if (!resolver->prepare())
	{
		LOGE("Failed to prepare database.\n");
		return EXIT_FAILURE;
	}
	auto end_prepare = chrono::steady_clock::now();

	StateReplayer state_replayer;
	state_replayer.set_resolve_derivative_pipeline_handles(false);

	vector<Hash> resource_hashes;
	vector<uint8_t> state_json;

	static const ResourceTag playback_order[] = {
		RESOURCE_APPLICATION_INFO, // This will create the device, etc.
		RESOURCE_SHADER_MODULE, // Kick off shader modules first since it can be done in a thread while we deal with trivial objects.
		RESOURCE_SAMPLER, // Trivial, run in main thread.
		RESOURCE_DESCRIPTOR_SET_LAYOUT, // Trivial, run in main thread
		RESOURCE_PIPELINE_LAYOUT, // Trivial, run in main thread
		RESOURCE_RENDER_PASS, // Trivial, run in main thread
		RESOURCE_GRAPHICS_PIPELINE, // Multi-threaded
		RESOURCE_COMPUTE_PIPELINE, // Multi-threaded
	};

	static const char *tag_names[] = {
		"AppInfo",
		"Sampler",
		"Descriptor Set Layout",
		"Pipeline Layout",
		"Shader Module",
		"Render Pass",
		"Graphics Pipeline",
		"Compute Pipeline",
	};

	for (auto &tag : playback_order)
	{
		auto main_thread_start = std::chrono::steady_clock::now();
		size_t tag_total_size = 0;
		size_t tag_total_size_compressed = 0;
		size_t resource_hash_count = 0;

		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, nullptr))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		if (tag == RESOURCE_GRAPHICS_PIPELINE)
		{
			replayer.derived_graphics.reserve(resource_hash_count);
			replayer.potential_graphics_parent.reserve(resource_hash_count);
		}
		else if (tag == RESOURCE_COMPUTE_PIPELINE)
		{
			replayer.derived_compute.reserve(resource_hash_count);
			replayer.potential_compute_parent.reserve(resource_hash_count);
		}

		resource_hashes.resize(resource_hash_count);

		if (!resolver->get_hash_list_for_resource_tag(tag, &resource_hash_count, resource_hashes.data()))
		{
			LOGE("Failed to get list of resource hashes.\n");
			return EXIT_FAILURE;
		}

		for (auto &hash : resource_hashes)
		{
			size_t state_json_size = 0;
			if (!resolver->read_entry(tag, hash, &state_json_size, nullptr, PAYLOAD_READ_RAW_FOSSILIZE_DB_BIT))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}
			tag_total_size_compressed += state_json_size;

			if (!resolver->read_entry(tag, hash, &state_json_size, nullptr, 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			state_json.resize(state_json_size);
			tag_total_size += state_json_size;

			if (!resolver->read_entry(tag, hash, &state_json_size, state_json.data(), 0))
			{
				LOGE("Failed to load blob from cache.\n");
				return EXIT_FAILURE;
			}

			try
			{
				state_replayer.parse(replayer, resolver.get(), state_json.data(), state_json.size());
			}
			catch (const exception &e)
			{
				LOGE("StateReplayer threw exception parsing (tag: %d, hash: 0x%llx): %s\n", tag, static_cast<unsigned long long>(hash), e.what());
			}
		}

		LOGI("Total binary size for %s: %llu (%llu compressed)\n", tag_names[tag],
		     static_cast<unsigned long long>(tag_total_size),
		     static_cast<unsigned long long>(tag_total_size_compressed));

		auto main_thread_end = std::chrono::steady_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(main_thread_end - main_thread_start).count();
		LOGI("Total time decoding %s in main thread: %.3f s\n", tag_names[tag], duration * 1e-9);

		// Before continuing with pipelines, make sure the threaded shader modules have been created.
		if (tag == RESOURCE_RENDER_PASS)
			replayer.sync_worker_threads();
		else if (tag == RESOURCE_GRAPHICS_PIPELINE && !replayer.derived_graphics.empty())
			replayer.resolve_derived_graphics_pipelines();
		else if (tag == RESOURCE_COMPUTE_PIPELINE && !replayer.derived_compute.empty())
			replayer.resolve_derived_compute_pipelines();
	}

	// VALVE: drain all outstanding pipeline compiles
	replayer.sync_worker_threads();
	replayer.tear_down_threads();

	unsigned long total_size =
		replayer.samplers.size() +
		replayer.layouts.size() +
		replayer.pipeline_layouts.size() +
		replayer.shader_modules.size() +
		replayer.render_passes.size() +
		replayer.compute_pipelines.size() +
		replayer.graphics_pipelines.size();

	long elapsed_ms_prepare = chrono::duration_cast<chrono::milliseconds>(end_prepare - start_prepare).count();
	long elapsed_ms_read_archive = chrono::duration_cast<chrono::milliseconds>(end_create_archive - start_create_archive).count();
	long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - start_time).count();

	LOGI("Opening archive took %ld ms:\n", elapsed_ms_read_archive);
	LOGI("Parsing archive took %ld ms:\n", elapsed_ms_prepare);

	LOGI("Playing back %u shader modules took %.3f s (accumulated time)\n",
	     replayer.shader_module_count.load(),
	     replayer.shader_module_ns.load() * 1e-9);

	LOGI("Playing back %u graphics pipelines took %.3f s (accumulated time)\n",
	     replayer.graphics_pipeline_count.load(),
	     replayer.graphics_pipeline_ns.load() * 1e-9);

	LOGI("Playing back %u compute pipelines took %.3f s (accumulated time)\n",
	     replayer.compute_pipeline_count.load(),
	     replayer.compute_pipeline_ns.load() * 1e-9);

	LOGI("Threads were idling in total for %.3f s (accumulated time)\n",
	     replayer.total_idle_ns.load() * 1e-9);

	LOGI("Threads were active in total for %.3f s (accumulated time)\n",
	     replayer.thread_total_ns.load() * 1e-9);

	LOGI("Replayed %lu objects in %ld ms:\n", total_size, elapsed_ms);
	LOGI("  samplers:              %7lu\n", (unsigned long)replayer.samplers.size());
	LOGI("  descriptor set layouts:%7lu\n", (unsigned long)replayer.layouts.size());
	LOGI("  pipeline layouts:      %7lu\n", (unsigned long)replayer.pipeline_layouts.size());
	LOGI("  shader modules:        %7lu\n", (unsigned long)replayer.shader_modules.size());
	LOGI("  render passes:         %7lu\n", (unsigned long)replayer.render_passes.size());
	LOGI("  compute pipelines:     %7lu\n", (unsigned long)replayer.compute_pipelines.size());
	LOGI("  graphics pipelines:    %7lu\n", (unsigned long)replayer.graphics_pipelines.size());

	return EXIT_SUCCESS;
}

// The implementations are drastically different.
// To simplify build system, just include implementation inline here.
#ifndef NO_ROBUST_REPLAYER
#ifdef __linux__
#include "fossilize_replay_linux.hpp"
#elif defined(_WIN32)
#include "fossilize_replay_windows.hpp"
#else
#error "Unsupported platform."
#endif
#endif

int main(int argc, char *argv[])
{
	string db_path;
	VulkanDevice::Options opts;
	ThreadedReplayer::Options replayer_opts;

#ifndef NO_ROBUST_REPLAYER
	bool master_process = false;
	bool slave_process = false;
	bool quiet_slave = false;
	bool progress = false;
	int timeout = -1;

#ifdef _WIN32
	const char *shm_name = nullptr;
	const char *shm_mutex_name = nullptr;
#else
	int shmem_fd = -1;
#endif
#endif

	CLICallbacks cbs;
	cbs.default_handler = [&](const char *arg) { db_path = arg; };
	cbs.add("--help", [](CLIParser &parser) { print_help(); parser.end(); });
	cbs.add("--device-index", [&](CLIParser &parser) { opts.device_index = parser.next_uint(); });
	cbs.add("--enable-validation", [&](CLIParser &) { opts.enable_validation = true; });
	cbs.add("--pipeline-cache", [&](CLIParser &) { replayer_opts.pipeline_cache = true; });
	cbs.add("--on-disk-pipeline-cache", [&](CLIParser &parser) { replayer_opts.on_disk_pipeline_cache_path = parser.next_string(); });
	cbs.add("--num-threads", [&](CLIParser &parser) { replayer_opts.num_threads = parser.next_uint(); });
	cbs.add("--loop", [&](CLIParser &parser) { replayer_opts.loop_count = parser.next_uint(); });
	cbs.add("--graphics-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_graphics_index = parser.next_uint();
		replayer_opts.end_graphics_index = parser.next_uint();
	});
	cbs.add("--compute-pipeline-range", [&](CLIParser &parser) {
		replayer_opts.start_compute_index = parser.next_uint();
		replayer_opts.end_compute_index = parser.next_uint();
	});

#ifndef NO_ROBUST_REPLAYER
	cbs.add("--quiet-slave", [&](CLIParser &) { quiet_slave = true; });
	cbs.add("--master-process", [&](CLIParser &) { master_process = true; });
	cbs.add("--slave-process", [&](CLIParser &) { slave_process = true; });
	cbs.add("--timeout", [&](CLIParser &parser) { timeout = parser.next_uint(); });
	cbs.add("--progress", [&](CLIParser &) { progress = true; });

#ifdef _WIN32
	cbs.add("--shm-name", [&](CLIParser &parser) { shm_name = parser.next_string(); });
	cbs.add("--shm-mutex-name", [&](CLIParser &parser) { shm_mutex_name = parser.next_string(); });
#else
	cbs.add("--shmem-fd", [&](CLIParser &parser) { shmem_fd = parser.next_uint(); });
#endif
#endif

	cbs.error_handler = [] { print_help(); };

	CLIParser parser(move(cbs), argc - 1, argv + 1);
	if (!parser.parse())
		return EXIT_FAILURE;
	if (parser.is_ended_state())
		return EXIT_SUCCESS;

	if (db_path.empty())
	{
		LOGE("No path to serialized state provided.\n");
		print_help();
		return EXIT_FAILURE;
	}

#ifndef NO_ROBUST_REPLAYER
	// We cannot safely deal with multiple threads here, force one thread.
	if (slave_process)
	{
		if (replayer_opts.num_threads > 1)
			LOGE("Cannot use more than one thread per slave process. Forcing 1 thread.\n");
		replayer_opts.num_threads = 1;
	}

	if (replayer_opts.num_threads < 1)
		replayer_opts.num_threads = 1;

	if (!replayer_opts.on_disk_pipeline_cache_path.empty())
		replayer_opts.pipeline_cache = true;
#endif

	int ret;
#ifndef NO_ROBUST_REPLAYER
	if (progress)
	{
		ret = run_progress_process(opts, replayer_opts, db_path, timeout);
	}
	else if (master_process)
	{
#ifdef _WIN32
		ret = run_master_process(opts, replayer_opts, db_path, quiet_slave, shm_name, shm_mutex_name);
#else
		ret = run_master_process(opts, replayer_opts, db_path, quiet_slave, shmem_fd);
#endif
	}
	else if (slave_process)
	{
#ifdef _WIN32
		ret = run_slave_process(opts, replayer_opts, db_path, shm_name, shm_mutex_name);
#else
		ret = run_slave_process(opts, replayer_opts, db_path);
#endif
	}
	else
#endif
	{
		ThreadedReplayer replayer(opts, replayer_opts);
		ret = run_normal_process(replayer, db_path);
	}

	return ret;
}
