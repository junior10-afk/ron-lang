cmake_minimum_required(VERSION 3.20)
project(Ron LANGUAGES CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /EHsc)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ─── Compiler core (shared by ronc and the tests) ───
add_library(ron_core STATIC
    src/ast.cpp
    src/codegen.cpp
    src/diagnostics.cpp
    src/driver.cpp
    src/lexer.cpp
    src/parser.cpp
    src/pe.cpp
    src/sema.cpp
    src/token.cpp
    src/types.cpp
    src/x64.cpp
)
target_include_directories(ron_core PUBLIC src)

# ─── ronc: the command-line compiler ───
add_executable(ronc src/main.cpp)
target_link_libraries(ronc PRIVATE ron_core)

# Ship the standard library next to ronc.exe
add_custom_command(TARGET ronc POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/std $<TARGET_FILE_DIR:ronc>/std)

# ─── ron_engine.dll: the Vulkan engine (optional, needs the Vulkan SDK) ───
if(NOT DEFINED ENV{VULKAN_SDK} AND EXISTS "C:/VulkanSDK")
    file(GLOB _vk_sdks "C:/VulkanSDK/*")
    list(SORT _vk_sdks)
    list(REVERSE _vk_sdks)
    list(GET _vk_sdks 0 _vk_sdk)
    set(ENV{VULKAN_SDK} "${_vk_sdk}")
endif()
find_package(Vulkan QUIET COMPONENTS glslc)
if(Vulkan_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/engine/CMakeLists.txt")
    add_subdirectory(engine)
else()
    message(STATUS "Vulkan SDK not found: ron_engine.dll will not be built")
endif()

# ─── Tests ───
add_executable(ron_tests
    tests/test_main.cpp
    tests/lexer_tests.cpp
    tests/parser_tests.cpp
    tests/sema_tests.cpp
    tests/e2e_tests.cpp
)
target_link_libraries(ron_tests PRIVATE ron_core)
target_compile_definitions(ron_tests PRIVATE RON_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME ron_tests COMMAND ron_tests)

# Standard library tests written in Ron, run with `ronc test`
file(GLOB RON_TEST_FILES ${CMAKE_SOURCE_DIR}/tests/ron/*.ron)
foreach(test_file ${RON_TEST_FILES})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_test(NAME ron_${test_name} COMMAND ronc test ${test_file})
endforeach()
