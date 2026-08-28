//
// Copyright 2025 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
// Entry point for the pjson unit test suite. The actual TEST() cases live in
// the sibling tests_*.cpp files and self-register with the harness. With no
// arguments it runs everything; CTest uses --run-test NAME for one case.
//
#include "test_harness.h"

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--list-tests") {
        return pjson_test::list_tests();
    }
    if (argc == 3 && std::string(argv[1]) == "--run-test") {
        return pjson_test::run_one(argv[2]);
    }
    if (argc != 1) {
        std::fprintf(stderr, "Usage: %s [--list-tests | --run-test NAME]\n", argv[0]);
        return 2;
    }
    return pjson_test::run_all();
}
