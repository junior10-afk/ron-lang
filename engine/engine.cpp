// ron_engine: window, input and a forward Vulkan renderer (MSAA, depth,
// hemispheric ambient + directional light + specular, fog, ACES tone mapping).

#include "ron_engine.h"

#include <windows.h>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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
constexpr float kShadowExtent = 18.0f;   // half-size of the light's orthographic box

struct Mesh {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    uint32_t indexCount = 0;
};

struct SceneUniforms {
    float lightViewProj[16];
    float lightDir[4];
    float lightColor[4];
    float skyColor[4];
    float groundColor[4];
    float cameraPos[4];
    float fog[4];
};

struct PushConstants {
    float mvp[16];
    float model0[4];
    float model1[4];
    float model2[4];
    float color[4];
};
static_assert(sizeof(PushConstants) == 128, "push constants must fit the guaranteed 128 bytes");

// Draws are queued during the frame and recorded at the end, so the shadow pass
// can render every object before the main pass.
struct DrawCommand {
    int32_t mesh;
    float model[16];
    float color[4];
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

    // Pipeline
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
    VkPipelineLayout shadowLayout = VK_NULL_HANDLE;
    VkPipeline shadowPipeline = VK_NULL_HANDLE;
    std::vector<DrawCommand> draws;
    float clearColor[3] = {};

    // Per frame in flight
    VkCommandPool commandPool = VK_NULL_HANDLE;
    VkCommandBuffer commands[kFramesInFlight] = {};
    VkSemaphore imageAvailable[kFramesInFlight] = {};
    VkFence inFlight[kFramesInFlight] = {};
    VkBuffer uniformBuffers[kFramesInFlight] = {};
    VkDeviceMemory uniformMemory[kFramesInFlight] = {};
    void* uniformMapped[kFramesInFlight] = {};
    VkDescriptorSet descriptorSets[kFramesInFlight] = {};

    uint32_t frame = 0;
    uint32_t imageIndex = 0;
    bool frameActive = false;
    bool initialized = false;

    // Screenshot request (written after the frame completes)
    std::string screenshotPath;
    int screenshotAtFrame = -1;   // from RON_SCREENSHOT_FRAME
    uint64_t frameCounter = 0;
    bool canCopySwapchain = false;

