// ron_engine: window, input and a forward Vulkan renderer.
//
// Rendering: 4x MSAA, depth, directional shadow map with PCF, physically based
// materials (GGX), textures with mipmaps (bindless-style array), GPU skinning,
// glTF models with keyframe animation and blending, fog, ACES tone mapping.

#include "ron_engine.h"

#include "gltf.hpp"
#include "png.hpp"

#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

void audio_init();
void audio_shutdown();

namespace {

const uint32_t kVertSpv[] = {
#include "mesh.vert.inc"
};
const uint32_t kFragSpv[] = {
#include "mesh.frag.inc"
};
const uint32_t kShadowVertSpv[] = {
#include "shadow.vert.inc"
};

constexpr int kFramesInFlight = 2;
constexpr uint32_t kShadowMapSize = 2048;
constexpr float kShadowExtent = 18.0f;    // half-size of the light's orthographic box
constexpr uint32_t kMaxTextures = 256;    // must match textures[] in mesh.frag
constexpr uint32_t kMaxJoints = 4096;     // joint matrices per frame
constexpr int kFlagChecker = 1;
constexpr int kFlagSkinned = 2;

using Vertex = ron::gltf::Vertex;

struct Mesh {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
};

struct Texture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

struct SceneUniforms {
    float viewProj[16];
    float lightViewProj[16];
    float lightDir[4];
    float lightColor[4];
    float skyColor[4];
    float groundColor[4];
    float cameraPos[4];
    float fog[4];
};

struct PushConstants {
    float model0[4];
    float model1[4];
    float model2[4];
    float color[4];
    float material[4];   // roughness, metallic, emissive, unused
    int32_t info[4];     // texture, flags, first joint, unused
};
static_assert(sizeof(PushConstants) == 96, "push constants must stay within the guaranteed 128 bytes");

// Draws are queued during the frame and recorded at the end, so the shadow pass
// can render every object before the main pass.
struct DrawCommand {
    int32_t mesh;
    float model[16];
    float color[4];
    float material[4];
    int32_t texture;
    int32_t flags;
    int32_t jointBase;
};

struct Model {
    ron::gltf::Model data;
    std::vector<std::vector<int32_t>> primitiveMeshes;   // [mesh][primitive] -> engine mesh id
    std::vector<int32_t> imageTextures;                   // [image] -> engine texture id
};

struct Engine {
    // Window
    HINSTANCE instanceHandle = nullptr;
    HWND hwnd = nullptr;
    bool quit = false;
    bool resized = false;
    int width = 0, height = 0;
    std::string title;

    // Input
    bool keys[256] = {};
    bool prevKeys[256] = {};
    bool mouse[3] = {};
    bool prevMouse[3] = {};
    bool haveLastMouse = false;
    int lastMouseX = 0, lastMouseY = 0;
    float accDx = 0, accDy = 0, accWheel = 0;
    float mouseDx = 0, mouseDy = 0, wheel = 0;

    // Time
    LARGE_INTEGER freq{}, start{}, lastFrame{};
    float delta = 1.0f / 60.0f;
    float fps = 0;
    float fpsTimer = 0;
    int fpsFrames = 0;

    // Vulkan core
    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};
    float maxAnisotropy = 1.0f;

    // Swapchain and targets
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkSemaphore> renderFinished;   // one per swapchain image
    VkImage msaaImage = VK_NULL_HANDLE;
    VkDeviceMemory msaaMemory = VK_NULL_HANDLE;
    VkImageView msaaView = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView depthView = VK_NULL_HANDLE;
    bool canCopySwapchain = false;

    // Pipelines (both share one layout and descriptor set)
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    // Shadow mapping
    VkImage shadowImage = VK_NULL_HANDLE;
    VkDeviceMemory shadowMemory = VK_NULL_HANDLE;
    VkImageView shadowView = VK_NULL_HANDLE;
    VkSampler shadowSampler = VK_NULL_HANDLE;
    VkRenderPass shadowPass = VK_NULL_HANDLE;
    VkFramebuffer shadowFramebuffer = VK_NULL_HANDLE;
    VkPipeline shadowPipeline = VK_NULL_HANDLE;

    // Textures
    VkSampler textureSampler = VK_NULL_HANDLE;
    std::vector<Texture> textures;

    // Per frame in flight
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commands[kFramesInFlight] = {};
    VkSemaphore imageAvailable[kFramesInFlight] = {};
    VkFence inFlight[kFramesInFlight] = {};
    VkBuffer uniformBuffers[kFramesInFlight] = {};
    VkDeviceMemory uniformMemory[kFramesInFlight] = {};
    void* uniformMapped[kFramesInFlight] = {};
    VkBuffer jointBuffers[kFramesInFlight] = {};
    VkDeviceMemory jointMemory[kFramesInFlight] = {};
    float* jointMapped[kFramesInFlight] = {};
    uint32_t jointCursor = 0;
    VkDescriptorSet descriptorSets[kFramesInFlight] = {};

    uint32_t frame = 0;
    uint32_t imageIndex = 0;
    bool frameActive = false;
    bool initialized = false;

    // Screenshot request (written after the frame completes)
    std::string screenshotPath;
    int screenshotAtFrame = -1;   // from RON_SCREENSHOT_FRAME
    uint64_t frameCounter = 0;

    std::vector<Mesh> meshes;
    std::vector<std::unique_ptr<Model>> models;
    std::vector<DrawCommand> draws;
    float clearColor[3] = {};
    float material[4] = {0.6f, 0.0f, 0.0f, 0.0f};   // default roughness, metallic, emissive, world UV scale
    float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    SceneUniforms scene{};
};

Engine g;

bool check(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return true;
    std::fprintf(stderr, "ron_engine: %s failed (VkResult %d)\n", what, static_cast<int>(r));
    return false;
}

void mat4Mul(const float* a, const float* b, float* out) {
    float r[16];
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k];
            r[col * 4 + row] = s;
        }
    std::memcpy(out, r, sizeof(r));
}

// ───────────────────────────── Window ─────────────────────────────

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g.quit = true;
        return 0;
    case WM_SIZE:
        g.width = LOWORD(lp);
        g.height = HIWORD(lp);
        g.resized = true;
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp < 256) g.keys[wp] = true;
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp < 256) g.keys[wp] = false;
        return 0;
    case WM_KILLFOCUS:
        std::memset(g.keys, 0, sizeof(g.keys));
        std::memset(g.mouse, 0, sizeof(g.mouse));
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        int b = msg == WM_LBUTTONDOWN ? 0 : msg == WM_RBUTTONDOWN ? 1 : 2;
        g.mouse[b] = true;
        SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        int b = msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2;
        g.mouse[b] = false;
        if (!g.mouse[0] && !g.mouse[1] && !g.mouse[2]) ReleaseCapture();
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = static_cast<short>(LOWORD(lp));
        int y = static_cast<short>(HIWORD(lp));
        if (g.haveLastMouse) {
            g.accDx += static_cast<float>(x - g.lastMouseX);
            g.accDy += static_cast<float>(y - g.lastMouseY);
        }
        g.lastMouseX = x;
        g.lastMouseY = y;
        g.haveLastMouse = true;
        return 0;
    }
    case WM_MOUSEWHEEL:
        g.accWheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool createWindow(int width, int height, const char* title) {
    SetProcessDPIAware();
    g.instanceHandle = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = g.instanceHandle;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hIcon = LoadIconW(nullptr, reinterpret_cast<LPCWSTR>(IDI_APPLICATION));
    wc.lpszClassName = L"RonEngineWindow";
    RegisterClassExW(&wc);

    RECT r{0, 0, width, height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    wchar_t wtitle[256];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, wtitle, 256);
    g.hwnd = CreateWindowExW(0, wc.lpszClassName, wtitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, g.instanceHandle, nullptr);
    if (!g.hwnd) return false;
    g.title = title;
    g.width = width;
    g.height = height;
    ShowWindow(g.hwnd, SW_SHOW);
    SetForegroundWindow(g.hwnd);
    return true;
}

