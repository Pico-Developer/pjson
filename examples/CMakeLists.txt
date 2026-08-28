cmake_minimum_required(VERSION 3.21)

project(pjson_examples)

# Keep target names aligned with their source-file stems so the loop below can
# define every tutorial as an independently runnable program.
set(PJSON_EXAMPLES
    01_hello_world
    02_building_values
    03_parsing_and_reading
    04_editing
    05_parsing_and_errors
    06_schema_validation
    07_address_book
    08_streaming
    09_custom_allocator
)

# The examples intentionally use the library's minimum supported C++ standard.
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Each example links the in-tree library and includes its public headers.
foreach(example ${PJSON_EXAMPLES})
    add_executable(${example} src/${example}.cpp)
    target_include_directories(${example} PRIVATE "../pjsonlib/include")
    target_link_libraries(${example} pjson)
endforeach()
