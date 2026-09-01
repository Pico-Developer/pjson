# SPDX-License-Identifier: Apache-2.0

from conan import ConanFile
from conan.tools.build import check_min_cppstd
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy

import os


# ---- Conan package recipe ----------------------------------------------

# Keep Conan's package model aligned with the exported CMake target and
# pkg-config metadata installed by pjsonlib/CMakeLists.txt.
class PjsonConan(ConanFile):
    name = "pjson"
    version = "2.0.0"
    package_type = "library"

    license = "Apache-2.0"
    author = "Praveen Babu J D and pjson contributors"
    url = "https://github.com/Pico-Developer/pjson"
    homepage = "https://github.com/Pico-Developer/pjson"
    description = "An ultra-simple JSON value type for C++11"
    topics = ("json", "parser", "serialization", "schema")

    settings = "os", "arch", "compiler", "build_type"
    options = {
        "shared": [True, False],
        "fPIC": [True, False],
    }
    default_options = {
        "shared": False,
        "fPIC": True,
    }

    exports_sources = (
        "CMakeLists.txt",
        "LICENSE",
        "cmake/*",
        "pjsonlib/CMakeLists.txt",
        "pjsonlib/include/*",
        "pjsonlib/src/*",
    )

    # fPIC is a Unix-only option and is irrelevant for Windows binaries.
    def config_options(self):
        if self.settings.os == "Windows":
            self.options.rm_safe("fPIC")

    # Shared objects are inherently position-independent, so Conan should not
    # expose a redundant fPIC package-ID dimension for shared builds.
    def configure(self):
        if self.options.shared:
            self.options.rm_safe("fPIC")

    # Use Conan's standard source/build/generator directory layout.
    def layout(self):
        cmake_layout(self)

    # Reject compiler profiles that explicitly request a pre-C++11 dialect.
    def validate(self):
        if self.settings.compiler.get_safe("cppstd"):
            check_min_cppstd(self, "11")

    # Translate Conan package options into the project's CMake configuration.
    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["PJSON_BUILD_TESTS"] = False
        toolchain.variables["PJSON_BUILD_EXAMPLES"] = False
        toolchain.variables["PJSON_BUILD_BENCHMARKS"] = False
        toolchain.variables["BUILD_SHARED_LIBS"] = bool(self.options.shared)
        fpic = self.options.get_safe("fPIC")
        if fpic is not None:
            toolchain.variables["CMAKE_POSITION_INDEPENDENT_CODE"] = bool(fpic)
        toolchain.generate()

    # Configure and build through the Conan-generated CMake toolchain.
    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    # Install CMake/pkg-config metadata with the library and retain the license
    # in Conan's conventional package location.
    def package(self):
        copy(
            self,
            "LICENSE",
            src=self.source_folder,
            dst=os.path.join(self.package_folder, "licenses"),
        )
        cmake = CMake(self)
        cmake.install()

    # Advertise identical logical names to CMake and pkg-config so consumers do
    # not need build-system-specific target names.
    def package_info(self):
        self.cpp_info.set_property("cmake_file_name", "pjson")
        self.cpp_info.set_property("cmake_target_name", "pjson::pjson")
        self.cpp_info.set_property("pkg_config_name", "pjson")
        self.cpp_info.libs = ["pjson"]
