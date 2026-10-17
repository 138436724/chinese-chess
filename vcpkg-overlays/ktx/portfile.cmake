vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO KhronosGroup/KTX-Software
    REF "v${VERSION}"
    SHA512 7c7559fe4d8dd87fe9858f34c3b4522073818186be6853512294ecb7b6883348d6834a00b9b622920ed2688802b812b002bb2224fdce630ff66ec1cf9784104d
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
