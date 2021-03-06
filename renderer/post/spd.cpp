/* Copyright (c) 2017-2022 Hans-Kristian Arntzen
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

#include "spd.hpp"
#include "device.hpp"
#include "render_graph.hpp"
#include "render_context.hpp"
#include <algorithm>

namespace Granite
{
bool supports_single_pass_downsample(Vulkan::Device &device, VkFormat format)
{
	auto &features = device.get_device_features();

	bool supports_full_group =
			device.supports_subgroup_size_log2(true, 2, 7);
	bool supports_compute = (features.subgroup_properties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0;

	if (device.get_gpu_properties().limits.maxComputeWorkGroupSize[0] < 256)
		return false;
	if (!features.enabled_features.shaderStorageImageArrayDynamicIndexing)
		return false;

	VkFormatProperties3KHR props3 = { VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3_KHR };
	device.get_format_properties(format, &props3);
	if ((props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT_KHR) == 0)
		return false;
	if ((props3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT_KHR) == 0)
		return false;

	constexpr VkSubgroupFeatureFlags required = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_QUAD_BIT;
	bool supports_quad_basic = (features.subgroup_properties.supportedOperations & required) == required;
	return supports_full_group && supports_compute && supports_quad_basic;
}

void emit_single_pass_downsample(Vulkan::CommandBuffer &cmd, const SPDInfo &info)
{
	cmd.set_program("builtin://shaders/post/ffx-spd/spd.comp",
	                {{"SUBGROUP", 1},
	                 {"SINGLE_INPUT_TAP", 1},
	                 {"COMPONENTS", int(info.num_components)},
	                 {"FILTER_MOD", int(info.filter_mod != nullptr)},
	                 {"Z_TRANSFORM", int(info.z_transform != nullptr)}});

	const Vulkan::StockSampler stock = info.z_transform ?
			Vulkan::StockSampler::NearestClamp : Vulkan::StockSampler::LinearClamp;

	cmd.set_texture(0, 0, *info.input, stock);
	cmd.set_storage_buffer(0, 1, *info.counter_buffer, info.counter_buffer_offset, 4);
	for (unsigned i = 0; i < MaxSPDMips; i++)
		cmd.set_storage_texture(0, 2 + i, *info.output_mips[std::min(i, info.num_mips - 1)]);

	if (info.filter_mod)
	{
		memcpy(cmd.allocate_typed_constant_data<vec4>(1, 0, info.num_mips),
		       info.filter_mod, info.num_mips * sizeof(*info.filter_mod));
	}

	if (info.z_transform)
	{
		memcpy(cmd.allocate_typed_constant_data<mat2>(1, 1, 1),
		       info.z_transform, sizeof(*info.z_transform));
	}

	struct Registers
	{
		uint32_t base_image_resolution[2];
		float inv_resolution[2];
		uint32_t mips;
		uint32_t num_workgroups;
	} push = {};

	push.base_image_resolution[0] = info.output_mips[0]->get_view_width();
	push.base_image_resolution[1] = info.output_mips[0]->get_view_height();
	push.inv_resolution[0] = 1.0f / float(info.input->get_view_width());
	push.inv_resolution[1] = 1.0f / float(info.input->get_view_height());
	push.mips = info.num_mips;

	uint32_t wg_x = (push.base_image_resolution[0] + 31) / 32;
	uint32_t wg_y = (push.base_image_resolution[1] + 31) / 32;
	push.num_workgroups = wg_x * wg_y;
	cmd.push_constants(&push, 0, sizeof(push));

	cmd.enable_subgroup_size_control(true);
	cmd.set_subgroup_size_log2(true, 2, 7);
	cmd.dispatch(wg_x, wg_y, 1);
	cmd.enable_subgroup_size_control(false);
}

struct SPDPassState : Util::IntrusivePtrEnabled<SPDPassState>
{
	RenderTextureResource *otex;
	RenderTextureResource *itex;
	RenderBufferResource *counter;
	Util::SmallVector<Vulkan::ImageViewHandle, MaxSPDMips> views;
	const Vulkan::ImageView *output_mips[MaxSPDMips];
	SPDInfo info = {};
};

void setup_depth_hierarchy_pass(RenderGraph &graph, const RenderContext &context,
                                const std::string &input, const std::string &output)
{
	AttachmentInfo att;
	att.format = VK_FORMAT_R16_SFLOAT;
	att.size_relative_name = input;
	att.size_class = SizeClass::InputRelative;

	auto &pass = graph.add_pass(output, RENDER_GRAPH_QUEUE_COMPUTE_BIT);
	auto handle = Util::make_handle<SPDPassState>();
	handle->itex = &pass.add_texture_input(input);

	// Stop if we reach 2x1 or 1x2 dimension.
	auto dim = graph.get_resource_dimensions(*handle->itex);
	att.levels = Util::floor_log2(std::min(dim.width, dim.height)) + 1;
	handle->otex = &pass.add_storage_texture_output(output, att);

	BufferInfo storage_info = {};
	storage_info.size = 4;
	storage_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	handle->counter = &pass.add_storage_output(output + "-counter", storage_info);

	pass.set_build_render_pass([&graph, &context, h = handle](Vulkan::CommandBuffer &cmd) mutable {
		if (h->views.empty())
		{
			auto &otex = graph.get_physical_texture_resource(*h->otex);
			h->info.num_mips = otex.get_image().get_create_info().levels;
			h->views.reserve(h->info.num_mips);
			VK_ASSERT(h->info.num_mips <= MaxSPDMips);

			Vulkan::ImageViewCreateInfo info = {};
			info.image = &otex.get_image();
			info.levels = 1;
			info.layers = 1;
			info.format = VK_FORMAT_R16_SFLOAT;
			info.view_type = VK_IMAGE_VIEW_TYPE_2D;

			for (unsigned i = 0; i < h->info.num_mips; i++)
			{
				info.base_level = i;
				h->views.push_back(cmd.get_device().create_image_view(info));
				h->output_mips[i] = h->views.back().get();
			}
			h->info.output_mips = h->output_mips;
		}

		h->info.input = &graph.get_physical_texture_resource(*h->itex);
		h->info.counter_buffer = &graph.get_physical_buffer_resource(*h->counter);
		h->info.num_components = 1;

		mat2 inv_zw = mat2(context.get_render_parameters().inv_projection[2].zw(),
		                   context.get_render_parameters().inv_projection[3].zw());
		h->info.z_transform = &inv_zw;
		emit_single_pass_downsample(cmd, h->info);
	});
}
}
