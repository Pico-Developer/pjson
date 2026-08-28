# SPDX-License-Identifier: Apache-2.0

from conan import ConanFile
from conan.tools.build import can_run
from conan.tools.cmake import CMake, cmake_layout

import os


# ---- Conan package-consumer recipe --------------------------------------

# Builds and, when the target is executable on the host, runs a minimal client
# against the exact binary package produced by Conan's test-package workflow.
class PjsonTestConan(ConanFile):
    test_type = "explicit"
    settings = "os", "arch", "compiler", "build_type"
    generators = "CMakeDeps", "CMakeToolchain"

    # Depend on the package reference that initiated this test-package run.
    def requirements(self):
        self.requires(self.tested_reference_str)

    # Use Conan's conventional source/build folders for the CMake consumer.
    def layout(self):
        cmake_layout(self)

    # Configure and compile through the generated dependency and toolchain data.
    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    # Run under Conan's generated environment whenever the target executable is
    # runnable on this host; other builds remain compile-only checks.
    def test(self):
        if can_run(self):
            executable = os.path.join(self.cpp.build.bindirs[0], "pjson_package_test")
            self.run(executable, env="conanrun")