// ───────────────────────────── Vulkan helpers ─────────────────────────────

uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < g.memProps.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (g.memProps.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buffer,
                  VkDeviceMemory& memory) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!check(vkCreateBuffer(g.device, &bi, nullptr, &buffer), "vkCreateBuffer")) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.device, buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    if (!check(vkAllocateMemory(g.device, &ai, nullptr, &memory), "vkAllocateMemory")) return false;
    vkBindBufferMemory(g.device, buffer, memory, 0);
    return true;
}

bool createImage(uint32_t w, uint32_t h, uint32_t mips, VkFormat format, VkImageUsageFlags usage,
                 VkSampleCountFlagBits samples, VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory,
                 VkImageView& view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = mips;
    ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!check(vkCreateImage(g.device, &ii, nullptr, &image), "vkCreateImage")) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g.device, image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!check(vkAllocateMemory(g.device, &ai, nullptr, &memory), "vkAllocateMemory (image)")) return false;
    vkBindImageMemory(g.device, image, memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, mips, 0, 1};
    return check(vkCreateImageView(g.device, &vi, nullptr, &view), "vkCreateImageView");
}

void destroyImage(VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    if (view) vkDestroyImageView(g.device, view, nullptr);
    if (image) vkDestroyImage(g.device, image, nullptr);
    if (memory) vkFreeMemory(g.device, memory, nullptr);
    view = VK_NULL_HANDLE;
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                  uint32_t baseMip = 0, uint32_t mipCount = 1) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mipCount, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// One-off command buffer for uploads (waits for completion).
VkCommandBuffer beginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.commandPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(g.device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void endOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g.queue);
    vkFreeCommandBuffers(g.device, g.commandPool, 1, &cmd);
}

VkShaderModule shaderModule(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(g.device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

// ───────────────────────────── Vulkan setup ─────────────────────────────

bool createInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = g.title.c_str();
    app.pEngineName = "Ron Engine";
    app.engineVersion = VK_MAKE_VERSION(0, 2, 0);
    app.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> layers;
    if (std::getenv("RON_VULKAN_VALIDATION")) layers.push_back("VK_LAYER_KHRONOS_validation");

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = 2;
    ci.ppEnabledExtensionNames = extensions;
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (vkCreateInstance(&ci, nullptr, &g.instance) != VK_SUCCESS && !layers.empty()) {
        ci.enabledLayerCount = 0;   // validation layer missing: run without it
        return check(vkCreateInstance(&ci, nullptr, &g.instance), "vkCreateInstance");
    }
    return g.instance != VK_NULL_HANDLE;
}

bool pickDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(g.instance, &count, nullptr);
    std::vector<VkPhysicalDevice> gpus(count);
    vkEnumeratePhysicalDevices(g.instance, &count, gpus.data());

    int bestScore = -1;
    for (VkPhysicalDevice gpu : gpus) {
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> qprops(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qcount, qprops.data());

        uint32_t extCount = 0;
        vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, exts.data());
        bool hasSwapchain = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
            return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });
        VkPhysicalDeviceFeatures features;
        vkGetPhysicalDeviceFeatures(gpu, &features);
        if (!hasSwapchain || !features.shaderSampledImageArrayDynamicIndexing) continue;

        for (uint32_t q = 0; q < qcount; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpu, q, g.surface, &present);
            if (!(qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(gpu, &props);
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore = score;
                g.gpu = gpu;
                g.queueFamily = q;
            }
            break;
        }
    }
    if (!g.gpu) {
        std::fprintf(stderr, "ron_engine: no Vulkan GPU with graphics + present support\n");
        return false;
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g.gpu, &props);
    vkGetPhysicalDeviceMemoryProperties(g.gpu, &g.memProps);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    g.samples = (counts & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT
              : (counts & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT
                                                 : VK_SAMPLE_COUNT_1_BIT;
    g.maxAnisotropy = std::min(8.0f, props.limits.maxSamplerAnisotropy);
    std::fprintf(stderr, "ron_engine: GPU '%s', %dx MSAA\n", props.deviceName, static_cast<int>(g.samples));

    // Surface format: prefer an sRGB swapchain so shading happens in linear space.
    uint32_t fcount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.gpu, g.surface, &fcount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fcount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.gpu, g.surface, &fcount, formats.data());
    g.colorFormat = formats[0].format;
    g.colorSpace = formats[0].colorSpace;
    for (const auto& f : formats) {
        if ((f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            g.colorFormat = f.format;
            g.colorSpace = f.colorSpace;
            break;
        }
    }
    return true;
}

bool createDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g.queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures available;
    vkGetPhysicalDeviceFeatures(g.gpu, &available);
    VkPhysicalDeviceFeatures features{};
    features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    features.samplerAnisotropy = available.samplerAnisotropy;
    if (!available.samplerAnisotropy) g.maxAnisotropy = 1.0f;
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = exts;
    di.pEnabledFeatures = &features;
    if (!check(vkCreateDevice(g.gpu, &di, nullptr, &g.device), "vkCreateDevice")) return false;
    vkGetDeviceQueue(g.device, g.queueFamily, 0, &g.queue);
    return true;
}

bool createRenderPass() {
    bool msaa = g.samples != VK_SAMPLE_COUNT_1_BIT;
    std::vector<VkAttachmentDescription> att;

    VkAttachmentDescription color{};
    color.format = g.colorFormat;
    color.samples = g.samples;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = msaa ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.push_back(color);

    VkAttachmentDescription depth{};
    depth.format = g.depthFormat;
    depth.samples = g.samples;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att.push_back(depth);

    if (msaa) {
        VkAttachmentDescription resolve{};
        resolve.format = g.colorFormat;
        resolve.samples = VK_SAMPLE_COUNT_1_BIT;
        resolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        resolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        resolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        resolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        resolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        att.push_back(resolve);
    }

    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolveRef{2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    sub.pDepthStencilAttachment = &depthRef;
    sub.pResolveAttachments = msaa ? &resolveRef : nullptr;

    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep.dstStageMask = dep.srcStageMask;
    dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = static_cast<uint32_t>(att.size());
    rp.pAttachments = att.data();
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    return check(vkCreateRenderPass(g.device, &rp, nullptr, &g.renderPass), "vkCreateRenderPass");
}

bool createLayout() {
    VkDescriptorSetLayoutBinding b[4] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 4;
    sl.pBindings = b;
    if (!check(vkCreateDescriptorSetLayout(g.device, &sl, nullptr, &g.setLayout), "vkCreateDescriptorSetLayout"))
        return false;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    return check(vkCreatePipelineLayout(g.device, &pl, nullptr, &g.pipelineLayout), "vkCreatePipelineLayout");
}

// Vertex layout shared by both pipelines (shadow uses a subset).
void vertexInput(VkVertexInputBindingDescription& binding, std::vector<VkVertexInputAttributeDescription>& attrs,
                 bool shadow) {
    binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    attrs.push_back({0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)});
    if (!shadow) {
        attrs.push_back({1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)});
        attrs.push_back({2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)});
    }
    attrs.push_back({3, 0, VK_FORMAT_R8G8B8A8_UINT, offsetof(Vertex, joints)});
    attrs.push_back({4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, weights)});
}

