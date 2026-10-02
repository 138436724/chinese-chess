#include "shared/cpu_shared_types.h"

#include "shared/vulkan_shared_utils.h"

vk::AccelerationStructureInstanceKHR cpu_model_data::get_blas_instance() const noexcept
{
    return vk::AccelerationStructureInstanceKHR(vulkan_shared_utils::glm_matrix_to_vulkan(model_matrix), custom_index,
                                                is_show ? 0xFF : 0, 0, {}, model_info->blas_info.get_address());
}
