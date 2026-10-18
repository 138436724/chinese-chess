/**
 * @file vulkan_swapchain.cpp
 * @brief vulkan_swapchain 的实现：交换链创建与重建、acquire、present 与取消路径。
 * @details recreate 先 drain 帧调度器与 present fence，再用临时 vector 建好新 image 的
 *          view、semaphore、fence 后整体替换；仅捕获 OutOfDate 的创建失败并清空旧链，其他异常向外传播。
 *          acquire 成功后才把 image 标记为已获取，present 入队成功才 finish。
 *          取消路径用 releaseSwapchainImagesKHR 归还 image 并替换 acquire semaphore，
 *          避免留下已信号却无人消费的信号量。
 */
#include "vulkan_swapchain.h"

#include "vulkan_common.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
/**
 * @brief 以无限超时等待 present fence，只在返回 eTimeout 时重试。
 */
void wait_present(const vk::raii::Device& device, vk::Fence fence)
{
    while (device.waitForFences(fence, true, std::numeric_limits<uint64_t>::max()) == vk::Result::eTimeout) {}
}
/**
 * @brief 把 acquire 的 vk::Result 映射为 ready/skip/recreate/fatal。
 */
vulkan_frame_status acquire_status(vk::Result result)
{
    if (result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR) return vulkan_frame_status::ready;
    if (result == vk::Result::eErrorOutOfDateKHR) return vulkan_frame_status::recreate;
    if (result == vk::Result::eTimeout || result == vk::Result::eNotReady) return vulkan_frame_status::skip;
    return vulkan_frame_status::fatal;
}
/**
 * @brief 判断 present 结果是否已让交换链接管 image（含 OutOfDate 与 SurfaceLost）。
 */
bool present_enqueued(vk::Result result)
{
    return result == vk::Result::eSuccess || result == vk::Result::eSuboptimalKHR ||
           result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eErrorSurfaceLostKHR;
}
}

/**
 * @brief 析构直接走 shutdown：先取消未完成帧，再排空帧调度器与 present fence。
 */
vulkan_swapchain::~vulkan_swapchain() { shutdown(); }

/**
 * @brief 接管 surface 所有权、选格式与呈现模式、接上 present 队列与帧调度器，再尝试创建交换链。
 * @note OCIO 开启时偏好 B8G8R8A8Unorm（合成阶段自己输出显示编码），否则偏好 sRGB；
 *       surface 支持 eMailbox 时用它，否则保持 eFifo。
 */
void vulkan_swapchain::create(const vk::raii::Instance& instance, const vk::raii::PhysicalDevice& phys,
                              const vk::raii::Device& logical, vk::SurfaceKHR native_surface,
                              const vulkan_queue& graphic, const vulkan_queue& transfer,
                              uint32_t present_index, uint32_t width, uint32_t height)
{
    device = &logical;
    physical = &phys;
    graphics = &graphic;
    surface = vk::raii::SurfaceKHR(instance, native_surface);
    const auto formats = physical->getSurfaceFormatsKHR(*surface);
    if (formats.empty()) throw std::runtime_error("Surface has no supported format.");
    const auto preferred = vulkan_common::USE_OCIO ? vk::Format::eB8G8R8A8Unorm : vk::Format::eB8G8R8A8Srgb;
    const auto found = std::ranges::find_if(formats, [preferred](const auto& f) {
        return f.format == preferred && f.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear;
    });
    const auto selected = found == formats.end() ? formats.front() : *found;
    format = selected.format;
    color_space = selected.colorSpace;
    const auto modes = physical->getSurfacePresentModesKHR(*surface);
    if (std::ranges::find(modes, vk::PresentModeKHR::eMailbox) != modes.end()) present_mode = vk::PresentModeKHR::eMailbox;
    if (present_index == graphic.get_index()) present_queue.create(logical, present_index, graphic.get_submission_state());
    else if (present_index == transfer.get_index()) present_queue.create(logical, present_index, transfer.get_submission_state());
    else present_queue.create(logical, present_index);
    frames.create(logical, vulkan_common::MAX_FRAMES_IN_FLIGHT);
    (void)recreate(width, height);
}

/**
 * @brief 先 drain 再建新交换链，新 image 状态建好后整体替换；仅创建返回 OutOfDate 时清空旧链待重建。
 * @note image 数取 MAX_FRAMES_IN_FLIGHT + 1 供 present fence 轮转，并 clamp 到 surface 上下限；
 *       重建成功 generation 自增，重建前取得的帧上下文随即失效。
 */
