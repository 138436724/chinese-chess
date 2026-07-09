#pragma once

#include <algorithm>
#include <bitset>
#include <ranges>
#include <vulkan/vulkan_raii.hpp>

class vulkan_physical_device
{
public:
    vulkan_physical_device()                                   = default;
    ~vulkan_physical_device()                                  = default;
    vulkan_physical_device(vulkan_physical_device&)            = delete;
    vulkan_physical_device& operator=(vulkan_physical_device&) = delete;
    vulkan_physical_device(vulkan_physical_device&& _other) noexcept;
    vulkan_physical_device& operator=(vulkan_physical_device&& _other) noexcept;

    template <typename Func>
        requires std::predicate<Func, const vk::raii::PhysicalDevice&>
    uint32_t create(const vk::raii::Instance& _instance, const std::span<const char* const> _extensions, Func&& _features, vk::SurfaceKHR _surface);

    uint32_t                        get_queue_index(vk::QueueFlagBits _queue_type) const noexcept;
    const vk::raii::PhysicalDevice& operator*() const noexcept;

private:
    vk::raii::PhysicalDevice physical_device = nullptr;

    uint32_t graphic_index  = vk::QueueFamilyIgnored;
    uint32_t compute_index  = vk::QueueFamilyIgnored;
    uint32_t transfer_index = vk::QueueFamilyIgnored;
};

template <typename Func>
    requires std::predicate<Func, const vk::raii::PhysicalDevice&>
inline uint32_t vulkan_physical_device::create(const vk::raii::Instance&          _instance,
                                               const std::span<const char* const> _extensions,
                                               Func&&                             _features,
                                               vk::SurfaceKHR                     _surface)
{
    auto all_physical_devices = _instance.enumeratePhysicalDevices();
    auto filtered_physical_devices =
        all_physical_devices | std::views::filter([&](const auto& _physical_device) {
            bool support_vulkan_1_4 = _physical_device.getProperties().apiVersion >= vk::ApiVersion14;

            auto available_device_extensions = _physical_device.enumerateDeviceExtensionProperties();
            bool has_all_required_extensions =
                std::ranges::all_of(_extensions, [&available_device_extensions](const auto& required_device_extension) {
                    return std::ranges::any_of(available_device_extensions, [required_device_extension](const auto& available_device_extension) {
                        return strcmp(available_device_extension.extensionName, required_device_extension) == 0;
                    });
                });

            return support_vulkan_1_4 && has_all_required_extensions && std::forward<Func>(_features)(_physical_device);
        })
        | std::ranges::to<std::vector>();

    uint32_t present_index        = vk::QueueFamilyIgnored;
    auto     find_physical_device = std::ranges::find_if(filtered_physical_devices, [&](const auto& _physical_device) {
        auto queue_family_properties = _physical_device.getQueueFamilyProperties();

        auto all_queue_supports =
            queue_family_properties | std::views::enumerate | std::views::transform([&](const auto& _pair) {
                const auto& [queue_family_index, queue_family_property] = _pair;
                bool support_graphics = static_cast<bool>(queue_family_property.queueFlags & vk::QueueFlagBits::eGraphics);
                bool support_compute = static_cast<bool>(queue_family_property.queueFlags & vk::QueueFlagBits::eCompute);
                bool support_transfer = static_cast<bool>(queue_family_property.queueFlags & vk::QueueFlagBits::eTransfer);
                bool support_present =
                    _physical_device.getSurfaceSupportKHR(static_cast<uint32_t>(queue_family_index), _surface) == vk::True;
                return std::array{support_graphics, support_compute, support_transfer, support_present};
            });

        // get all nums, like 0,1,2,3,4
        auto the_digit = std::views::iota(0u, all_queue_supports.size());
        // filter the num's position, for example 1 not on persent location
        auto digits_at = [&](size_t pos) {
            return the_digit
                   | std::views::filter([&all_queue_supports, pos](size_t d) { return all_queue_supports[d][pos]; });
        };
        // list all support
        auto all_kinds = std::views::cartesian_product(digits_at(0), digits_at(1), digits_at(2), digits_at(3));
        // count the different num, for example 1234->4 0000->1
        auto unique_count = [](const auto& k) {
            const auto& [g, c, t, p] = k;
            std::bitset<10> bits;
            bits.set(g).set(c).set(t).set(p);
            return bits.count();
        };
        // find the most different queue
        auto best_kind = std::ranges::max_element(all_kinds, [&](const auto& a, const auto& b) {
            // prefer only support transfer queue
            const auto& [g_a, c_a, t_a, p_a] = a;
            const auto& [g_b, c_b, t_b, p_b] = b;
            auto a_score = static_cast<size_t>(all_queue_supports[t_a][0] == false && all_queue_supports[t_a][1] == false
                                               && all_queue_supports[t_a][2] == true && all_queue_supports[t_a][3] == false);
            a_score *= 10;
            auto b_score = static_cast<size_t>(all_queue_supports[t_b][0] == false && all_queue_supports[t_b][1] == false
                                               && all_queue_supports[t_b][2] == true && all_queue_supports[t_b][3] == false);
            b_score *= 10;
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