bool createPipeline() {
    VkShaderModule vert = shaderModule(kVertSpv, sizeof(kVertSpv));
    VkShaderModule frag = shaderModule(kFragSpv, sizeof(kFragSpv));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
    };

    VkVertexInputBindingDescription vb;
    std::vector<VkVertexInputAttributeDescription> attrs;
    vertexInput(vb, attrs, false);
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // the shader flips normals of back faces
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = g.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = g.pipelineLayout;
    pi.renderPass = g.renderPass;
    bool ok = check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pi, nullptr, &g.pipeline),
                    "vkCreateGraphicsPipelines");
    vkDestroyShaderModule(g.device, vert, nullptr);
    vkDestroyShaderModule(g.device, frag, nullptr);
    return ok;
}

bool createShadowResources() {
    if (!createImage(kShadowMapSize, kShadowMapSize, 1, VK_FORMAT_D32_SFLOAT,
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT,
                     VK_IMAGE_ASPECT_DEPTH_BIT, g.shadowImage, g.shadowMemory, g.shadowView))
        return false;

    // Comparison sampler: texture() returns the filtered "lit" fraction directly.
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    si.compareEnable = VK_TRUE;
    si.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    si.maxLod = 1.0f;
    if (!check(vkCreateSampler(g.device, &si, nullptr, &g.shadowSampler), "vkCreateSampler")) return false;

    VkAttachmentDescription depth{};
    depth.format = VK_FORMAT_D32_SFLOAT;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.pDepthStencilAttachment = &ref;
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &depth;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    if (!check(vkCreateRenderPass(g.device, &rp, nullptr, &g.shadowPass), "vkCreateRenderPass (shadow)")) return false;

    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g.shadowPass;
    fi.attachmentCount = 1;
    fi.pAttachments = &g.shadowView;
    fi.width = kShadowMapSize;
    fi.height = kShadowMapSize;
    fi.layers = 1;
    if (!check(vkCreateFramebuffer(g.device, &fi, nullptr, &g.shadowFramebuffer), "vkCreateFramebuffer (shadow)"))
        return false;

    VkShaderModule vert = shaderModule(kShadowVertSpv, sizeof(kShadowVertSpv));
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                          VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    VkVertexInputBindingDescription vb;
    std::vector<VkVertexInputAttributeDescription> attrs;
    vertexInput(vb, attrs, true);
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0, 1};
    VkRect2D scissor{{0, 0}, {kShadowMapSize, kShadowMapSize}};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.pViewports = &viewport;
    vp.scissorCount = 1;
    vp.pScissors = &scissor;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = VK_TRUE;
    rs.depthBiasConstantFactor = 1.25f;
    rs.depthBiasSlopeFactor = 1.75f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.stageCount = 1;
    pi.pStages = &stage;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.layout = g.pipelineLayout;
    pi.renderPass = g.shadowPass;
    bool ok = check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pi, nullptr, &g.shadowPipeline),
                    "vkCreateGraphicsPipelines (shadow)");
    vkDestroyShaderModule(g.device, vert, nullptr);
    return ok;
}

// Points every texture slot of both descriptor sets at `textureId` (slot `slot`).
void writeTextureSlot(uint32_t slot, int32_t textureId) {
    VkDescriptorImageInfo info{g.textureSampler, g.textures[textureId].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (int f = 0; f < kFramesInFlight; ++f) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = g.descriptorSets[f];
        w.dstBinding = 2;
        w.dstArrayElement = slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(g.device, 1, &w, 0, nullptr);
    }
}

// Uploads RGBA8 pixels as an sRGB texture with a full mip chain; returns its id or -1.
int32_t createTexture(uint32_t w, uint32_t h, const uint8_t* rgba) {
    if (g.textures.size() >= kMaxTextures || w == 0 || h == 0) return -1;
    uint32_t mips = 1;
    while ((std::max(w, h) >> mips) > 0) ++mips;

    VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    VkBuffer staging;
    VkDeviceMemory stagingMemory;
    if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMemory))
        return -1;
    void* p;
    vkMapMemory(g.device, stagingMemory, 0, bytes, 0, &p);
    std::memcpy(p, rgba, bytes);
    vkUnmapMemory(g.device, stagingMemory);

    Texture t;
    if (!createImage(w, h, mips, VK_FORMAT_R8G8B8A8_SRGB,
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                     VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, t.image, t.memory, t.view))
        return -1;

    VkCommandBuffer cmd = beginOneShot();
    imageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, mips);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, staging, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Mip chain: each level is a linear blit of the previous one.
    int32_t mw = static_cast<int32_t>(w), mh = static_cast<int32_t>(h);
    for (uint32_t level = 1; level < mips; ++level) {
        imageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, level - 1, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {mw, mh, 1};
        int32_t nw = std::max(1, mw / 2), nh = std::max(1, mh / 2);
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blit, VK_FILTER_LINEAR);
        imageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, level - 1, 1);
        mw = nw;
        mh = nh;
    }
    imageBarrier(cmd, t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, mips - 1, 1);
    endOneShot(cmd);
    vkDestroyBuffer(g.device, staging, nullptr);
    vkFreeMemory(g.device, stagingMemory, nullptr);

    g.textures.push_back(t);
    auto id = static_cast<int32_t>(g.textures.size() - 1);
    if (g.descriptorSets[0]) {
        vkDeviceWaitIdle(g.device);   // descriptor sets may be in use by frames in flight
        writeTextureSlot(static_cast<uint32_t>(id), id);
    }
    return id;
}

bool createFrameResources() {
    VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = g.queueFamily;
    if (!check(vkCreateCommandPool(g.device, &cp, nullptr, &g.commandPool), "vkCreateCommandPool")) return false;
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = g.commandPool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = kFramesInFlight;
    if (!check(vkAllocateCommandBuffers(g.device, &ca, g.commands), "vkAllocateCommandBuffers")) return false;

    // Texture sampler (trilinear + anisotropic, repeating) and the default white texture.
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.anisotropyEnable = g.maxAnisotropy > 1.0f;
    si.maxAnisotropy = g.maxAnisotropy;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (!check(vkCreateSampler(g.device, &si, nullptr, &g.textureSampler), "vkCreateSampler (textures)")) return false;
    const uint8_t white[4] = {255, 255, 255, 255};
    if (createTexture(1, 1, white) != 0) return false;

    VkDescriptorPoolSize poolSizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight * (1 + kMaxTextures)},
                                         {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kFramesInFlight}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kFramesInFlight;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = poolSizes;
    if (!check(vkCreateDescriptorPool(g.device, &dp, nullptr, &g.descriptorPool), "vkCreateDescriptorPool"))
        return false;

    VkDescriptorSetLayout layouts[kFramesInFlight] = {g.setLayout, g.setLayout};
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = g.descriptorPool;
    da.descriptorSetCount = kFramesInFlight;
    da.pSetLayouts = layouts;
    if (!check(vkAllocateDescriptorSets(g.device, &da, g.descriptorSets), "vkAllocateDescriptorSets")) return false;

    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (!createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, host, g.uniformBuffers[i],
                          g.uniformMemory[i]) ||
            !createBuffer(kMaxJoints * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, host, g.jointBuffers[i], g.jointMemory[i]))
            return false;
        vkMapMemory(g.device, g.uniformMemory[i], 0, sizeof(SceneUniforms), 0, &g.uniformMapped[i]);
        void* jm;
        vkMapMemory(g.device, g.jointMemory[i], 0, kMaxJoints * 64, 0, &jm);
        g.jointMapped[i] = static_cast<float*>(jm);
        // Joint 0 is the identity (used by static meshes, harmless if read).
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(g.jointMapped[i], identity, 64);

        VkDescriptorBufferInfo ub{g.uniformBuffers[i], 0, sizeof(SceneUniforms)};
        VkDescriptorBufferInfo jb{g.jointBuffers[i], 0, kMaxJoints * 64};
        VkDescriptorImageInfo shadow{g.shadowSampler, g.shadowView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        std::vector<VkDescriptorImageInfo> texInfos(kMaxTextures,
                                                    {g.textureSampler, g.textures[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet w[4] = {};
        for (auto& x : w) {
            x.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            x.dstSet = g.descriptorSets[i];
            x.descriptorCount = 1;
        }
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &ub;
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &shadow;
        w[2].dstBinding = 2;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].descriptorCount = kMaxTextures;
        w[2].pImageInfo = texInfos.data();
        w[3].dstBinding = 3;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[3].pBufferInfo = &jb;
        vkUpdateDescriptorSets(g.device, 4, w, 0, nullptr);

        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!check(vkCreateSemaphore(g.device, &sci, nullptr, &g.imageAvailable[i]), "vkCreateSemaphore") ||
            !check(vkCreateFence(g.device, &fi, nullptr, &g.inFlight[i]), "vkCreateFence"))
            return false;
    }
    return true;
}