bool vulkan_swapchain::recreate(uint32_t width, uint32_t height)
{
    if (!width || !height) return false;
    if (frames.active()) throw std::logic_error("Cannot recreate an active frame.");
    const auto caps = physical->getSurfaceCapabilitiesKHR(*surface);
    const auto size = caps.currentExtent.width != std::numeric_limits<uint32_t>::max() ? caps.currentExtent :
        vk::Extent2D(std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width),
                     std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height));
    if (!size.width || !size.height) return false;
    // 图形提交完成不代表呈现已放开信号量；替换旧链前还需逐 image 等待 present fence。
    drain();
    auto count = std::max(vulkan_common::MAX_FRAMES_IN_FLIGHT + 1, caps.minImageCount);
    if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
    const std::array families{graphics->get_index(), present_queue.get_index()};
    const bool concurrent = families[0] != families[1];
    vk::SwapchainCreateInfoKHR info({}, *surface, count, format, color_space, size, 1,
        vk::ImageUsageFlagBits::eColorAttachment, concurrent ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive,
        concurrent ? 2u : 0u, concurrent ? families.data() : nullptr, caps.currentTransform,
        vk::CompositeAlphaFlagBitsKHR::eOpaque, present_mode, true, *swapchain);
    vk::raii::SwapchainKHR replacement = nullptr;
    try { replacement = vk::raii::SwapchainKHR(*device, info); }
    catch (const vk::OutOfDateKHRError&)
    {
        // oldSwapchain may already have been retired by the failed creation.
        images.clear();
        swapchain = nullptr;
        return false;
    }
    const auto handles = replacement.getImages();
    std::vector<image_state> replacement_images;
    replacement_images.reserve(handles.size());
    for (const auto image : handles)
    {
        image_state state;
        state.image = image;
        state.view = vk::raii::ImageView(*device, vk::ImageViewCreateInfo({}, image, vk::ImageViewType::e2D, format, {},
            vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)));
        state.render_finished = vk::raii::Semaphore(*device, vk::SemaphoreCreateInfo());
        state.present_done = vk::raii::Fence(*device, vk::FenceCreateInfo());
        replacement_images.push_back(std::move(state));
    }
    images.clear(); // views/semaphores/fences before the old swapchain
    swapchain = std::move(replacement);
    images = std::move(replacement_images);
    extent = size;
    ++generation;
    recreate_after_present = false;
    log("recreate");
    return true;
}

/**
 * @brief 先 prepare 当前 slot，再 vkAcquireNextImageKHR，并按结果分类。
 * @note acquire 到的 image 若上次 present 尚未确认，先等它的 present fence；SUBOPTIMAL 仍算
 *       ready，但置 recreate_after_present，让本帧呈现后再重建。
 */
vulkan_frame_result vulkan_swapchain::acquire_image()
{
    if (!*swapchain) return {vulkan_frame_status::recreate, {}, vk::Result::eErrorOutOfDateKHR};
    frames.prepare(*device);
    uint32_t index = 0;
    auto result = static_cast<vk::Result>(device->getDispatcher()->vkAcquireNextImageKHR(**device, *swapchain,
        std::numeric_limits<uint64_t>::max(), frames.acquire_semaphore(), frames.acquire_fence(), &index));
    const auto status = acquire_status(result);
    if (status != vulkan_frame_status::ready)
    {
        log("acquire", result);
        return {status, {}, result};
    }
    const auto context = frames.acquired(index, generation);
    image_acquired = true;
    recreate_after_present = result == vk::Result::eSuboptimalKHR;
    auto& image = images.at(index);
    if (image.present_pending)
    {
        wait_present(*device, *image.present_done);
        image.present_pending = false;
        log("present-fence-complete", vk::Result::eSuccess, index);
    }
    log("acquire", result);
    return {status, context, result};
}

/**
 * @brief 把提交 token 交给帧调度器；present 之前必须先登记。
 */
void vulkan_swapchain::submitted(vulkan_submission_token token) { frames.submitted(token); }

/**
 * @brief 用该 image 的 render-finished semaphore 与 present fence 提交呈现。
 * @note 呈现入队（含 OutOfDate/SurfaceLost）才置 present_pending 并 finish；present fence
 *       只证明 image 资源可回收，不证明像素已经显示。
 */
vulkan_frame_result vulkan_swapchain::present_image()
{
    if (!frames.has_submission()) throw std::logic_error("Cannot present an unsubmitted frame.");
    const auto context = frames.context();
    auto& image = images.at(context.image);
    device->resetFences(*image.present_done);
    const vk::Fence fence = *image.present_done;
    const vk::Semaphore semaphore = *image.render_finished;
    const vk::SwapchainPresentFenceInfoKHR present_fence(fence);
    const vk::PresentInfoKHR info(semaphore, *swapchain, context.image, {}, &present_fence);
    const auto result = present_queue.present(info);
    // OUT_OF_DATE/SURFACE_LOST 仍会执行已入队的 semaphore wait；OOM 未入队，须保留 acquisition 供取消。
    if (present_enqueued(result))
    {
        image.present_pending = true;
        image_acquired = false;
    }
    log("present", result);
    if (image.present_pending) frames.finish();
    const auto status = result == vk::Result::eSuccess ?
        (recreate_after_present ? vulkan_frame_status::recreate : vulkan_frame_status::ready) :
        (result == vk::Result::eSuboptimalKHR || result == vk::Result::eErrorOutOfDateKHR ?
            vulkan_frame_status::recreate : vulkan_frame_status::fatal);
    return {status, context, result};
}

