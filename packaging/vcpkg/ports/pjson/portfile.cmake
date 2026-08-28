# SPDX-License-Identifier: Apache-2.0

# This port is an in-repository overlay. Build the checkout that contains the
# port so local packaging validation cannot accidentally test an older archive.
get_filename_component(SOURCE_PATH "${CURRENT_PORT_DIR}/../../../.." ABSOLUTE)

# ---- Configure and install ---------------------------------------------

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DPJSON_BUILD_TESTS=OFF
        -DPJSON_BUILD_EXAMPLES=OFF
        -DPJSON_BUILD_BENCHMARKS=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME pjson
    CONFIG_PATH lib/cmake/pjson
)
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

# Headers and package metadata are configuration-independent; retain only the
# release copies to avoid duplicate files in the debug package subtree.
file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
