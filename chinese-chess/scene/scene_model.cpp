#include "scene_model.h"

#include "vulkan_core/vulkan_common.h"

vk::AccelerationStructureInstanceKHR scene_model::get_blas_instance() const noexcept
{
    return vk::AccelerationStructureInstanceKHR(vulkan_common::glm_matrix_to_vulkan(model_matrix), custom_index,
                                                is_show ? 0xFF : 0, 0, {}, model_info->blas_info.get_address());
}
