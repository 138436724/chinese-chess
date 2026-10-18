/**
 * @file vulkan_physical_device.h
 * @brief 物理设备选择：过滤 Vulkan 1.4、必需扩展与特性谓词，并求解四个队列族。
 * @details create 是模板函数，特性谓词由调用方以 concept 约束传入，本类只负责枚举、过滤与打分。
 *          按枚举顺序选择第一个满足要求的设备；其四类队列需求做笛卡尔积，优先选择不支持
 *          graphics/compute/present 的传输族，再比较组合中不同族的数量，并不在设备间比较分数。
 */
#pragma once

#include <algorithm>
#include <array>
#include <bitset>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include <vulkan/vulkan_raii.hpp>

/**
 * @brief 物理设备及其 graphics/compute/transfer 队列族下标。
 * @details 队列族在 create 里一次选定，之后 get_queue_index 只查缓存，不再向驱动发查询。
 *          四个需求（graphics/compute/transfer/present）做笛卡尔积；传输族不支持另外三项时加 10 分，
 *          再加组合中的不同族数，因此专用传输族的优先级高于增加不同族数。
 * @note present 队列族下标不存成员，由 create 的返回值交给 vulkan_application。
 */
class vulkan_physical_device
{
public:
    /** @brief 构造空对象，三个队列族下标均为 vk::QueueFamilyIgnored。 */
    vulkan_physical_device()                                         = default;
    /** @brief 默认析构：物理设备句柄由实例管理，本对象无需显式销毁。 */
    ~vulkan_physical_device()                                        = default;
    /** @brief 此包装禁止拷贝；VkPhysicalDevice 本身由实例管理，并非独占资源。 */
    vulkan_physical_device(const vulkan_physical_device&)            = delete;
    /** @brief 禁止拷贝赋值。 */
    vulkan_physical_device& operator=(const vulkan_physical_device&) = delete;
    /** @brief 移动构造：句柄与三个队列族下标一起转移，源对象下标复位为 QueueFamilyIgnored。 */
    vulkan_physical_device(vulkan_physical_device&& _other) noexcept;
    /** @brief 移动赋值：逐成员与源对象交换。 */
    vulkan_physical_device& operator=(vulkan_physical_device&& _other) noexcept;

    /**
     * @brief 枚举物理设备，按 Vulkan 1.4、必需扩展与调用方谓词过滤，再为每个候选求解四个队列族。
     * @param _instance   已创建的实例，用于 enumeratePhysicalDevices。
     * @param _extensions 设备必须全部支持的扩展名。
     * @param _features   额外过滤谓词，签名等价于 bool(const vk::raii::PhysicalDevice&)，通常检查特性链。
     * @param _surface    用于查询 present 支持的 surface。
     * @return 选中设备的 present 队列族下标。
     * @throws std::runtime_error 没有任何设备同时满足 1.4、扩展、谓词与四个队列需求时抛出。
     * @note 每个候选设备读取一次队列族属性；surface 支持由惰性视图按族查询，遍历和比较时可能重复查询。
     *       成功返回时 physical_device 与三个队列族下标同时生效；当前评分用 bitset<10>，遇到下标 >= 10 会抛 out_of_range。
     */
    template <typename Func>
        requires std::predicate<Func, const vk::raii::PhysicalDevice&>
    [[nodiscard]] uint32_t create(const vk::raii::Instance&          _instance,
                                  const std::span<const char* const> _extensions,
                                  Func&&                             _features,
                                  vk::SurfaceKHR                     _surface);

    /**
     * @brief 按队列能力位查对应的队列族下标。
     * @param _queue_type 队列能力位，只识别 Graphics / Compute / Transfer。
     * @return 对应的缓存族下标；其他能力位返回 vk::QueueFamilyIgnored。新对象初值为 Ignored，重复 create 失败不清空旧值。
     */
    [[nodiscard]] uint32_t                        get_queue_index(vk::QueueFlagBits _queue_type) const noexcept;
    /**
     * @brief 解引用取内部物理设备句柄。
     * @return 内部 vk::raii::PhysicalDevice；create 之前为空句柄。
     */
    [[nodiscard]] const vk::raii::PhysicalDevice& operator*() const noexcept;

private:
    /** @brief 选中的物理设备句柄，未创建时为 nullptr。 */
    vk::raii::PhysicalDevice physical_device = nullptr;

    /** @brief graphics 队列族下标，默认 QueueFamilyIgnored。 */
    uint32_t graphic_index  = vk::QueueFamilyIgnored;
    /** @brief compute 队列族下标；当前没有提交走 compute 队列。 */
    uint32_t compute_index  = vk::QueueFamilyIgnored;
    /** @brief transfer 队列族下标，上传与回读走它。 */
    uint32_t transfer_index = vk::QueueFamilyIgnored;
};

