#include "scene_model.h"
#include "vulkan_core/vulkan_common.h"

scene_model::scene_model(std::shared_ptr<model_infomation> _model_info) noexcept
	:model_info(_model_info)
{
}

vk::AccelerationStructureInstanceKHR scene_model::get_blas_instance() const noexcept
{
	return vk::AccelerationStructureInstanceKHR(vulkan_common::glm_matrix_to_vulkan(model_matrix), custom_index, is_show ? 0xFF : 0, 0, vk::GeometryInstanceFlagBitsKHR::eTriangleCullDisable, model_info->blas_info.get_address());
}