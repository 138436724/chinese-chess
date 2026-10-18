#pragma once

#include <imgui.h>
#include <vulkan/vulkan.h>

// Initializes only upstream's private VBO/IBO owner. The application owns WSI.
VkPipeline ImGui_ImplVulkan_CreateViewportRenderBuffers(ImGuiViewport* viewport, VkFormat format);
