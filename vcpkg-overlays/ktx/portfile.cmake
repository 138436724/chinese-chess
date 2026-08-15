vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO KhronosGroup/KTX-Software
    REF "v${VERSION}"
    SHA512 3e204dd9befe0e2fe9d1e3c69f79eb7c08d26b4b3f38b784de67d93ffbc9e4ad44f8800628d62397586b55e21d9ac38102ab635f0d4428b3ab1b101f0f3fb5ab
    HEAD_REF master
)
file(GLOB third_party "${SOURCE_PATH}/external/*")
list(FILTER third_party EXCLUDE REGEX "/(astc-encoder|basis_universal|dfdutils|etcdec|imageio|glm|lodepng|SDL_gesture)$")
file(REMOVE_RECURSE ${third_party})

vcpkg_list(SET OPTIONS)
if(VCPKG_TARGET_IS_WINDOWS)
    vcpkg_acquire_msys(MSYS_ROOT
        PACKAGES
            bash
        DIRECT_PACKAGES
            # Required for "getopt"
            "https://repo.msys2.org/msys/x86_64/util-linux-2.40.2-2-x86_64.pkg.tar.zst"
            bf45b16cd470f8d82a9fe03842a09da2e6c60393c11f4be0bab354655072c7a461afc015b9c07f9f5c87a0e382cd867e4f079ede0d42f1589aa99ebbb3f76309
            # Required for "dos2unix"
            "https://mirror.msys2.org/msys/x86_64/dos2unix-7.5.2-1-x86_64.pkg.tar.zst"
            e5e949f01b19c82630131e338a4642da75e42f84220f5af4a97a11dd618e363396567b233d2adab79e05422660a0000abcbbabcd17efcadf37f07fe7565f041e
    )
    vcpkg_add_to_path("${MSYS_ROOT}/usr/bin")
    vcpkg_list(APPEND OPTIONS "-DBASH_EXECUTABLE=${MSYS_ROOT}/usr/bin/bash.exe")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DKTX_GIT_VERSION_FULL=v${VERSION}-vcpkg
        -DKTX_FEATURE_TESTS=OFF
        -DKTX_FEATURE_LOADTEST_APPS=OFF
        -DLIBKTX_FEATURE_VK_UPLOAD=OFF
        -DLIBKTX_FEATURE_GL_UPLOAD=OFF
        ${OPTIONS}
    OPTIONS_DEBUG
        -DKTX_FEATURE_TOOLS=OFF
    DISABLE_PARALLEL_CONFIGURE
)
vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/ktx)
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(GLOB LICENSE_FILES "${SOURCE_PATH}/LICENSES/*")
file(COPY ${LICENSE_FILES} DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}/LICENSES")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