    std::vector<Mesh> meshes;
    float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float proj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float viewProj[16] = {};
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

bool createImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                 VkImageAspectFlags aspect, VkImage& image, VkDeviceMemory& memory, VkImageView& view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
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
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
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

// ───────────────────────────── Vulkan setup ─────────────────────────────

bool createInstance() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = g.title.c_str();
    app.pEngineName = "Ron Engine";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
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
        if (!hasSwapchain) continue;

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
    const char* exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1;
    di.ppEnabledExtensionNames = exts;
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

VkShaderModule shaderModule(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(g.device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

bool createPipeline() {
    VkDescriptorSetLayoutBinding bindings[2] = {};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 2;
    sl.pBindings = bindings;
    if (!check(vkCreateDescriptorSetLayout(g.device, &sl, nullptr, &g.setLayout), "vkCreateDescriptorSetLayout"))
        return false;

    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g.setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!check(vkCreatePipelineLayout(g.device, &pl, nullptr, &g.pipelineLayout), "vkCreatePipelineLayout"))
        return false;

    VkShaderModule vert = shaderModule(kVertSpv, sizeof(kVertSpv));
    VkShaderModule frag = shaderModule(kFragSpv, sizeof(kFragSpv));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
    };

    VkVertexInputBindingDescription vb{0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)},
    };
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = attrs;

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
    if (!createImage(kShadowMapSize, kShadowMapSize, VK_FORMAT_D32_SFLOAT,
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

    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!check(vkCreatePipelineLayout(g.device, &pl, nullptr, &g.shadowLayout), "vkCreatePipelineLayout (shadow)"))
        return false;

    VkShaderModule vert = shaderModule(kShadowVertSpv, sizeof(kShadowVertSpv));
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                                          VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    VkVertexInputBindingDescription vb{0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attr{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 1;
    vi.pVertexAttributeDescriptions = &attr;
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
    pi.layout = g.shadowLayout;
    pi.renderPass = g.shadowPass;
    bool ok = check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pi, nullptr, &g.shadowPipeline),
                    "vkCreateGraphicsPipelines (shadow)");
    vkDestroyShaderModule(g.device, vert, nullptr);
    return ok;
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

    VkDescriptorPoolSize poolSizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                         {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kFramesInFlight}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kFramesInFlight;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = poolSizes;
    if (!check(vkCreateDescriptorPool(g.device, &dp, nullptr, &g.descriptorPool), "vkCreateDescriptorPool"))
        return false;

    VkDescriptorSetLayout layouts[kFramesInFlight] = {g.setLayout, g.setLayout};
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = g.descriptorPool;
    da.descriptorSetCount = kFramesInFlight;
    da.pSetLayouts = layouts;
    if (!check(vkAllocateDescriptorSets(g.device, &da, g.descriptorSets), "vkAllocateDescriptorSets")) return false;

    for (int i = 0; i < kFramesInFlight; ++i) {
        if (!createBuffer(sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          g.uniformBuffers[i], g.uniformMemory[i]))
            return false;
        vkMapMemory(g.device, g.uniformMemory[i], 0, sizeof(SceneUniforms), 0, &g.uniformMapped[i]);
        VkDescriptorBufferInfo bi{g.uniformBuffers[i], 0, sizeof(SceneUniforms)};
        VkDescriptorImageInfo ii{g.shadowSampler, g.shadowView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
        w[0].dstSet = g.descriptorSets[i];
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &bi;
        w[1].dstSet = g.descriptorSets[i];
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &ii;
        vkUpdateDescriptorSets(g.device, 2, w, 0, nullptr);

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (!check(vkCreateSemaphore(g.device, &si, nullptr, &g.imageAvailable[i]), "vkCreateSemaphore") ||
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
    if (msaa && !createImage(extent.width, extent.height, g.colorFormat,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, g.samples,
                             VK_IMAGE_ASPECT_COLOR_BIT, g.msaaImage, g.msaaMemory, g.msaaView))
        return false;
    if (!createImage(extent.width, extent.height, g.depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, g.samples,
                     VK_IMAGE_ASPECT_DEPTH_BIT, g.depthImage, g.depthMemory, g.depthView))
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

int32_t uploadMesh(const std::vector<float>& vertices, const std::vector<uint32_t>& indices) {
    Mesh m;
    VkDeviceSize vbytes = vertices.size() * sizeof(float);
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

void setDefaultScene() {
    float lx = -0.45f, ly = -1.0f, lz = -0.3f;
    float len = std::sqrt(lx * lx + ly * ly + lz * lz);
    g.scene = SceneUniforms{
        {},
        {lx / len, ly / len, lz / len, 1.6f},
        {1.0f, 0.93f, 0.8f, 1.0f},
        {0.22f, 0.28f, 0.38f, 1.0f},
        {0.1f, 0.08f, 0.06f, 1.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {0.45f, 0.55f, 0.7f, 0.018f},
    };
}

void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                  VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
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

// Records a copy of the current swapchain image into a host buffer, then saves it.
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

void queueDraw(int32_t mesh, const float* model, float r, float g2, float b, float flags) {
    if (!g.frameActive || mesh < 0 || mesh >= static_cast<int32_t>(g.meshes.size())) return;
    DrawCommand d;
    d.mesh = mesh;
    std::memcpy(d.model, model, sizeof(d.model));
    d.color[0] = r;
    d.color[1] = g2;
    d.color[2] = b;
    d.color[3] = flags;
    g.draws.push_back(d);
}

void bindMesh(VkCommandBuffer cmd, const Mesh& m) {
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m.vertexBuffer, &offset);
    vkCmdBindIndexBuffer(cmd, m.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
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
    // Light basis: forward = L, right = normalize(L x up), up = right x L
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
    // Ortho: x,y in [-E, E] -> [-1, 1]; z (along L) in [-depth, depth] -> [0, 1]
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

    // 1) Shadow map
    float lightVP[16];
    std::memcpy(lightVP, g.scene.lightViewProj, sizeof(lightVP));
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
    for (const DrawCommand& d : g.draws) {
        if (d.color[3] > 0.5f) continue;   // ground planes do not cast shadows
        float mvp[16];
        mat4Mul(lightVP, d.model, mvp);
        const Mesh& m = g.meshes[d.mesh];
        bindMesh(cmd, m);
        vkCmdPushConstants(cmd, g.shadowLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
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
        mat4Mul(g.viewProj, d.model, pc.mvp);
        for (int row = 0; row < 3; ++row) {
            float* dst = row == 0 ? pc.model0 : row == 1 ? pc.model1 : pc.model2;
            for (int col = 0; col < 4; ++col) dst[col] = d.model[col * 4 + row];
        }
        std::memcpy(pc.color, d.color, sizeof(pc.color));
        const Mesh& m = g.meshes[d.mesh];
        bindMesh(cmd, m);
        vkCmdPushConstants(cmd, g.pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);
        vkCmdDrawIndexed(cmd, m.indexCount, 1, 0, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
}

constexpr float kExposure = 0.6f;   // must match EXPOSURE in mesh.frag
float aces(float x) {
    x *= kExposure;
    return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f); }

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

    if (!pickDevice() || !createDevice() || !createRenderPass() || !createShadowResources() || !createPipeline() ||
        !createFrameResources())
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
    destroySwapchain();
    for (int i = 0; i < kFramesInFlight; ++i) {
        vkDestroySemaphore(g.device, g.imageAvailable[i], nullptr);
        vkDestroyFence(g.device, g.inFlight[i], nullptr);
        vkDestroyBuffer(g.device, g.uniformBuffers[i], nullptr);
        vkFreeMemory(g.device, g.uniformMemory[i], nullptr);
    }
    vkDestroyPipeline(g.device, g.shadowPipeline, nullptr);
    vkDestroyPipelineLayout(g.device, g.shadowLayout, nullptr);
    vkDestroyFramebuffer(g.device, g.shadowFramebuffer, nullptr);
    vkDestroyRenderPass(g.device, g.shadowPass, nullptr);
    vkDestroySampler(g.device, g.shadowSampler, nullptr);
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
    g.frameActive = true;
    return true;
}

RE_API void engine_end_frame() {
    if (!g.frameActive) return;
    g.frameActive = false;
    VkCommandBuffer cmd = g.commands[g.frame];
    mat4Mul(g.proj, g.view, g.viewProj);
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

RE_API void engine_set_title(const char* title) {
    g.title = title ? title : "";
}

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

RE_API int32_t engine_mesh_cube() {
    // 6 faces x 4 vertices, each face with its own normal.
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<float> v;
    std::vector<uint32_t> idx;
    for (int f = 0; f < 6; ++f) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        // Two axes spanning the face
        float ux = ny, uy = nz, uz = nx;    // a vector perpendicular to n
        float vx = ny * uz - nz * uy, vy = nz * ux - nx * uz, vz = nx * uy - ny * ux;   // n x u
        uint32_t base = static_cast<uint32_t>(v.size() / 6);
        const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (auto& c : corners) {
            float px = 0.5f * (nx + c[0] * ux + c[1] * vx);
            float py = 0.5f * (ny + c[0] * uy + c[1] * vy);
            float pz = 0.5f * (nz + c[0] * uz + c[1] * vz);
            v.insert(v.end(), {px, py, pz, nx, ny, nz});
        }
        idx.insert(idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    return uploadMesh(v, idx);
}

RE_API int32_t engine_mesh_sphere(int32_t rings, int32_t segments) {
    rings = std::max(3, rings);
    segments = std::max(3, segments);
    std::vector<float> v;
    std::vector<uint32_t> idx;
    const float pi = 3.14159265358979f;
    for (int r = 0; r <= rings; ++r) {
        float phi = pi * static_cast<float>(r) / static_cast<float>(rings);
        for (int s = 0; s <= segments; ++s) {
            float theta = 2.0f * pi * static_cast<float>(s) / static_cast<float>(segments);
            float x = std::sin(phi) * std::cos(theta);
            float y = std::cos(phi);
            float z = std::sin(phi) * std::sin(theta);
            v.insert(v.end(), {0.5f * x, 0.5f * y, 0.5f * z, x, y, z});
        }
    }
    uint32_t stride = static_cast<uint32_t>(segments + 1);
    for (uint32_t r = 0; r < static_cast<uint32_t>(rings); ++r)
        for (uint32_t s = 0; s < static_cast<uint32_t>(segments); ++s) {
            uint32_t a = r * stride + s, b = a + stride;
            idx.insert(idx.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    return uploadMesh(v, idx);
}

RE_API int32_t engine_mesh_plane(float size) {
    float h = size * 0.5f;
    std::vector<float> v = {-h, 0, -h, 0, 1, 0, h, 0, -h, 0, 1, 0, h, 0, h, 0, 1, 0, -h, 0, h, 0, 1, 0};
    std::vector<uint32_t> idx = {0, 2, 1, 0, 3, 2};
    return uploadMesh(v, idx);
}

RE_API int32_t engine_mesh_create(const float* vertices, int32_t vertexCount, const uint32_t* indices, int32_t indexCount) {
    if (!vertices || !indices || vertexCount <= 0 || indexCount <= 0) return -1;
    std::vector<float> v(vertices, vertices + vertexCount * 6);
    std::vector<uint32_t> idx(indices, indices + indexCount);
    return uploadMesh(v, idx);
}

RE_API void engine_draw(int32_t mesh, const float* model16, float r, float gg, float b) {
    queueDraw(mesh, model16, r, gg, b, 0.0f);
}

RE_API void engine_draw_checker(int32_t mesh, const float* model16, float r, float gg, float b) {
    queueDraw(mesh, model16, r, gg, b, 1.0f);
}

RE_API bool engine_key_down(int32_t key) { return key >= 0 && key < 256 && g.keys[key]; }
RE_API bool engine_key_pressed(int32_t key) { return key >= 0 && key < 256 && g.keys[key] && !g.prevKeys[key]; }
RE_API bool engine_mouse_down(int32_t button) { return button >= 0 && button < 3 && g.mouse[button]; }
RE_API float engine_mouse_dx() { return g.mouseDx; }
RE_API float engine_mouse_dy() { return g.mouseDy; }
RE_API float engine_mouse_wheel() { return g.wheel; }