void destroySwapchain() {
    for (VkFramebuffer fb : g.framebuffers) vkDestroyFramebuffer(g.device, fb, nullptr);
    for (VkImageView v : g.views) vkDestroyImageView(g.device, v, nullptr);
    for (VkSemaphore s : g.renderFinished) vkDestroySemaphore(g.device, s, nullptr);
    g.framebuffers.clear();
    g.views.clear();
    g.renderFinished.clear();
    destroyImage(g.msaaImage, g.msaaMemory, g.msaaView);
    destroyImage(g.depthImage, g.depthMemory, g.depthView);
    if (g.swapchain) vkDestroySwapchainKHR(g.device, g.swapchain, nullptr);
    g.swapchain = VK_NULL_HANDLE;
}

bool createSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.gpu, g.surface, &caps);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(static_cast<uint32_t>(g.width), caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(static_cast<uint32_t>(g.height), caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) return false;   // minimized
    g.extent = extent;

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                alpha = a;
                break;
            }
    }

    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = g.surface;
    sc.minImageCount = imageCount;
    sc.imageFormat = g.colorFormat;
    sc.imageColorSpace = g.colorSpace;
    sc.imageExtent = extent;
    sc.imageArrayLayers = 1;
    g.canCopySwapchain = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (g.canCopySwapchain ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    sc.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sc.preTransform = caps.currentTransform;
    sc.compositeAlpha = alpha;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // vsync, always supported
    sc.clipped = VK_TRUE;
    if (!check(vkCreateSwapchainKHR(g.device, &sc, nullptr, &g.swapchain), "vkCreateSwapchainKHR")) return false;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &count, nullptr);
    g.images.resize(count);
    vkGetSwapchainImagesKHR(g.device, g.swapchain, &count, g.images.data());

    bool msaa = g.samples != VK_SAMPLE_COUNT_1_BIT;
    if (msaa && !createImage(extent.width, extent.height, 1, g.colorFormat,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, g.samples,
                             VK_IMAGE_ASPECT_COLOR_BIT, g.msaaImage, g.msaaMemory, g.msaaView))
        return false;
    if (!createImage(extent.width, extent.height, 1, g.depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                     g.samples, VK_IMAGE_ASPECT_DEPTH_BIT, g.depthImage, g.depthMemory, g.depthView))
        return false;

    for (VkImage img : g.images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = g.colorFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        if (!check(vkCreateImageView(g.device, &vi, nullptr, &view), "vkCreateImageView (swapchain)")) return false;
        g.views.push_back(view);

        std::vector<VkImageView> att;
        if (msaa) att = {g.msaaView, g.depthView, view};
        else att = {view, g.depthView};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g.renderPass;
        fi.attachmentCount = static_cast<uint32_t>(att.size());
        fi.pAttachments = att.data();
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        VkFramebuffer fb;
        if (!check(vkCreateFramebuffer(g.device, &fi, nullptr, &fb), "vkCreateFramebuffer")) return false;
        g.framebuffers.push_back(fb);

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore s;
        vkCreateSemaphore(g.device, &si, nullptr, &s);
        g.renderFinished.push_back(s);
    }
    return true;
}

bool recreateSwapchain() {
    vkDeviceWaitIdle(g.device);
    destroySwapchain();
    return createSwapchain();
}

int32_t uploadMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices) {
    if (vertices.empty() || indices.empty()) return -1;
    Mesh m;
    VkDeviceSize vbytes = vertices.size() * sizeof(Vertex);
    VkDeviceSize ibytes = indices.size() * sizeof(uint32_t);
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (!createBuffer(vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host, m.vertexBuffer, m.vertexMemory) ||
        !createBuffer(ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host, m.indexBuffer, m.indexMemory))
        return -1;
    void* p;
    vkMapMemory(g.device, m.vertexMemory, 0, vbytes, 0, &p);
    std::memcpy(p, vertices.data(), vbytes);
    vkUnmapMemory(g.device, m.vertexMemory);
    vkMapMemory(g.device, m.indexMemory, 0, ibytes, 0, &p);
    std::memcpy(p, indices.data(), ibytes);
    vkUnmapMemory(g.device, m.indexMemory);
    m.indexCount = static_cast<uint32_t>(indices.size());
    g.meshes.push_back(m);
    return static_cast<int32_t>(g.meshes.size() - 1);
}

Vertex makeVertex(float px, float py, float pz, float nx, float ny, float nz, float u, float v) {
    Vertex vx{};
    vx.position[0] = px; vx.position[1] = py; vx.position[2] = pz;
    vx.normal[0] = nx; vx.normal[1] = ny; vx.normal[2] = nz;
    vx.uv[0] = u; vx.uv[1] = v;
    vx.weights[0] = 1.0f;
    return vx;
}

void setDefaultScene() {
    float lx = -0.45f, ly = -1.0f, lz = -0.3f;
    float len = std::sqrt(lx * lx + ly * ly + lz * lz);
    g.scene = SceneUniforms{};
    float dir[4] = {lx / len, ly / len, lz / len, 1.6f};
    float color[4] = {1.0f, 0.93f, 0.8f, 1.0f};
    float sky[4] = {0.22f, 0.28f, 0.38f, 1.0f};
    float ground[4] = {0.1f, 0.08f, 0.06f, 1.0f};
    float fog[4] = {0.45f, 0.55f, 0.7f, 0.018f};
    std::memcpy(g.scene.lightDir, dir, 16);
    std::memcpy(g.scene.lightColor, color, 16);
    std::memcpy(g.scene.skyColor, sky, 16);
    std::memcpy(g.scene.groundColor, ground, 16);
    std::memcpy(g.scene.fog, fog, 16);
}