template <typename Func>
    requires std::predicate<Func, const vk::raii::PhysicalDevice&>
inline uint32_t vulkan_physical_device::create(const vk::raii::Instance&          _instance,
                                               const std::span<const char* const> _extensions,
                                               Func&&                             _features,
                                               vk::SurfaceKHR                     _surface)
{
    const auto all_physical_devices = _instance.enumeratePhysicalDevices();
    const auto filtered_physical_devices =
        all_physical_devices | std::views::filter([&](const auto& _physical_device) {
            const bool support_vulkan_1_4 = _physical_device.getProperties2().properties.apiVersion >= vk::ApiVersion14;

            const auto available_device_extensions = _physical_device.enumerateDeviceExtensionProperties();
            const bool has_all_required_extensions =
                std::ranges::all_of(_extensions, [&available_device_extensions](const auto& required_device_extension) {
                    return std::ranges::any_of(available_device_extensions, [required_device_extension](const auto& available_device_extension) {
                        return strcmp(available_device_extension.extensionName, required_device_extension) == 0;
                    });
                });

            return support_vulkan_1_4 && has_all_required_extensions && _features(_physical_device);
        })
        | std::ranges::to<std::vector>();

    uint32_t present_index = vk::QueueFamilyIgnored;
    const auto find_physical_device = std::ranges::find_if(filtered_physical_devices, [&](const auto& _physical_device) {
        const auto queue_family_properties = _physical_device.getQueueFamilyProperties2();

        const auto all_queue_supports =
            queue_family_properties | std::views::enumerate | std::views::transform([&](const auto& _pair) {
                const auto& [queue_family_index, queue_family_property] = _pair;
                const auto& queue_flags      = queue_family_property.queueFamilyProperties.queueFlags;
                bool        support_graphics = static_cast<bool>(queue_flags & vk::QueueFlagBits::eGraphics);
                bool        support_compute  = static_cast<bool>(queue_flags & vk::QueueFlagBits::eCompute);
                bool        support_transfer = static_cast<bool>(queue_flags & vk::QueueFlagBits::eTransfer);
                bool        support_present =
                    _physical_device.getSurfaceSupportKHR(static_cast<uint32_t>(queue_family_index), _surface) == vk::True;
                return std::array{support_graphics, support_compute, support_transfer, support_present};
            });

        // get all nums, like 0,1,2,3,4
        const auto the_digit = std::views::iota(0u, all_queue_supports.size());
        // filter the num's position, for example 1 not on present location
        const auto digits_at = [&](size_t pos) {
            return the_digit
                   | std::views::filter([&all_queue_supports, pos](size_t d) { return all_queue_supports[d][pos]; });
        };
        // list all support; cartesian_product_view is not const-iterable, so this one stays mutable
        auto all_kinds = std::views::cartesian_product(digits_at(0), digits_at(1), digits_at(2), digits_at(3));
        // count the different num, for example 1234->4 0000->1
        const auto unique_count = [](const auto& k) {
            const auto& [g, c, t, p] = k;
            std::bitset<10> bits;
            bits.set(g).set(c).set(t).set(p);
            return bits.count();
        };
        // find the most different queue
        const auto best_kind = std::ranges::max_element(all_kinds, [&](const auto& a, const auto& b) {
            // prefer only support transfer queue
            const auto& [g_a, c_a, t_a, p_a] = a;
            const auto& [g_b, c_b, t_b, p_b] = b;
            const auto a_score = static_cast<size_t>(all_queue_supports[t_a][0] == false && all_queue_supports[t_a][1] == false
                                                     && all_queue_supports[t_a][2] == true && all_queue_supports[t_a][3] == false)
                                 * 10;
            const auto b_score = static_cast<size_t>(all_queue_supports[t_b][0] == false && all_queue_supports[t_b][1] == false
                                                     && all_queue_supports[t_b][2] == true && all_queue_supports[t_b][3] == false)
                                 * 10;
            return a_score + unique_count(a) < b_score + unique_count(b);
        });

        if (best_kind != all_kinds.end())
        {
            auto [g, c, t, p] = *best_kind;

            graphic_index  = static_cast<uint32_t>(g);
            compute_index  = static_cast<uint32_t>(c);
            transfer_index = static_cast<uint32_t>(t);
            present_index  = static_cast<uint32_t>(p);

            return true;
        }

        return false;
    });

    if (find_physical_device == filtered_physical_devices.end())
    {
        throw std::runtime_error("failed to find a suitable GPU!");
    }

    physical_device = *find_physical_device;

    return present_index;
}
