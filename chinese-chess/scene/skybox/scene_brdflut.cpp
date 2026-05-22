#include "scene_brdflut.h"
#include "tools/shader_compiler.h"

void scene_brdflut::create(const vulkan_application* _app)
{
	// pipeline
	auto spirv_code = SHADER_COMPILER.compile_shader_to_spv(std::u8string(SHADERS_PATH) + u8"scene_brdflut.slang", { std::string(VERT_ENTYR_NAME), std::string(FRAG_ENTYR_NAME) });
	if (spirv_code.empty())
	{
		throw std::runtime_error("compile .spv failed!");
	}

	vk::raii::ShaderModule shaderModule(_app->get_device(), vk::ShaderModuleCreateInfo({}, spirv_code.size() * sizeof(char), reinterpret_cast<const uint32_t*>(spirv_code.data())));
	std::array<vk::PipelineShaderStageCreateInfo, 2> shader_stages = {
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eVertex, shaderModule, VERT_ENTYR_NAME.data()),
		vk::PipelineShaderStageCreateInfo({}, vk::ShaderStageFlagBits::eFragment, shaderModule, FRAG_ENTYR_NAME.data()),
	};

	pipeline.create(_app->get_device(), {}, {}, {}, {}, shader_stages,
		vk::PrimitiveTopology::eTriangleList, vk::PolygonMode::eFill, vk::CullModeFlagBits::eNone, vk::FrontFace::eCounterClockwise,
		vk::SampleCountFlagBits::e1, vk::False, std::span(&color_format, 1), vk::Format::eUndefined);


	// create sampler
	vk::PhysicalDeviceProperties properties = _app->get_physical_device().getProperties();
	vk::SamplerCreateInfo sampler_info({}, vk::Filter::eLinear, vk::Filter::eLinear, vk::SamplerMipmapMode::eLinear,
		vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder, vk::SamplerAddressMode::eClampToBorder,
		0.f, vk::True, properties.limits.maxSamplerAnisotropy, vk::False, vk::CompareOp::eAlways, 0.f, 1.f,
		vk::BorderColor::eFloatOpaqueBlack, vk::False, nullptr);
	brdflut_sampler = vk::raii::Sampler(_app->get_device(), sampler_info);


	// create image
	vk::ImageCreateInfo brdflut_image_info({}, vk::ImageType::e2D, color_format, vk::Extent3D(width, height, 1), 1, 1, vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment, vk::SharingMode::eExclusive, 0);
	vk::ImageViewCreateInfo brdflut_view_info({}, {}, vk::ImageViewType::e2D, color_format, {}, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, {}, 1, 0, 1), nullptr);
	brdflut_image.create(_app->get_physical_device(), _app->get_device(), brdflut_image_info, brdflut_view_info, vk::MemoryPropertyFlagBits::eDeviceLocal, vk::ClearColorValue(0.f, 0.f, 0.f, 1.f));


	// begin a commandbuffer
	vulkan_commandbuffer commandbuffer = std::move(vulkan_commandbuffer::create(vk::CommandBufferAllocateInfo(_app->get_queue(vk::QueueFlagBits::eGraphics).get_command_pool(), vk::CommandBufferLevel::ePrimary, 1), &(_app->get_device()), &(_app->get_queue(vk::QueueFlagBits::eGraphics).get_queue())).front());
	commandbuffer.begin_record(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);


	// render
	std::vector<vk::ImageMemoryBarrier2> begin_barrier;
	begin_barrier.emplace_back(brdflut_image.set_layout(vk::ImageLayout::eColorAttachmentOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, begin_barrier));

	vk::RenderingAttachmentInfo colorAttachmentInfo(brdflut_image.get_imageview(), vk::ImageLayout::eColorAttachmentOptimal, vk::ResolveModeFlagBits::eNone,
		{}, vk::ImageLayout::eUndefined, vk::AttachmentLoadOp::eClear, vk::AttachmentStoreOp::eStore, brdflut_image.get_clear_value());

	vk::RenderingInfo renderingInfo({}, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)), 1, {}, 1, &colorAttachmentInfo, nullptr, nullptr, nullptr);

	(*commandbuffer).beginRendering(renderingInfo);

	(*commandbuffer).setViewport(0, vk::Viewport(0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f));
	(*commandbuffer).setScissor(0, vk::Rect2D(vk::Offset2D(0, 0), vk::Extent2D(width, height)));
	(*commandbuffer).bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline.get_pipeline());
	(*commandbuffer).draw(3, 1, 0, 0);
	(*commandbuffer).endRendering();

	std::vector<vk::ImageMemoryBarrier2> end_barrier;
	end_barrier.emplace_back(brdflut_image.set_layout(vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
	(*commandbuffer).pipelineBarrier2(vk::DependencyInfo({}, {}, {}, end_barrier));


	// commandbuffer submit
	commandbuffer.end_record();
	commandbuffer.submit({}, {}, true);
}

vulkan_image& scene_brdflut::get_image() noexcept
{
	return brdflut_image;
}

const vk::raii::Sampler& scene_brdflut::get_sampler() const noexcept
{
	return brdflut_sampler;
}
