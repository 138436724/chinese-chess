#include "scene_cubemap.h"
#include "tools/image_helper.h"
#include "tools/shader_compiler.h"

void scene_cubemap::create(const vulkan_application* _app, const std::filesystem::path& _hdr_path)
{
	// read hdr image
	std::vector<float> hdr_data;
	IMAGE_HELPER.read_hdr_image(_hdr_path, width, height, hdr_data);


	// create hdr image and sampler
	vulkan_image hdr_image;
	vk::ImageCreateInfo hdr_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(width, height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo hdr_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	hdr_image.create(_app->get_physical_device(), _app->get_device(), hdr_image_info, hdr_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));

	vk::PhysicalDeviceProperties hdr_properties = _app->get_physical_device().getProperties();
	vk::SamplerCreateInfo hdr_sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
		vk::SamplerAddressMode::eRepeat, vk::SamplerAddressMode::eRepeat, vk::SamplerAddressMode::eRepeat,
		0.f, vk::True, hdr_properties.limits.maxSamplerAnisotropy, vk::False, vk::CompareOp::eAlways, 0.f, 1.f,
		vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
	vk::raii::Sampler hdr_sampler = vk::raii::Sampler(_app->get_device(), hdr_sampler_info);


	// descriptor pool
	std::array pool_size{
		vk::DescriptorPoolSize(vk::DescriptorType::eCombinedImageSampler, 1),
	};
	vk::DescriptorPoolCreateInfo pool_info(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, pool_size);
	descriptor_pool = vk::raii::DescriptorPool(_app->get_device(), pool_info);


	// pipeline
	std::array bindings{
		vk::DescriptorSetLayoutBinding(0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment, nullptr),
	};

	auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"scene_cubemap.slang", { std::string(VERT_ENTYR_NAME), std::string(FRAG_ENTYR_NAME) });
	if (spirv_code.empty())
	{
		throw std::runtime_error("compile .spv failed!");
	}

	vk::raii::ShaderModule shaderModule(_app->get_device(), vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char), reinterpret_cast<const uint32_t*>(spirv_code.data())));
	std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTYR_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTYR_NAME.data()),
	};

	std::vector<vk::Format> color_formats(6, color_format);

	pipeline.create_pipeline(_app->get_device(), bindings, {}, {}, {}, shader_stages,
		vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise,
		vk::SampleCountFlagBits::e1, vk::False, color_formats, vk::Format::eUndefined);


	// descriptor set
	std::vector<vk::DescriptorSetLayout> layouts(1, *(pipeline.get_descriptor_set_layout()));
	vk::DescriptorSetAllocateInfo alloc_info(descriptor_pool, layouts);

	//descriptor_set.clear();
	descriptor_set = std::move(_app->get_device().allocateDescriptorSets(alloc_info).front());

	vk::DescriptorImageInfo hdr_descriptor_image_info(hdr_sampler, hdr_image.get_imageview(), vk::ImageLayout::eShaderReadOnlyOptimal);

	std::array descriptorWrite{
		vk::WriteDescriptorSet(descriptor_set, 0, 0, vk::DescriptorType::eCombinedImageSampler, hdr_descriptor_image_info, nullptr),
	};

	_app->get_device().updateDescriptorSets(descriptorWrite, {});


	// create sampler
	vk::PhysicalDeviceProperties cubemap_properties = _app->get_physical_device().getProperties();
	vk::SamplerCreateInfo cubemap_sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
		vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge, vk::SamplerAddressMode::eClampToEdge,
		0.f, vk::True, cubemap_properties.limits.maxSamplerAnisotropy, vk::False, vk::CompareOp::eAlways, 0.f, 1.f,
		vk::BorderColor::eFloatOpaqueWhite, vk::False, nullptr);
	cubemap_sampler = vk::raii::Sampler(_app->get_device(), cubemap_sampler_info);


	// create image
	vk::ImageCreateInfo cubemap_image_info(vk::ImageCreateFlagBits::eCubeCompatible, vk::ImageType::e2D, color_format, vk::Extent3D(height / 2, height / 2, 1), 1, 6, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo cubemap_view_info({}, {}, vk::ImageViewType::eCube, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 6), nullptr);
	cubemap_image.create(_app->get_physical_device(), _app->get_device(), cubemap_image_info, cubemap_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));


	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(_app->get_queue(vk::QueueFlagBits::eGraphics).get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), &(_app->get_device()), &(_app->get_queue(vk::QueueFlagBits::eGraphics).get_queue())).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	vulkan_buffer stage_buffer;
	stage_buffer.create(_app->get_physical_device(), _app->get_device(), hdr_data.size() * sizeof(float), vk::BufferUsageFlagBits::eTransferSrc, vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
	memcpy(stage_buffer.get_buffer_address(), hdr_data.data(), hdr_data.size() * sizeof(float));

	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(hdr_image.set_layout(vk::ImageLayout::eTransferDstOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	vulkan_buffer::copy_buffer_to_image(*commandbuffer, stage_buffer.get_buffer(), hdr_image.get_image(), vk::BufferImageCopy2(0, 0, 0, vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1), vk::Offset3D(0, 0, 0), vk::Extent3D(width, height, 1)));

	std::vector<vk::ImageMemoryBarrier2> middle_barrier;
	middle_barrier.emplace_back(hdr_image.set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	middle_barrier.emplace_back(cubemap_image.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, vk::RemainingMipLevels, 0, vk::RemainingArrayLayers)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, middle_barrier));


	std::vector<vk::raii::ImageView> render_images;
	std::vector<vk::RenderingAttachmentInfo> color_attachment_infos;
	for (int i = 0; i < 6; i++)
	{
		vk::ImageViewCreateInfo render_image_view_info({}, cubemap_image.get_image(), vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, i, 1), nullptr);
		render_images.emplace_back(vk::raii::ImageView(_app->get_device(), render_image_view_info));
		color_attachment_infos.emplace_back(vk::RenderingAttachmentInfo(render_images.back(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eNone,
			{}, vk::ImageLayout::eUndefined, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, cubemap_image.get_clear_value()));
	}

	vk::RenderingInfo renderingInfo({}, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(height / 2, height / 2)), 1, {}, color_attachment_infos, nullptr, nullptr, nullptr);

	(*commandbuffer).beginRendering(renderingInfo);

	(*commandbuffer).setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(height / 2), static_cast<float>(height / 2), 0.f, 1.f));
	(*commandbuffer).setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(height / 2, height / 2)));
	(*commandbuffer).bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	(*commandbuffer).bindDescriptorSets(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline_layout(), 0, *descriptor_set, nullptr);
	(*commandbuffer).draw(3, 1, 0, 0);
	(*commandbuffer).endRendering();

	std::vector<vk::ImageMemoryBarrier2> end_barrier;
	end_barrier.emplace_back(cubemap_image.set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, vk::RemainingMipLevels, 0, vk::RemainingArrayLayers)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

vulkan_image& scene_cubemap::get_image() noexcept
{
	return cubemap_image;
}

const vk::raii::Sampler& scene_cubemap::get_sampler() const noexcept
{
	return cubemap_sampler;
}
