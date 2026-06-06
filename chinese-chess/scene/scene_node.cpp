#include "scene_node.h"

scene_node::scene_node(std::shared_ptr<model_infomation> _model_info, std::shared_ptr<vulkan_acceleration_structure> _blas_info) noexcept
	:model_info(_model_info),
	blas_info(_blas_info)
{
}

vk::AccelerationStructureInstanceKHR scene_node::get_blas_instance() const noexcept
{
	return vk::AccelerationStructureInstanceKHR(vulkan_common::glm_matrix_to_vulkan(model_matrix), custom_index, is_show ? 0xFF : 0, 0, vk::GeometryInstanceFlagBitsKHR::eTriangleCullDisable, blas_info->get_address());
}