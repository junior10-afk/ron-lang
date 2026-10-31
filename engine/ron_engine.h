#pragma once

// ron_engine.dll public C API. Mirrors std/engine.ron.
// Matrices are 16 floats, column-major. Vectors are 3 floats.

#include <cstdint>

#ifdef RON_ENGINE_BUILD
#define RE_API extern "C" __declspec(dllexport)
#else
#define RE_API extern "C" __declspec(dllimport)
#endif

// Window & frame
RE_API bool engine_init(int32_t width, int32_t height, const char* title);
RE_API void engine_shutdown();
RE_API bool engine_running();
RE_API bool engine_begin_frame(float r, float g, float b);
RE_API void engine_end_frame();
RE_API void engine_set_title(const char* title);
RE_API int32_t engine_width();
RE_API int32_t engine_height();
RE_API float engine_aspect();
RE_API void engine_screenshot(const char* path);   // saves the next frame as a .bmp
// Environment: RON_SCREENSHOT=<file.bmp> [RON_SCREENSHOT_FRAME=<n>] captures frame n automatically.

// Time
RE_API double engine_time();
RE_API float engine_delta();
RE_API float engine_fps();

// Scene
RE_API void engine_set_camera(const float* view16, const float* proj16, const float* eye3);
RE_API void engine_set_light(float dx, float dy, float dz, float intensity);
RE_API void engine_set_light_color(float r, float g, float b);
RE_API void engine_set_ambient(float skyR, float skyG, float skyB, float groundR, float groundG, float groundB);
RE_API void engine_set_fog(float r, float g, float b, float density);
// For the next draws. worldUvScale > 0 maps textures in world space (repeats every 1/scale units).
RE_API void engine_set_material(float roughness, float metallic, float emissive, float worldUvScale);

// Meshes (returns a mesh id, or -1)
RE_API int32_t engine_mesh_cube();
RE_API int32_t engine_mesh_sphere(int32_t rings, int32_t segments);
RE_API int32_t engine_mesh_plane(float size);
// 8 floats per vertex: position, normal, uv
RE_API int32_t engine_mesh_create(const float* vertices, int32_t vertexCount, const uint32_t* indices, int32_t indexCount);
// 16 floats per vertex: position, normal, uv, 4 joint indices, 4 weights
RE_API int32_t engine_mesh_create_skinned(const float* vertices, int32_t vertexCount, const uint32_t* indices,
                                          int32_t indexCount);

// Textures (PNG decoder built in; returns a texture id, or -1)
RE_API int32_t engine_texture_load(const char* path);
RE_API int32_t engine_texture_create(int32_t width, int32_t height, const uint8_t* rgba);

// Drawing
RE_API void engine_draw(int32_t mesh, const float* model16, float r, float g, float b);
RE_API void engine_draw_checker(int32_t mesh, const float* model16, float r, float g, float b);
RE_API void engine_draw_textured(int32_t mesh, const float* model16, int32_t texture, float r, float g, float b);
RE_API void engine_draw_skinned(int32_t mesh, const float* model16, const float* joints16, int32_t jointCount,
                                int32_t texture, float r, float g, float b);

// glTF 2.0 models (.glb / .gltf) with skeletal animation
RE_API int32_t engine_model_load(const char* path);
RE_API int32_t engine_model_animation_count(int32_t model);
RE_API float engine_model_animation_duration(int32_t model, int32_t anim);
RE_API int32_t engine_model_find_animation(int32_t model, const char* name);
RE_API void engine_draw_model(int32_t model, const float* transform16, int32_t anim, float time);
RE_API void engine_draw_model_blend(int32_t model, const float* transform16, int32_t animA, float timeA,
                                    int32_t animB, float timeB, float weight);

// Input (key codes are Windows virtual-key codes)
RE_API bool engine_key_down(int32_t key);
RE_API bool engine_key_pressed(int32_t key);
RE_API bool engine_mouse_down(int32_t button);   // 0 = left, 1 = right, 2 = middle
RE_API bool engine_mouse_pressed(int32_t button);
RE_API float engine_mouse_dx();
RE_API float engine_mouse_dy();
RE_API float engine_mouse_wheel();

// Audio synthesizer: wave 0 = sine, 1 = square, 2 = saw, 3 = noise
RE_API void engine_play_tone(float frequency, float seconds, float volume, int32_t wave);
RE_API void engine_play_sweep(float fromHz, float toHz, float seconds, float volume, int32_t wave);
