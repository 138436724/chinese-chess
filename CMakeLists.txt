cmake_minimum_required(VERSION 4.0)

set(VCPKG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg")
set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    CACHE STRING "Local vcpkg toolchain file")

project(chinese-chess)

find_package(fastgltf CONFIG REQUIRED)
find_package(Freetype REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(ICU COMPONENTS i18n io uc REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED) # for OpenColorIO
find_package(OpenColorIO CONFIG REQUIRED)
find_package(OpenImageIO CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(msft_proxy4 CONFIG REQUIRED)
find_package(slang CONFIG REQUIRED)
find_package(unofficial-vulkan-memory-allocator-hpp CONFIG REQUIRED)
find_package(VulkanUtilityLibraries CONFIG REQUIRED)


file(GLOB_RECURSE MY_SOURCES "chinese-chess/*.cpp")
file(GLOB_RECURSE MY_HEADERS "chinese-chess/*.h")

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/chinese-chess"
             FILES ${MY_SOURCES} ${MY_HEADERS})

add_executable(chinese-chess ${MY_SOURCES} ${MY_HEADERS})

target_compile_features(chinese-chess PRIVATE cxx_std_23)
target_include_directories(chinese-chess PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/chinese-chess)
target_compile_definitions(chinese-chess PRIVATE
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_ENABLE_EXPERIMENTAL
    GLM_FORCE_DEPTH_ZERO_TO_ONE
)

if(WIN32)
    target_compile_definitions(chinese-chess PRIVATE
        NOMINMAX
        UNICODE
        _UNICODE
        VK_USE_PLATFORM_WIN32_KHR
    )
    # target_link_libraries(chinese-chess PRIVATE comdlg32)
endif()


target_link_libraries(chinese-chess PRIVATE 
    fastgltf::fastgltf
    Freetype::Freetype # since CMake 3.10
    glfw
    glm::glm
    ICU::i18n ICU::io ICU::uc
    Vulkan::Headers
    imgui::imgui
    yaml-cpp::yaml-cpp OpenColorIO::OpenColorIO
    OpenImageIO::OpenImageIO
    OpenSSL::SSL OpenSSL::Crypto
    slang::slang
    msft_proxy4::proxy
    unofficial::VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
    Vulkan::SafeStruct Vulkan::LayerSettings Vulkan::UtilityHeaders Vulkan::CompilerConfiguration
)

set_target_properties(chinese-chess PROPERTIES
    DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/chinese-chess/"
)