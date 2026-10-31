# ron_engine.dll — Vulkan renderer, window, input, audio, PNG and glTF loading,
# with a flat C API that Ron programs import with `extern "ron_engine.dll"`.

set(SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_OUT ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})

set(SHADER_INCLUDES)
foreach(shader mesh.vert mesh.frag shadow.vert)
    set(src ${SHADER_DIR}/${shader})
    set(out ${SHADER_OUT}/${shader}.inc)
    add_custom_command(
        OUTPUT ${out}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} -O -mfmt=num -o ${out} ${src}
        DEPENDS ${src} ${SHADER_DIR}/common.glsl
        COMMENT "Compiling shader ${shader}")
    list(APPEND SHADER_INCLUDES ${out})
endforeach()

# Asset loaders (no Vulkan dependency, also used by the tests and tools)
add_library(ron_assets STATIC png.cpp gltf.cpp)
target_include_directories(ron_assets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(ron_engine SHARED
    engine.cpp
    audio.cpp
    ron_engine.h
    ${SHADER_INCLUDES}
)
target_include_directories(ron_engine PRIVATE ${SHADER_OUT})
target_link_libraries(ron_engine PRIVATE ron_assets Vulkan::Vulkan winmm user32)
target_compile_definitions(ron_engine PRIVATE RON_ENGINE_BUILD VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN)

# Put the DLL next to ronc.exe so `ronc build` can ship it with games.
set_target_properties(ron_engine PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")
add_dependencies(ronc ron_engine)

# Asset tests and the procedural character generator
add_executable(asset_tests ${CMAKE_SOURCE_DIR}/tests/asset_tests.cpp ${CMAKE_SOURCE_DIR}/tests/test_main.cpp)
target_link_libraries(asset_tests PRIVATE ron_assets)
target_include_directories(asset_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests)
target_compile_definitions(asset_tests PRIVATE RON_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME asset_tests COMMAND asset_tests)

add_executable(make_character ${CMAKE_SOURCE_DIR}/tools/make_character.cpp)
target_link_libraries(make_character PRIVATE ron_assets)
