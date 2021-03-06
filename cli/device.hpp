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

#pragma once

#include "volk.h"

namespace Fossilize
{
class VulkanDevice
{
public:
	struct Options
	{
		bool enable_validation = false;
		bool need_disasm = true;
		int device_index = -1;
		const VkApplicationInfo *application_info = nullptr;
		const VkPhysicalDeviceFeatures2 *features = nullptr;
	};
	bool init_device(const Options &opts);

	VulkanDevice() = default;
	~VulkanDevice();
	VulkanDevice(VulkanDevice &&) = delete;
	void operator=(VulkanDevice &&) = delete;

	VkDevice get_device() const
	{
		return device;
	}

	VkPhysicalDevice get_gpu() const
	{
		return gpu;
	}

private:
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice gpu = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkDebugReportCallbackEXT callback = VK_NULL_HANDLE;
};
}