// Copies joint matrices into this frame's storage buffer; returns the first index or -1.
int32_t pushJoints(const float* matrices, int32_t count) {
    if (count <= 0 || g.jointCursor + static_cast<uint32_t>(count) > kMaxJoints) return -1;
    std::memcpy(g.jointMapped[g.frame] + g.jointCursor * 16, matrices, static_cast<size_t>(count) * 64);
    auto base = static_cast<int32_t>(g.jointCursor);
    g.jointCursor += static_cast<uint32_t>(count);
    return base;
}

void queueDraw(int32_t mesh, const float* model, int32_t texture, float r, float g2, float b, float roughness,
               float metallic, float emissive, int32_t flags, int32_t jointBase, float uvScale = 0.0f) {
    if (!g.frameActive || mesh < 0 || mesh >= static_cast<int32_t>(g.meshes.size())) return;
    if (texture < 0 || texture >= static_cast<int32_t>(g.textures.size())) texture = 0;
    DrawCommand d;
    d.mesh = mesh;
    std::memcpy(d.model, model, sizeof(d.model));
    d.color[0] = r;
    d.color[1] = g2;
    d.color[2] = b;
    d.color[3] = 1.0f;
    d.material[0] = roughness;
    d.material[1] = metallic;
    d.material[2] = emissive;
    d.material[3] = uvScale;
    d.texture = texture;
    d.flags = flags;
    d.jointBase = jointBase;
    g.draws.push_back(d);
}

void bindMesh(VkCommandBuffer cmd, const Mesh& m) {
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
}

void pushConstantsFor(const DrawCommand& d, PushConstants& pc) {
    for (int row = 0; row < 3; ++row) {
        float* dst = row == 0 ? pc.model0 : row == 1 ? pc.model1 : pc.model2;
        for (int col = 0; col < 4; ++col) dst[col] = d.model[col * 4 + row];
    }
    std::memcpy(pc.color, d.color, sizeof(pc.color));
    std::memcpy(pc.material, d.material, sizeof(pc.material));
    pc.info[0] = d.texture;
    pc.info[1] = d.flags;
    pc.info[2] = d.jointBase;
    pc.info[3] = 0;
}

// Orthographic light camera covering a box around the point the camera looks at.
void computeLightViewProj(float* out) {
    // Camera forward is the negated third row of the view matrix.
    float fx = -g.view[2], fy = -g.view[6], fz = -g.view[10];
    float cx = g.scene.cameraPos[0] + fx * kShadowExtent * 0.6f;
    float cy = g.scene.cameraPos[1] + fy * kShadowExtent * 0.6f;
    float cz = g.scene.cameraPos[2] + fz * kShadowExtent * 0.6f;
    cy = std::max(cy, 0.0f);

    const float* L = g.scene.lightDir;
    float upx = 0, upy = 1, upz = 0;
    if (std::fabs(L[1]) > 0.99f) { upy = 0; upz = 1; }
    float rx = L[1] * upz - L[2] * upy, ry = L[2] * upx - L[0] * upz, rz = L[0] * upy - L[1] * upx;
    float rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    rx /= rl; ry /= rl; rz /= rl;
    float ux = ry * L[2] - rz * L[1], uy = rz * L[0] - rx * L[2], uz = rx * L[1] - ry * L[0];

    // Snap the center to shadow-map texels so shadows do not shimmer when the camera moves.
    float texel = 2.0f * kShadowExtent / static_cast<float>(kShadowMapSize);
    float pr = std::floor((rx * cx + ry * cy + rz * cz) / texel) * texel;
    float pu = std::floor((ux * cx + uy * cy + uz * cz) / texel) * texel;
    float pf = L[0] * cx + L[1] * cy + L[2] * cz;

    const float depth = 80.0f;
    float view[16] = {rx, ux, L[0], 0, ry, uy, L[1], 0, rz, uz, L[2], 0, -pr, -pu, -pf, 1};
    float proj[16] = {};
    proj[0] = 1.0f / kShadowExtent;
    proj[5] = 1.0f / kShadowExtent;
    proj[10] = 1.0f / (2.0f * depth);
    proj[14] = 0.5f;
    proj[15] = 1.0f;
    mat4Mul(proj, view, out);
}

void recordFrame(VkCommandBuffer cmd) {
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    // 1) Shadow map
    VkClearValue shadowClear;
    shadowClear.depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo sp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    sp.renderPass = g.shadowPass;
    sp.framebuffer = g.shadowFramebuffer;
    sp.renderArea = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
    sp.clearValueCount = 1;
    sp.pClearValues = &shadowClear;
    vkCmdBeginRenderPass(cmd, &sp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.shadowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipelineLayout, 0, 1, &g.descriptorSets[g.frame],
                            0, nullptr);
    for (const DrawCommand& d : g.draws) {
        if (d.flags & kFlagChecker) continue;   // ground planes do not cast shadows
        PushConstants pc;
        pushConstantsFor(d, pc);
        const Mesh& m = g.meshes[d.mesh];
        bindMesh(cmd, m);
        vkCmdPushConstants(cmd, g.pipelineLayout, stages, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd);

    // 2) Main pass
    VkClearValue clears[3];
    clears[0].color = {{g.clearColor[0], g.clearColor[1], g.clearColor[2], 1.0f}};
    clears[1].depthStencil = {1.0f, 0};
    clears[2].color = clears[0].color;
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = g.renderPass;
    rp.framebuffer = g.framebuffers[g.imageIndex];
    rp.renderArea = {{0, 0}, g.extent};
    rp.clearValueCount = g.samples != VK_SAMPLE_COUNT_1_BIT ? 3 : 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, static_cast<float>(g.extent.width), static_cast<float>(g.extent.height), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, g.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipelineLayout, 0, 1, &g.descriptorSets[g.frame],
                            0, nullptr);
    for (const DrawCommand& d : g.draws) {
        PushConstants pc;
        pushConstantsFor(d, pc);
        const Mesh& m = g.meshes[d.mesh];
        bindMesh(cmd, m);
        vkCmdPushConstants(cmd, g.pipelineLayout, stages, 0, sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
}

constexpr float kExposure = 0.6f;   // must match EXPOSURE in mesh.frag
float aces(float x) {
    x *= kExposure;
    return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f);
}

// Writes a 24-bit BMP from 4-byte pixels (BGRA or RGBA swapchain formats).
void writeBmp(const std::string& path, const uint8_t* pixels, uint32_t w, uint32_t h, bool bgra) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t rowBytes = (w * 3 + 3) & ~3u;
    uint32_t dataSize = rowBytes * h;
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2, 54 + dataSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    header[26] = 1;
    header[28] = 24;
    put32(34, dataSize);
    std::fwrite(header, 1, 54, f);
    std::vector<uint8_t> row(rowBytes, 0);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = pixels + static_cast<size_t>(h - 1 - y) * w * 4;   // BMP rows go bottom-up
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 3 + 0] = bgra ? src[x * 4 + 0] : src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = bgra ? src[x * 4 + 2] : src[x * 4 + 0];
        }
        std::fwrite(row.data(), 1, rowBytes, f);
    }
    std::fclose(f);
}