/**
 * @brief 取消路径：归还已 acquire 的 image 并替换相关 semaphore，不提交任何命令。
 */
void vulkan_swapchain::abandon_frame()
{
    cancel_acquisition(true);
}

/**
 * @brief 归还当前 acquisition：先等提交与 acquire fence，再 release image，按 reuse 决定是否重建信号量。
 * @note 即使没有提交消费 acquire semaphore，acquire fence 也证明信号操作已完成，因此可以安全销毁。
 */
void vulkan_swapchain::cancel_acquisition(bool reuse)
{
    if (!frames.active()) return;
    // An acquire fence proves the signal operation has finished even if no
    // submit consumed the acquire semaphore. It can then be destroyed safely.
    frames.drain(*device);
    // 帧的最终提交可能尚未发生；UI/scene 已提交的部分工作只能从图形队列前沿取得完成凭据。
    graphics->get_latest_token().wait();
    if (image_acquired)
    {
        const auto index = frames.context().image;
        device->releaseSwapchainImagesKHR(vk::ReleaseSwapchainImagesInfoKHR(*swapchain, 1, &index));
        image_acquired = false;
        if (reuse) images.at(index).render_finished = vk::raii::Semaphore(*device, vk::SemaphoreCreateInfo());
    }
    if (reuse) frames.replace_acquire_semaphore(*device);
    log("cancel");
    frames.cancel();
}

/**
 * @brief 等所有 slot 的提交与 acquire fence，再逐个等 present fence 并清 present_pending。
 * @note recreate 与 shutdown 都先走这里，保证重建或销毁时没有仍在使用的呈现资源。
 */
void vulkan_swapchain::drain()
{
    frames.drain(*device);
    for (uint32_t index = 0; index < images.size(); ++index)
    {
        auto& image = images[index];
        if (image.present_pending)
        {
            wait_present(*device, *image.present_done);
            image.present_pending = false;
            log("present-fence-complete", vk::Result::eSuccess, index);
        }
    }
}

/**
 * @brief 先 cancel_acquisition(false) 再 drain；device lost 只打印，其他异常打印后 terminate。
 * @note 幂等：device 为空（尚未 create）时直接返回。
 */
void vulkan_swapchain::shutdown() noexcept
{
    if (!device) return;
    try { cancel_acquisition(false); drain(); }
    catch (const vk::DeviceLostError&) { std::fputs("WSI device lost; releasing presentation resources.\n", stderr); }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Cannot safely drain presentation resources: %s\n", error.what());
        std::terminate();
    }
}

/**
 * @brief 用当前帧上下文索引 image 表，无活动帧时帧调度器抛 logic_error。
 */
vk::Image vulkan_swapchain::get_current_image() const { return images.at(frames.context().image).image; }
/**
 * @brief 取当前帧 image 的 view，与 get_current_image 走同一次上下文校验。
 */
const vk::raii::ImageView& vulkan_swapchain::get_current_imageview() const { return images.at(frames.context().image).view; }
/**
 * @brief 构造帧提交的等待项：当前 slot 的 acquire semaphore，阶段为 color attachment output。
 */
vk::SemaphoreSubmitInfo vulkan_swapchain::get_waited_info() const
{
    return vk::SemaphoreSubmitInfo(frames.acquire_semaphore(), 0, vk::PipelineStageFlagBits2::eColorAttachmentOutput);
}
/**
 * @brief 构造帧提交的信号项：当前 image 的 render-finished semaphore，阶段为 ALL_COMMANDS。
 */
vk::SemaphoreSubmitInfo vulkan_swapchain::get_signal_info() const
{
    return vk::SemaphoreSubmitInfo(*images.at(frames.context().image).render_finished, 0, vk::PipelineStageFlagBits2::eAllCommands);
}
/**
 * @brief trace 为 false 时直接返回；打开后打印 generation/slot/image/result 并 flush。
 */
void vulkan_swapchain::log(const char* action, vk::Result result, uint32_t image) const
{
    if (!trace) return;
    std::fprintf(stdout, "WSI %s generation=%llu slot=%u image=%u images=%zu result=%d\n", action,
        static_cast<unsigned long long>(generation), frames.slot(), image != UINT32_MAX ? image : (frames.active() ? frames.context().image : 0),
        images.size(), static_cast<int>(result));
    std::fflush(stdout);
}