// Records a copy of the current swapchain image into a host buffer.
void captureFrame(VkCommandBuffer cmd, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkDeviceSize bytes = static_cast<VkDeviceSize>(g.extent.width) * g.extent.height * 4;
    if (!createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buffer, memory))
        return;
    VkImage image = g.images[g.imageIndex];
    imageBarrier(cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {g.extent.width, g.extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    imageBarrier(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

// Draws every mesh node of a model with the given pose.
void drawModel(int32_t id, const float* transform, int32_t anim, float time, int32_t animB, float timeB, float weight) {
    if (id < 0 || id >= static_cast<int32_t>(g.models.size()) || !g.frameActive) return;
    Model& model = *g.models[id];
    const ron::gltf::Model& m = model.data;
    std::vector<ron::gltf::Mat4> local, global;
    ron::gltf::samplePose(m, anim, time, animB, timeB, weight, local);
    ron::gltf::globalTransforms(m, local, global);
    ron::gltf::Mat4 user;
    std::memcpy(user.data(), transform, 64);

    for (size_t n = 0; n < m.nodes.size(); ++n) {
        const ron::gltf::Node& node = m.nodes[n];
        if (node.mesh < 0 || static_cast<size_t>(node.mesh) >= m.meshes.size()) continue;
        int32_t jointBase = -1;
        if (node.skin >= 0 && static_cast<size_t>(node.skin) < m.skins.size()) {
            // Skinned: vertices are moved by joint matrices (global joint * inverse bind).
            const ron::gltf::Skin& skin = m.skins[node.skin];
            std::vector<float> palette(skin.joints.size() * 16);
            for (size_t j = 0; j < skin.joints.size(); ++j) {
                ron::gltf::Mat4 jm = ron::gltf::multiply(global[skin.joints[j]], skin.inverseBind[j]);
                std::memcpy(&palette[j * 16], jm.data(), 64);
            }
            jointBase = pushJoints(palette.data(), static_cast<int32_t>(skin.joints.size()));
        }
        ron::gltf::Mat4 world = jointBase >= 0 ? user : ron::gltf::multiply(user, global[n]);
        const auto& prims = m.meshes[node.mesh].primitives;
        for (size_t p = 0; p < prims.size(); ++p) {
            int32_t mesh = model.primitiveMeshes[node.mesh][p];
            float color[4] = {1, 1, 1, 1};
            int32_t texture = 0;
            float roughness = 0.8f, metallic = 0.0f;
            if (prims[p].material >= 0 && static_cast<size_t>(prims[p].material) < m.materials.size()) {
                const auto& mat = m.materials[prims[p].material];
                std::memcpy(color, mat.baseColor, 16);
                roughness = mat.roughness;
                metallic = mat.metallic;
                if (mat.image >= 0 && static_cast<size_t>(mat.image) < model.imageTextures.size())
                    texture = model.imageTextures[mat.image];
            }
            bool skinned = jointBase >= 0 && prims[p].skinned;
            queueDraw(mesh, world.data(), texture, color[0], color[1], color[2], roughness, metallic, 0.0f,
                      skinned ? kFlagSkinned : 0, skinned ? jointBase : 0);
        }
    }
}

} // namespace

// ───────────────────────────── Public API ─────────────────────────────

RE_API bool engine_init(int32_t width, int32_t height, const char* title) {
    if (g.initialized) return true;
    QueryPerformanceFrequency(&g.freq);
    QueryPerformanceCounter(&g.start);
    g.lastFrame = g.start;
    if (!createWindow(width, height, title ? title : "Ron")) return false;
    if (!createInstance()) return false;

    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = g.instanceHandle;
    si.hwnd = g.hwnd;
    if (!check(vkCreateWin32SurfaceKHR(g.instance, &si, nullptr, &g.surface), "vkCreateWin32SurfaceKHR")) return false;

    if (!pickDevice() || !createDevice() || !createRenderPass() || !createLayout() || !createShadowResources() ||
        !createPipeline() || !createFrameResources())
        return false;
    createSwapchain();
    setDefaultScene();
    audio_init();
    if (std::getenv("RON_SCREENSHOT")) {
        const char* at = std::getenv("RON_SCREENSHOT_FRAME");
        g.screenshotAtFrame = at ? std::atoi(at) : 60;
    }
    g.initialized = true;
    return true;
}

RE_API void engine_screenshot(const char* path) {
    if (path) g.screenshotPath = path;
}

RE_API void engine_shutdown() {
    if (!g.initialized) return;
    audio_shutdown();
    vkDeviceWaitIdle(g.device);
    for (Mesh& m : g.meshes) {
        vkDestroyBuffer(g.device, m.vertexBuffer, nullptr);
        vkFreeMemory(g.device, m.vertexMemory, nullptr);
        vkDestroyBuffer(g.device, m.indexBuffer, nullptr);
        vkFreeMemory(g.device, m.indexMemory, nullptr);
    }
    g.meshes.clear();
    for (Texture& t : g.textures) destroyImage(t.image, t.memory, t.view);
    g.textures.clear();
    g.models.clear();
    destroySwapchain();
    for (int i = 0; i < kFramesInFlight; ++i) {
        vkDestroySemaphore(g.device, g.imageAvailable[i], nullptr);
        vkDestroyFence(g.device, g.inFlight[i], nullptr);
        vkDestroyBuffer(g.device, g.uniformBuffers[i], nullptr);
        vkFreeMemory(g.device, g.uniformMemory[i], nullptr);
        vkDestroyBuffer(g.device, g.jointBuffers[i], nullptr);
        vkFreeMemory(g.device, g.jointMemory[i], nullptr);
    }
    vkDestroyPipeline(g.device, g.shadowPipeline, nullptr);
    vkDestroyFramebuffer(g.device, g.shadowFramebuffer, nullptr);
    vkDestroyRenderPass(g.device, g.shadowPass, nullptr);
    vkDestroySampler(g.device, g.shadowSampler, nullptr);
    vkDestroySampler(g.device, g.textureSampler, nullptr);
    destroyImage(g.shadowImage, g.shadowMemory, g.shadowView);
    vkDestroyCommandPool(g.device, g.commandPool, nullptr);
    vkDestroyDescriptorPool(g.device, g.descriptorPool, nullptr);
    vkDestroyPipeline(g.device, g.pipeline, nullptr);
    vkDestroyPipelineLayout(g.device, g.pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(g.device, g.setLayout, nullptr);
    vkDestroyRenderPass(g.device, g.renderPass, nullptr);
    vkDestroyDevice(g.device, nullptr);
    vkDestroySurfaceKHR(g.instance, g.surface, nullptr);
    vkDestroyInstance(g.instance, nullptr);
    DestroyWindow(g.hwnd);
    g.initialized = false;
}

RE_API bool engine_running() {
    std::memcpy(g.prevKeys, g.keys, sizeof(g.keys));
    std::memcpy(g.prevMouse, g.mouse, sizeof(g.mouse));
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g.mouseDx = g.accDx;
    g.mouseDy = g.accDy;
    g.wheel = g.accWheel;
    g.accDx = g.accDy = g.accWheel = 0;
    if (g.keys[VK_ESCAPE]) g.quit = true;
    return !g.quit;
}

RE_API bool engine_begin_frame(float r, float gg, float b) {
    if (!g.initialized || g.frameActive) return false;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g.delta = std::min(0.1f, static_cast<float>(now.QuadPart - g.lastFrame.QuadPart) / static_cast<float>(g.freq.QuadPart));
    g.lastFrame = now;
    g.fpsTimer += g.delta;
    ++g.fpsFrames;
    if (g.fpsTimer >= 0.5f) {
        g.fps = static_cast<float>(g.fpsFrames) / g.fpsTimer;
        g.fpsTimer = 0;
        g.fpsFrames = 0;
        char buf[300];
        std::snprintf(buf, sizeof(buf), "%s  |  %.0f FPS", g.title.c_str(), g.fps);
        wchar_t wbuf[300];
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 300);
        SetWindowTextW(g.hwnd, wbuf);
    }

    if (g.width == 0 || g.height == 0) {
        Sleep(16);
        return false;
    }
    if (g.resized || !g.swapchain) {
        g.resized = false;
        if (!recreateSwapchain()) return false;
    }

    vkWaitForFences(g.device, 1, &g.inFlight[g.frame], VK_TRUE, UINT64_MAX);
    VkResult res = vkAcquireNextImageKHR(g.device, g.swapchain, UINT64_MAX, g.imageAvailable[g.frame], VK_NULL_HANDLE,
                                         &g.imageIndex);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        g.resized = true;
        return false;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return false;
    vkResetFences(g.device, 1, &g.inFlight[g.frame]);

    // The clear color goes through the same tone mapping as shaded pixels, so a
    // background equal to the fog color blends seamlessly into the horizon.
    g.clearColor[0] = aces(r);
    g.clearColor[1] = aces(gg);
    g.clearColor[2] = aces(b);
    g.draws.clear();
    g.jointCursor = 1;   // joint 0 stays the identity
    g.frameActive = true;
    return true;
}

RE_API void engine_end_frame() {
    if (!g.frameActive) return;
    g.frameActive = false;
    VkCommandBuffer cmd = g.commands[g.frame];
    mat4Mul(g.proj, g.view, g.scene.viewProj);
    computeLightViewProj(g.scene.lightViewProj);
    std::memcpy(g.uniformMapped[g.frame], &g.scene, sizeof(g.scene));
    vkResetCommandBuffer(cmd, 0);
    recordFrame(cmd);

    ++g.frameCounter;
    if (g.screenshotAtFrame >= 0 && g.frameCounter == static_cast<uint64_t>(g.screenshotAtFrame) &&
        g.screenshotPath.empty())
        g.screenshotPath = std::getenv("RON_SCREENSHOT");
    VkBuffer shotBuffer = VK_NULL_HANDLE;
    VkDeviceMemory shotMemory = VK_NULL_HANDLE;
    bool capture = !g.screenshotPath.empty() && g.canCopySwapchain;
    if (capture) captureFrame(cmd, shotBuffer, shotMemory);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g.imageAvailable[g.frame];
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g.renderFinished[g.imageIndex];
    vkQueueSubmit(g.queue, 1, &si, g.inFlight[g.frame]);

    if (capture && shotBuffer) {
        vkWaitForFences(g.device, 1, &g.inFlight[g.frame], VK_TRUE, UINT64_MAX);
        void* p = nullptr;
        vkMapMemory(g.device, shotMemory, 0, VK_WHOLE_SIZE, 0, &p);
        bool bgra = g.colorFormat == VK_FORMAT_B8G8R8A8_SRGB || g.colorFormat == VK_FORMAT_B8G8R8A8_UNORM;
        writeBmp(g.screenshotPath, static_cast<const uint8_t*>(p), g.extent.width, g.extent.height, bgra);
        vkUnmapMemory(g.device, shotMemory);
        vkDestroyBuffer(g.device, shotBuffer, nullptr);
        vkFreeMemory(g.device, shotMemory, nullptr);
        std::fprintf(stderr, "ron_engine: screenshot saved to %s\n", g.screenshotPath.c_str());
        g.screenshotPath.clear();
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g.renderFinished[g.imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.swapchain;
    pi.pImageIndices = &g.imageIndex;
    VkResult res = vkQueuePresentKHR(g.queue, &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) g.resized = true;
    g.frame = (g.frame + 1) % kFramesInFlight;
}

RE_API void engine_set_title(const char* title) { g.title = title ? title : ""; }
RE_API int32_t engine_width() { return g.width; }
RE_API int32_t engine_height() { return g.height; }
RE_API float engine_aspect() { return g.height > 0 ? static_cast<float>(g.width) / static_cast<float>(g.height) : 1.0f; }

RE_API double engine_time() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g.start.QuadPart) / static_cast<double>(g.freq.QuadPart);
}

RE_API float engine_delta() { return g.delta; }
RE_API float engine_fps() { return g.fps; }

RE_API void engine_set_camera(const float* view16, const float* proj16, const float* eye3) {
    std::memcpy(g.view, view16, sizeof(g.view));
    std::memcpy(g.proj, proj16, sizeof(g.proj));
    g.scene.cameraPos[0] = eye3[0];
    g.scene.cameraPos[1] = eye3[1];
    g.scene.cameraPos[2] = eye3[2];
}

RE_API void engine_set_light(float dx, float dy, float dz, float intensity) {
    float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 1e-6f) return;
    g.scene.lightDir[0] = dx / len;
    g.scene.lightDir[1] = dy / len;
    g.scene.lightDir[2] = dz / len;
    g.scene.lightDir[3] = intensity;
}

RE_API void engine_set_light_color(float r, float gg, float b) {
    g.scene.lightColor[0] = r;
    g.scene.lightColor[1] = gg;
    g.scene.lightColor[2] = b;
}

RE_API void engine_set_ambient(float sr, float sg, float sb, float gr, float gg, float gb) {
    g.scene.skyColor[0] = sr;
    g.scene.skyColor[1] = sg;
    g.scene.skyColor[2] = sb;
    g.scene.groundColor[0] = gr;
    g.scene.groundColor[1] = gg;
    g.scene.groundColor[2] = gb;
}

RE_API void engine_set_fog(float r, float gg, float b, float density) {
    g.scene.fog[0] = r;
    g.scene.fog[1] = gg;
    g.scene.fog[2] = b;
    g.scene.fog[3] = density;
}

RE_API void engine_set_material(float roughness, float metallic, float emissive, float worldUvScale) {
    g.material[0] = roughness;
    g.material[1] = metallic;
    g.material[2] = emissive;
    g.material[3] = worldUvScale;
}

// ───────────── Meshes ─────────────

RE_API int32_t engine_mesh_cube() {
    // 6 faces x 4 vertices, each face with its own normal and full 0..1 UVs.
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    for (int f = 0; f < 6; ++f) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float ux = ny, uy = nz, uz = nx;                                                   // perpendicular to n
        float vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;   // n x u
        auto base = static_cast<uint32_t>(v.size());
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto& c : corners) {
            v.push_back(makeVertex(0.5f * (nx + c[0] * ux + c[1] * vx), 0.5f * (ny + c[0] * uy + c[1] * vy),
                                   0.5f * (nz + c[0] * uz + c[1] * vz), nx, ny, nz, (c[0] + 1) * 0.5f,
                                   (c[1] + 1) * 0.5f));
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return uploadMesh(v, idx);
}

RE_API int32_t engine_mesh_sphere(int32_t rings, int32_t segments) {
    rings = std::max(3, rings);
    segments = std::max(3, segments);
    std::vector<Vertex> v;
    std::vector<uint32_t> idx;
    const float pi = 3.14159265358979f;
    for (int r = 0; r <= rings; ++r) {
        float phi = pi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s <= segments; ++s) {
            float theta = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            float x = std::sin(phi) * std::cos(theta), y = std::cos(phi), z = std::sin(phi) * std::sin(theta);
            v.push_back(makeVertex(0.5f * x, 0.5f * y, 0.5f * z, x, y, z,
                                   static_cast<float>(s) / static_cast<float>(segments),
                                   static_cast<float>(r) / static_cast<float>(rings)));
        }
    }
    auto stride = static_cast<uint32_t>(segments + 1);
    for (uint32_t r = 0; r < static_cast<uint32_t>(rings); ++r)
        for (uint32_t s = 0; s < static_cast<uint32_t>(segments); ++s) {
            uint32_t a = r * stride + s, b = a + stride;
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return uploadMesh(v, idx);
}

RE_API int32_t engine_mesh_plane(float size) {
    float h = size * 0.5f;
    float t = size * 0.25f;   // UVs repeat every 4 units
    std::vector<Vertex> v = {makeVertex(-h, 0, -h, 0, 1, 0, 0, 0), makeVertex(h, 0, -h, 0, 1, 0, t, 0),
                             makeVertex(h, 0, h, 0, 1, 0, t, t), makeVertex(-h, 0, h, 0, 1, 0, 0, t)};
    std::vector<uint32_t> idx = {0, 2, 1, 0, 3, 2};
    return uploadMesh(v, idx);
}

// vertices: 8 floats each (position, normal, uv)
RE_API int32_t engine_mesh_create(const float* vertices, int32_t vertexCount, const uint32_t* indices, int32_t indexCount) {
    if (!vertices || !indices || vertexCount <= 0 || indexCount <= 0) return -1;
    std::vector<Vertex> v;
    for (int32_t i = 0; i < vertexCount; ++i) {
        const float* s = vertices + i * 8;
        v.push_back(makeVertex(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]));
    }
    std::vector<uint32_t> idx(indices, indices + indexCount);
    for (uint32_t& i : idx)
        if (i >= static_cast<uint32_t>(vertexCount)) i = 0;
    return uploadMesh(v, idx);
}

// vertices: 16 floats each (position, normal, uv, 4 joint indices, 4 weights)
RE_API int32_t engine_mesh_create_skinned(const float* vertices, int32_t vertexCount, const uint32_t* indices,
                                          int32_t indexCount) {
    if (!vertices || !indices || vertexCount <= 0 || indexCount <= 0) return -1;
    std::vector<Vertex> v;
    for (int32_t i = 0; i < vertexCount; ++i) {
        const float* s = vertices + i * 16;
        Vertex vx = makeVertex(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
        for (int k = 0; k < 4; ++k) {
            vx.joints[k] = static_cast<uint8_t>(std::clamp(s[8 + k], 0.0f, 255.0f));
            vx.weights[k] = s[12 + k];
        }
        v.push_back(vx);
    }
    std::vector<uint32_t> idx(indices, indices + indexCount);
    for (uint32_t& i : idx)
        if (i >= static_cast<uint32_t>(vertexCount)) i = 0;
    return uploadMesh(v, idx);
}

// ───────────── Textures ─────────────

RE_API int32_t engine_texture_create(int32_t width, int32_t height, const uint8_t* rgba) {
    if (!rgba || width <= 0 || height <= 0) return -1;
    return createTexture(static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgba);
}

RE_API int32_t engine_texture_load(const char* path) {
    ron::png::Image img;
    std::string error;
    if (!path || !ron::png::load(path, img, error)) {
        std::fprintf(stderr, "ron_engine: texture '%s': %s\n", path ? path : "(null)", error.c_str());
        return -1;
    }
    return createTexture(img.width, img.height, img.rgba.data());
}

// ───────────── Drawing ─────────────

RE_API void engine_draw(int32_t mesh, const float* model16, float r, float gg, float b) {
    queueDraw(mesh, model16, 0, r, gg, b, g.material[0], g.material[1], g.material[2], 0, 0, g.material[3]);
}

RE_API void engine_draw_checker(int32_t mesh, const float* model16, float r, float gg, float b) {
    queueDraw(mesh, model16, 0, r, gg, b, g.material[0], g.material[1], g.material[2], kFlagChecker, 0, g.material[3]);
}

RE_API void engine_draw_textured(int32_t mesh, const float* model16, int32_t texture, float r, float gg, float b) {
    queueDraw(mesh, model16, texture, r, gg, b, g.material[0], g.material[1], g.material[2], 0, 0, g.material[3]);
}

RE_API void engine_draw_skinned(int32_t mesh, const float* model16, const float* joints16, int32_t jointCount,
                                int32_t texture, float r, float gg, float b) {
    if (!g.frameActive) return;   // the joint buffer of an inactive frame may still be read by the GPU
    int32_t base = pushJoints(joints16, jointCount);
    if (base < 0) return;
    queueDraw(mesh, model16, texture, r, gg, b, g.material[0], g.material[1], g.material[2], kFlagSkinned, base, g.material[3]);
}

// ───────────── Models (glTF) ─────────────

RE_API int32_t engine_model_load(const char* path) {
    if (!g.initialized || !path) return -1;
    auto model = std::make_unique<Model>();
    std::string error;
    if (!ron::gltf::load(path, model->data, error)) {
        std::fprintf(stderr, "ron_engine: model '%s': %s\n", path, error.c_str());
        return -1;
    }
    for (const auto& img : model->data.images) model->imageTextures.push_back(createTexture(img.width, img.height, img.rgba.data()));
    for (const auto& mesh : model->data.meshes) {
        std::vector<int32_t> ids;
        for (const auto& prim : mesh.primitives) ids.push_back(uploadMesh(prim.vertices, prim.indices));
        model->primitiveMeshes.push_back(std::move(ids));
    }
    g.models.push_back(std::move(model));
    return static_cast<int32_t>(g.models.size() - 1);
}

RE_API int32_t engine_model_animation_count(int32_t model) {
    if (model < 0 || model >= static_cast<int32_t>(g.models.size())) return 0;
    return static_cast<int32_t>(g.models[model]->data.animations.size());
}

RE_API float engine_model_animation_duration(int32_t model, int32_t anim) {
    if (model < 0 || model >= static_cast<int32_t>(g.models.size())) return 0;
    const auto& anims = g.models[model]->data.animations;
    return anim >= 0 && anim < static_cast<int32_t>(anims.size()) ? anims[anim].duration : 0.0f;
}

RE_API int32_t engine_model_find_animation(int32_t model, const char* name) {
    if (model < 0 || model >= static_cast<int32_t>(g.models.size()) || !name) return -1;
    const auto& anims = g.models[model]->data.animations;
    for (size_t i = 0; i < anims.size(); ++i)
        if (anims[i].name == name) return static_cast<int32_t>(i);
    return -1;
}

RE_API void engine_draw_model(int32_t model, const float* transform16, int32_t anim, float time) {
    drawModel(model, transform16, anim, time, -1, 0.0f, 0.0f);
}

RE_API void engine_draw_model_blend(int32_t model, const float* transform16, int32_t animA, float timeA, int32_t animB,
                                    float timeB, float weight) {
    drawModel(model, transform16, animA, timeA, animB, timeB, weight);
}

// ───────────── Input ─────────────

RE_API bool engine_key_down(int32_t key) { return key >= 0 && key < 256 && g.keys[key]; }
RE_API bool engine_key_pressed(int32_t key) { return key >= 0 && key < 256 && g.keys[key] && !g.prevKeys[key]; }
RE_API bool engine_mouse_down(int32_t button) { return button >= 0 && button < 3 && g.mouse[button]; }
RE_API bool engine_mouse_pressed(int32_t button) {
    return button >= 0 && button < 3 && g.mouse[button] && !g.prevMouse[button];
}
RE_API float engine_mouse_dx() { return g.mouseDx; }
RE_API float engine_mouse_dy() { return g.mouseDy; }
RE_API float engine_mouse_wheel() { return g.wheel; }
