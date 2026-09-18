#include "shader_contract.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <faset/core/io.hpp>
#include <faset/render/render_graph.hpp>
#include <faset/render/renderer.hpp>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vulkan/vulkan.h>

namespace faset::render {
namespace {
void check(VkResult result, const char* action) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(action) + " failed (Vulkan " + std::to_string(result) +
                                 ")");
}
struct GpuVertex {
    float clip[4], world[3], normal[3], color[4], material[2], uv[2];
};
struct Push {
    Mat4 light_view_projection;
    std::array<float, 4> light_direction, eye;
};
static_assert(sizeof(Push) == 96, "Slang FrameParameters layout");
std::array<float, 4> point(const Mat4& m, std::array<float, 4> p) {
    std::array<float, 4> o{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            o[r] += m[c * 4 + r] * p[c];
    return o;
}
struct Buffer {
    VkBuffer handle{};
    VkDeviceMemory memory{};
    VkDeviceSize size{}, allocation_size{};
};
struct Image {
    VkImage handle{};
    VkDeviceMemory memory{};
    VkImageView view{};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkDeviceSize allocation_size{};
};
struct Batch {
    std::uint32_t first{}, count{};
    const Texture* texture{};
    std::array<float, 4> clip_rect{};
};
constexpr std::uint32_t shadow_size = 1024;
} // namespace
struct Renderer::Impl {
    RendererConfig config;
    SDL_Window* window{};
    std::string offscreen_clipboard;
    bool sdl{}, close{}, dirty_swapchain{};
    std::uint32_t width{}, height{};
    VkInstance instance{};
    VkDebugUtilsMessengerEXT messenger{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    VkQueue queue{};
    std::uint32_t queue_family{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkQueryPool timestamp_pool{};
    float timestamp_period{};
    std::uint32_t timestamp_bits{};
    VkSemaphore acquired{}, present_ready{};
    std::array<std::string, 3> shader_layouts{};
    VkSwapchainKHR swapchain{};
    VkFormat swap_format{};
    VkExtent2D swap_extent{};
    std::vector<VkImage> swap_images;
    std::vector<VkImageLayout> swap_layouts;
    Image color, depth, shadow;
    Buffer vertices, readback;
    VkDescriptorSetLayout descriptor_layout{};
    VkDescriptorPool descriptor_pool{};
    VkSampler shadow_sampler{}, color_sampler{};
    VkPipelineLayout pipeline_layout{};
    VkPipeline pipeline{}, ui_pipeline{}, shadow_pipeline{}, sprite_pipeline{};
    PFN_vkCmdBeginDebugUtilsLabelEXT begin_gpu_label{};
    PFN_vkCmdEndDebugUtilsLabelEXT end_gpu_label{};
    struct GpuTexture {
        Image image;
        VkDescriptorSet descriptor{};
        std::shared_ptr<const Texture> source;
        std::uint64_t revision{};
    };
    std::unordered_map<const Texture*, GpuTexture> textures;
    std::shared_ptr<Texture> white;
    std::vector<std::uint8_t> last_pixels;
    FrameStats statistics;
    std::atomic<std::uint32_t> validation_errors{};
    ~Impl() {
        cleanup();
    }
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                VkDebugUtilsMessageTypeFlagsEXT,
                                                const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                void* user) {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            static_cast<Impl*>(user)->validation_errors.fetch_add(1);
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            std::cerr << "[Vulkan] " << data->pMessage << '\n';
        return VK_FALSE;
    }
    void destroy(Buffer& b) {
        if (device) {
            if (b.handle)
                vkDestroyBuffer(device, b.handle, nullptr);
            if (b.memory)
                vkFreeMemory(device, b.memory, nullptr);
        }
        b = {};
    }
    void destroy(Image& i) {
        if (device) {
            if (i.view)
                vkDestroyImageView(device, i.view, nullptr);
            if (i.handle)
                vkDestroyImage(device, i.handle, nullptr);
            if (i.memory)
                vkFreeMemory(device, i.memory, nullptr);
        }
        i = {};
    }
    void cleanup() {
        if (device)
            vkDeviceWaitIdle(device);
        for (auto& [_, texture] : textures)
            destroy(texture.image);
        destroy(vertices);
        destroy(readback);
        destroy(color);
        destroy(depth);
        destroy(shadow);
        if (device) {
            if (pipeline)
                vkDestroyPipeline(device, pipeline, nullptr);
            if (ui_pipeline)
                vkDestroyPipeline(device, ui_pipeline, nullptr);
            if (shadow_pipeline)
                vkDestroyPipeline(device, shadow_pipeline, nullptr);
            if (sprite_pipeline)
                vkDestroyPipeline(device, sprite_pipeline, nullptr);
            if (pipeline_layout)
                vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
            if (descriptor_pool)
                vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
            if (descriptor_layout)
                vkDestroyDescriptorSetLayout(device, descriptor_layout, nullptr);
            if (shadow_sampler)
                vkDestroySampler(device, shadow_sampler, nullptr);
            if (color_sampler)
                vkDestroySampler(device, color_sampler, nullptr);
            if (swapchain)
                vkDestroySwapchainKHR(device, swapchain, nullptr);
            if (timestamp_pool)
                vkDestroyQueryPool(device, timestamp_pool, nullptr);
            if (acquired)
                vkDestroySemaphore(device, acquired, nullptr);
            if (present_ready)
                vkDestroySemaphore(device, present_ready, nullptr);
            if (fence)
                vkDestroyFence(device, fence, nullptr);
            if (pool)
                vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface)
            vkDestroySurfaceKHR(instance, surface, nullptr);
        if (messenger) {
            auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
            if (fn)
                fn(instance, messenger, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
        if (window)
            SDL_DestroyWindow(window);
        if (sdl)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags properties,
                              VkMemoryPropertyFlags preferred = 0) {
        VkPhysicalDeviceMemoryProperties p{};
        vkGetPhysicalDeviceMemoryProperties(physical, &p);
        if (preferred)
            for (std::uint32_t i = 0; i < p.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags &
                                           (properties | preferred)) == (properties | preferred))
                    return i;
        for (std::uint32_t i = 0; i < p.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        throw std::runtime_error("Required Vulkan memory type is unavailable");
    }
    Buffer make_buffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                       VkMemoryPropertyFlags properties, VkMemoryPropertyFlags preferred = 0) {
        Buffer b{};
        b.size = bytes;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = bytes;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &b.handle), "Create buffer");
        try {
            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, b.handle, &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = memory_type(req.memoryTypeBits, properties, preferred);
            check(vkAllocateMemory(device, &alloc, nullptr, &b.memory), "Allocate buffer memory");
            b.allocation_size = req.size;
            check(vkBindBufferMemory(device, b.handle, b.memory, 0), "Bind buffer memory");
        } catch (...) {
            destroy(b);
            throw;
        }
        return b;
    }
    Image make_image(std::uint32_t w, std::uint32_t h, VkFormat format, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect) {
        Image image{};
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device, &info, nullptr, &image.handle), "Create image");
        try {
            VkMemoryRequirements req{};
            vkGetImageMemoryRequirements(device, image.handle, &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex =
                memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(device, &alloc, nullptr, &image.memory),
                  "Allocate image memory");
            image.allocation_size = req.size;
            check(vkBindImageMemory(device, image.handle, image.memory, 0), "Bind image memory");
            VkImageViewCreateInfo view{};
            view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            view.image = image.handle;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = {aspect, 0, 1, 0, 1};
            check(vkCreateImageView(device, &view, nullptr, &image.view), "Create image view");
        } catch (...) {
            destroy(image);
            throw;
        }
        return image;
    }
    void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout& before, VkImageLayout after,
                    VkImageAspectFlags aspect) {
        // Conservative dependencies make the first single-queue backend auditable.
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = before == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? VK_PIPELINE_STAGE_2_NONE
                                   : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.srcAccessMask =
            before == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        before = after;
    }
    void transition(VkCommandBuffer cmd, Image& image, VkImageLayout after,
                    VkImageAspectFlags aspect) {
        transition(cmd, image.handle, image.layout, after, aspect);
    }
    void begin() {
        check(vkResetCommandBuffer(command, 0), "Reset command buffer");
        VkCommandBufferBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &info), "Begin command buffer");
    }
    void submit(bool present = false) {
        check(vkEndCommandBuffer(command), "End command buffer");
        check(vkResetFences(device, 1, &fence), "Reset fence");
        VkCommandBufferSubmitInfo cmd{};
        cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cmd.commandBuffer = command;
        VkSubmitInfo2 info{};
        info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        info.commandBufferInfoCount = 1;
        info.pCommandBufferInfos = &cmd;
        VkSemaphoreSubmitInfo wait{}, signal{};
        wait.sType = signal.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        if (present) {
            wait.semaphore = acquired;
            wait.stageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            signal.semaphore = present_ready;
            signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            info.waitSemaphoreInfoCount = 1;
            info.pWaitSemaphoreInfos = &wait;
            info.signalSemaphoreInfoCount = 1;
            info.pSignalSemaphoreInfos = &signal;
        }
        check(vkQueueSubmit2(queue, 1, &info, fence), "Submit frame");
        check(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX), "Wait frame fence");
    }
    void initialize(const RendererConfig& c) {
        config = c;
        width = c.width;
        height = c.height;
        if (!width || !height)
            throw std::invalid_argument("Renderer dimensions must be nonzero");
        std::vector<const char*> extensions;
        if (!c.headless) {
            if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
                throw std::runtime_error(SDL_GetError());
            sdl = true;
            window = SDL_CreateWindow(
                c.title.c_str(), static_cast<int>(width), static_cast<int>(height),
                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
            if (!window)
                throw std::runtime_error(SDL_GetError());
            Uint32 count{};
            auto names = SDL_Vulkan_GetInstanceExtensions(&count);
            if (!names)
                throw std::runtime_error(SDL_GetError());
            extensions.assign(names, names + count);
            SDL_StartTextInput(window);
        }
        std::uint32_t count{};
        check(vkEnumerateInstanceLayerProperties(&count, nullptr), "Enumerate layers");
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "Enumerate layers");
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr),
              "Enumerate instance extensions");
        std::vector<VkExtensionProperties> instance_extensions(count);
        check(vkEnumerateInstanceExtensionProperties(nullptr, &count, instance_extensions.data()),
              "Enumerate instance extensions");
        const bool debug_utils =
            std::any_of(instance_extensions.begin(), instance_extensions.end(), [](const auto& p) {
                return std::strcmp(p.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
            });
        bool validation =
            c.validation && debug_utils && std::any_of(layers.begin(), layers.end(), [](auto& p) {
                return std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
            });
        statistics.validation_enabled = validation;
        if (c.validation && !validation)
            std::cerr << "[Faset] Vulkan validation layer/debug-utils unavailable; validation "
                         "disabled.\n";
        if (debug_utils)
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "Faset Engine";
        app.apiVersion = VK_API_VERSION_1_3;
        VkDebugUtilsMessengerCreateInfoEXT debug_info{};
        debug_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
        debug_info.pfnUserCallback = debug;
        debug_info.pUserData = this;
        const char* validation_name = "VK_LAYER_KHRONOS_validation";
        VkInstanceCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        if (validation) {
            info.enabledLayerCount = 1;
            info.ppEnabledLayerNames = &validation_name;
            info.pNext = &debug_info;
        }
        check(vkCreateInstance(&info, nullptr, &instance), "Create Vulkan instance");
        if (validation) {
            auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
            if (fn)
                check(fn(instance, &debug_info, nullptr, &messenger),
                      "Create validation messenger");
        }
        if (window && !SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
            throw std::runtime_error(SDL_GetError());
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "Enumerate GPUs");
        int best = -1;
        for (auto gpu : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(gpu, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3)
                continue;
            VkPhysicalDeviceVulkan13Features f13{};
            f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(gpu, &features);
            if (!f13.synchronization2 || !f13.dynamicRendering)
                continue;
            VkFormatProperties color_props{}, depth_props{};
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R8G8B8A8_UNORM, &color_props);
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D32_SFLOAT, &depth_props);
            if (!(color_props.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) ||
                !(depth_props.optimalTilingFeatures &
                  VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
                !(depth_props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
                continue;
            std::uint32_t n{};
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
            std::vector<VkQueueFamilyProperties> queues(n);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, queues.data());
            for (std::uint32_t i = 0; i < n; ++i) {
                VkBool32 supports = VK_TRUE;
                if (surface)
                    check(vkGetPhysicalDeviceSurfaceSupportKHR(gpu, i, surface, &supports),
                          "Query present support");
                int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? 3
                            : properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
                                                                                              : 1;
                if (supports && (queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && score > best) {
                    best = score;
                    physical = gpu;
                    queue_family = i;
                    statistics.device = properties.deviceName;
                    timestamp_period = properties.limits.timestampPeriod;
                    timestamp_bits = queues[i].timestampValidBits;
                }
            }
        }
        if (!physical)
            throw std::runtime_error("No Vulkan 1.3 device supports dynamic rendering, "
                                     "synchronization2 and required color/depth formats");
        float priority = 1;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = queue_family;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f13.synchronization2 = VK_TRUE;
        f13.dynamicRendering = VK_TRUE;
        VkDeviceCreateInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        di.pNext = &f13;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        const char* swap_extension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        if (surface) {
            di.enabledExtensionCount = 1;
            di.ppEnabledExtensionNames = &swap_extension;
        }
        check(vkCreateDevice(physical, &di, nullptr, &device), "Create Vulkan device");
        vkGetDeviceQueue(device, queue_family, 0, &queue);
        if (debug_utils) {
            begin_gpu_label = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(device, "vkCmdBeginDebugUtilsLabelEXT"));
            end_gpu_label = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
                vkGetDeviceProcAddr(device, "vkCmdEndDebugUtilsLabelEXT"));
        }
        statistics.gpu_labels_enabled = begin_gpu_label && end_gpu_label;
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = queue_family;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pi, nullptr, &pool), "Create command pool");
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &ai, &command), "Allocate command buffer");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device, &fi, nullptr, &fence), "Create frame fence");
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        check(vkCreateSemaphore(device, &si, nullptr, &acquired), "Create acquire semaphore");
        check(vkCreateSemaphore(device, &si, nullptr, &present_ready), "Create present semaphore");
        if (timestamp_bits) {
            VkQueryPoolCreateInfo query{};
            query.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            check(vkCreateQueryPool(device, &query, nullptr, &timestamp_pool),
                  "Create GPU timestamp queries");
        }
        shadow =
            make_image(shadow_size, shadow_size, VK_FORMAT_D32_SFLOAT,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
        make_targets();
        make_descriptors();
        make_pipelines();
        white = std::make_shared<Texture>();
        white->width = white->height = 1;
        white->rgba = {255, 255, 255, 255};
        upload_texture(white);
        if (surface)
            make_swapchain();
    }
    void make_targets() {
        check(vkDeviceWaitIdle(device), "Wait resize");
        destroy(color);
        destroy(depth);
        destroy(readback);
        color = make_image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
        depth = make_image(width, height, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        // CPU reads this allocation every frame. Prefer cached coherent memory when available.
        readback =
            make_buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        last_pixels.clear();
    }
    void make_swapchain() {
        if (!surface)
            return;
        int w{}, h{};
        SDL_GetWindowSizeInPixels(window, &w, &h);
        if (w <= 0 || h <= 0)
            return;
        check(vkDeviceWaitIdle(device), "Wait swapchain");
        VkSurfaceCapabilitiesKHR caps{};
        check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps),
              "Read surface capabilities");
        std::uint32_t count{};
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr),
              "Read surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);
        check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()),
              "Read surface formats");
        if (formats.empty())
            throw std::runtime_error("Window surface has no formats");
        auto chosen = formats.front();
        for (auto f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
                f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = f;
        VkFormatProperties properties{};
        vkGetPhysicalDeviceFormatProperties(physical, chosen.format, &properties);
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
            !(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            throw std::runtime_error("Window surface does not support transfer presentation");
        swap_extent = caps.currentExtent;
        if (swap_extent.width == UINT32_MAX)
            swap_extent = {std::clamp(static_cast<std::uint32_t>(w), caps.minImageExtent.width,
                                      caps.maxImageExtent.width),
                           std::clamp(static_cast<std::uint32_t>(h), caps.minImageExtent.height,
                                      caps.maxImageExtent.height)};
        count = caps.minImageCount + 1;
        if (caps.maxImageCount)
            count = std::min(count, caps.maxImageCount);
        VkSwapchainCreateInfoKHR info{};
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = surface;
        info.minImageCount = count;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = swap_extent;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.preTransform = caps.currentTransform;
        info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & info.compositeAlpha)) {
            for (auto a :
                 {VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                  VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
                if (caps.supportedCompositeAlpha & a) {
                    info.compositeAlpha = a;
                    break;
                }
        }
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        info.oldSwapchain = swapchain;
        VkSwapchainKHR next{};
        check(vkCreateSwapchainKHR(device, &info, nullptr, &next), "Create swapchain");
        if (swapchain)
            vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = next;
        swap_format = chosen.format;
        check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "Get swapchain images");
        swap_images.resize(count);
        check(vkGetSwapchainImagesKHR(device, swapchain, &count, swap_images.data()),
              "Get swapchain images");
        swap_layouts.assign(count, VK_IMAGE_LAYOUT_UNDEFINED);
        dirty_swapchain = false;
        if (width != swap_extent.width || height != swap_extent.height) {
            width = swap_extent.width;
            height = swap_extent.height;
            make_targets();
        }
    }
    void make_descriptors() {
        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType =
                i % 2 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 4;
        li.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(device, &li, nullptr, &descriptor_layout),
              "Create descriptor layout");
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048},
                                        {VK_DESCRIPTOR_TYPE_SAMPLER, 2048}};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 1024;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        check(vkCreateDescriptorPool(device, &pi, nullptr, &descriptor_pool),
              "Create descriptor pool");
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0;
        check(vkCreateSampler(device, &si, nullptr, &shadow_sampler), "Create shadow sampler");
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        check(vkCreateSampler(device, &si, nullptr, &color_sampler), "Create color sampler");
    }
    VkDescriptorSet upload_texture(std::shared_ptr<const Texture> source) {
        if (!source)
            source = white;
        if (!source || !source->width || !source->height ||
            source->rgba.size() != std::size_t(source->width) * source->height * 4)
            throw std::invalid_argument("Texture requires width * height * 4 RGBA bytes");
        auto found = textures.find(source.get());
        if (found != textures.end() && found->second.revision == source->revision)
            return found->second.descriptor;
        check(vkDeviceWaitIdle(device), "Wait texture upload");
        GpuTexture texture{};
        texture.source = source;
        texture.revision = source->revision;
        texture.image =
            make_image(source->width, source->height,
                       source->srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT);
        Buffer staging{};
        try {
            staging = make_buffer(source->rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            void* mapped{};
            check(vkMapMemory(device, staging.memory, 0, staging.size, 0, &mapped),
                  "Map texture staging");
            std::memcpy(mapped, source->rgba.data(), source->rgba.size());
            vkUnmapMemory(device, staging.memory);
            begin();
            transition(command, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {source->width, source->height, 1};
            vkCmdCopyBufferToImage(command, staging.handle, texture.image.handle,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            transition(command, texture.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            submit();
            destroy(staging);
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = descriptor_pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &descriptor_layout;
            check(vkAllocateDescriptorSets(device, &ai, &texture.descriptor),
                  "Allocate texture descriptor");
            VkDescriptorImageInfo images[] = {
                {VK_NULL_HANDLE, shadow.view, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL},
                {shadow_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
                {VK_NULL_HANDLE, texture.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {color_sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED}};
            std::array<VkWriteDescriptorSet, 4> writes{};
            for (std::uint32_t i = 0; i < 4; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = texture.descriptor;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType =
                    i % 2 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[i].pImageInfo = &images[i];
            }
            vkUpdateDescriptorSets(device, 4, writes.data(), 0, nullptr);
        } catch (...) {
            destroy(staging);
            destroy(texture.image);
            throw;
        }
        if (found != textures.end()) {
            destroy(found->second.image);
            vkFreeDescriptorSets(device, descriptor_pool, 1, &found->second.descriptor);
            found->second = std::move(texture);
            return found->second.descriptor;
        }
        auto [inserted, _] = textures.emplace(source.get(), std::move(texture));
        return inserted->second.descriptor;
    }
    std::filesystem::path shader_directory() const {
        if (!config.shader_directory.empty())
            return config.shader_directory;
        std::vector<std::filesystem::path> roots;
        const char* base = SDL_GetBasePath();
        if (base)
            roots.emplace_back(faset::path_from_utf8(base) / "shaders");
        roots.emplace_back(std::filesystem::current_path() / "shaders");
        roots.emplace_back(faset::path_from_utf8(FASET_SHADER_DIRECTORY));
        for (const auto& root : roots)
            if (std::filesystem::is_regular_file(faset::native_io_path(root / "vertexMain.spv")))
                return root;
        throw std::runtime_error("Compiled Slang shader bundle is missing");
    }
    VkShaderModule shader(const detail::ShaderCode& code) {
        VkShaderModuleCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ci.codeSize = code.words.size() * sizeof(std::uint32_t);
        ci.pCode = code.words.data();
        VkShaderModule result{};
        check(vkCreateShaderModule(device, &ci, nullptr, &result), "Create shader module");
        return result;
    }
    void make_pipelines() {
        const auto shaders = detail::load_shader_bundle(shader_directory());
        for (std::size_t i = 0; i < shaders.size(); ++i)
            if (!shader_layouts[i].empty() && shader_layouts[i] != shaders[i].layout_fingerprint)
                throw std::runtime_error(
                    "Shader layout changed; the current pipeline was preserved");
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(Push)};
        VkPipelineLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        li.setLayoutCount = 1;
        li.pSetLayouts = &descriptor_layout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(device, &li, nullptr, &pipeline_layout),
              "Create pipeline layout");
        VkShaderModule vertex{}, fragment{}, shadow_vertex{};
        try {
            vertex = shader(shaders[0]);
            fragment = shader(shaders[1]);
            shadow_vertex = shader(shaders[2]);
            for (int mode = 0; mode < 4; ++mode) {
                bool shadow_pass = mode == 2, ui = mode == 1, sprite = mode == 3;
                VkPipelineShaderStageCreateInfo stages[2]{};
                stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
                stages[0].module = shadow_pass ? shadow_vertex : vertex;
                stages[0].pName = "main";
                stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
                stages[1].module = fragment;
                stages[1].pName = "main";
                VkVertexInputBindingDescription binding{0, sizeof(GpuVertex),
                                                        VK_VERTEX_INPUT_RATE_VERTEX};
                VkVertexInputAttributeDescription attrs[] = {
                    {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, clip)},
                    {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, world)},
                    {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(GpuVertex, normal)},
                    {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(GpuVertex, color)},
                    {4, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, material)},
                    {5, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(GpuVertex, uv)}};
                VkPipelineVertexInputStateCreateInfo vi{};
                vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
                vi.vertexBindingDescriptionCount = 1;
                vi.pVertexBindingDescriptions = &binding;
                vi.vertexAttributeDescriptionCount = shadow_pass ? 1 : 6;
                vi.pVertexAttributeDescriptions = shadow_pass ? attrs + 1 : attrs;
                VkPipelineInputAssemblyStateCreateInfo ia{};
                ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
                ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                VkPipelineViewportStateCreateInfo vp{};
                vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
                vp.viewportCount = vp.scissorCount = 1;
                VkPipelineRasterizationStateCreateInfo rs{};
                rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
                rs.polygonMode = VK_POLYGON_MODE_FILL;
                rs.cullMode = VK_CULL_MODE_NONE;
                rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
                rs.lineWidth = 1;
                rs.depthBiasEnable = shadow_pass;
                rs.depthBiasConstantFactor = 1.25f;
                rs.depthBiasSlopeFactor = 1.75f;
                VkPipelineMultisampleStateCreateInfo ms{};
                ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
                ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
                VkPipelineDepthStencilStateCreateInfo ds{};
                ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
                ds.depthTestEnable = !ui;
                ds.depthWriteEnable = !ui && !sprite;
                ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
                VkPipelineColorBlendAttachmentState blend{};
                blend.colorWriteMask = 15;
                blend.blendEnable = VK_TRUE;
                blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
                blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend.colorBlendOp = VK_BLEND_OP_ADD;
                blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                blend.alphaBlendOp = VK_BLEND_OP_ADD;
                VkPipelineColorBlendStateCreateInfo cb{};
                cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
                cb.attachmentCount = shadow_pass ? 0 : 1;
                cb.pAttachments = &blend;
                VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
                VkPipelineDynamicStateCreateInfo dynamic{};
                dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamic.dynamicStateCount = 2;
                dynamic.pDynamicStates = states;
                VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
                VkPipelineRenderingCreateInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
                rendering.colorAttachmentCount = shadow_pass ? 0 : 1;
                rendering.pColorAttachmentFormats = &format;
                rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
                VkGraphicsPipelineCreateInfo pi{};
                pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
                pi.pNext = &rendering;
                pi.stageCount = shadow_pass ? 1 : 2;
                pi.pStages = stages;
                pi.pVertexInputState = &vi;
                pi.pInputAssemblyState = &ia;
                pi.pViewportState = &vp;
                pi.pRasterizationState = &rs;
                pi.pMultisampleState = &ms;
                pi.pDepthStencilState = &ds;
                pi.pColorBlendState = &cb;
                pi.pDynamicState = &dynamic;
                pi.layout = pipeline_layout;
                auto* output = shadow_pass ? &shadow_pipeline
                               : ui        ? &ui_pipeline
                               : sprite    ? &sprite_pipeline
                                           : &pipeline;
                check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, output),
                      "Create graphics pipeline");
            }
        } catch (...) {
            vkDestroyShaderModule(device, vertex, nullptr);
            vkDestroyShaderModule(device, fragment, nullptr);
            vkDestroyShaderModule(device, shadow_vertex, nullptr);
            throw;
        }
        for (std::size_t i = 0; i < shaders.size(); ++i)
            shader_layouts[i] = shaders[i].layout_fingerprint;
        vkDestroyShaderModule(device, vertex, nullptr);
        vkDestroyShaderModule(device, fragment, nullptr);
        vkDestroyShaderModule(device, shadow_vertex, nullptr);
    }
    GpuVertex gpu_vertex(const Vertex& v, const DrawItem& item, const Mat4& vp) {
        GpuVertex out{};
        auto world = point(item.model, {v.position[0], v.position[1], v.position[2], 1});
        auto clip = point(vp, world);
        std::copy(clip.begin(), clip.end(), out.clip);
        std::copy_n(world.begin(), 3, out.world);
        // Inverse-transpose 3x3, including nonuniform scale. Singular models have no valid normal.
        const auto& m = item.model;
        Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
        auto cross = [](Vec3 x, Vec3 y) {
            return Vec3{x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2],
                        x[0] * y[1] - x[1] * y[0]};
        };
        auto ca = cross(b, c), cb = cross(c, a), cc = cross(a, b);
        float determinant = a[0] * ca[0] + a[1] * ca[1] + a[2] * ca[2];
        for (int i = 0; i < 3; ++i)
            out.normal[i] =
                determinant != 0
                    ? (ca[i] * v.normal[0] + cb[i] * v.normal[1] + cc[i] * v.normal[2]) *
                          (determinant < 0 ? -1.f : 1.f)
                    : 0;
        float normal_length = std::hypot(out.normal[0], out.normal[1], out.normal[2]);
        if (normal_length > 0)
            for (float& component : out.normal)
                component /= normal_length;
        for (int i = 0; i < 4; ++i)
            out.color[i] = item.color[i] * v.color[i];
        out.material[0] = item.roughness;
        out.material[1] = item.metallic;
        out.uv[0] = v.uv[0];
        out.uv[1] = v.uv[1];
        return out;
    }
    bool outside(const std::vector<GpuVertex>& data, std::size_t start) const {
        for (int plane = 0; plane < 6; ++plane) {
            bool all = true;
            for (std::size_t i = start; i < data.size(); ++i) {
                auto& p = data[i].clip;
                float d = plane == 0   ? p[0] + p[3]
                          : plane == 1 ? p[3] - p[0]
                          : plane == 2 ? p[1] + p[3]
                          : plane == 3 ? p[3] - p[1]
                          : plane == 4 ? p[2]
                                       : p[3] - p[2];
                if (d >= 0) {
                    all = false;
                    break;
                }
            }
            if (all)
                return true;
        }
        return false;
    }
    void quad(std::vector<GpuVertex>& data, const Quad& q) {
        const float xy[4][2] = {{q.x, q.y},
                                {q.x + q.width, q.y},
                                {q.x + q.width, q.y + q.height},
                                {q.x, q.y + q.height}};
        const float uv[4][2] = {{q.uv_rect[0], q.uv_rect[1]},
                                {q.uv_rect[2], q.uv_rect[1]},
                                {q.uv_rect[2], q.uv_rect[3]},
                                {q.uv_rect[0], q.uv_rect[3]}};
        for (auto i : {0, 1, 2, 0, 2, 3}) {
            GpuVertex v{};
            v.clip[0] = xy[i][0] / float(width) * 2 - 1;
            v.clip[1] = xy[i][1] / float(height) * 2 - 1;
            v.clip[3] = 1;
            std::copy(q.color.begin(), q.color.end(), v.color);
            v.uv[0] = uv[i][0];
            v.uv[1] = uv[i][1];
            v.material[0] = q.texture && q.texture->srgb ? 1.f : 0.f;
            data.push_back(v);
        }
    }
    void draw_debug_text(std::vector<GpuVertex>& data, const Text& text) {
        // Small diagnostic alphabet only. The editor supplies shaped Unicode text as texture quads.
        static const std::unordered_map<char, std::array<unsigned char, 7>> glyphs = {
            {'A', {14, 17, 17, 31, 17, 17, 17}}, {'B', {30, 17, 17, 30, 17, 17, 30}},
            {'C', {14, 17, 16, 16, 16, 17, 14}}, {'D', {30, 17, 17, 17, 17, 17, 30}},
            {'E', {31, 16, 16, 30, 16, 16, 31}}, {'F', {31, 16, 16, 30, 16, 16, 16}},
            {'G', {14, 17, 16, 23, 17, 17, 15}}, {'H', {17, 17, 17, 31, 17, 17, 17}},
            {'I', {14, 4, 4, 4, 4, 4, 14}},      {'J', {7, 2, 2, 2, 18, 18, 12}},
            {'K', {17, 18, 20, 24, 20, 18, 17}}, {'L', {16, 16, 16, 16, 16, 16, 31}},
            {'M', {17, 27, 21, 21, 17, 17, 17}}, {'N', {17, 25, 21, 19, 17, 17, 17}},
            {'O', {14, 17, 17, 17, 17, 17, 14}}, {'P', {30, 17, 17, 30, 16, 16, 16}},
            {'Q', {14, 17, 17, 17, 21, 18, 13}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
            {'S', {15, 16, 16, 14, 1, 1, 30}},   {'T', {31, 4, 4, 4, 4, 4, 4}},
            {'U', {17, 17, 17, 17, 17, 17, 14}}, {'V', {17, 17, 17, 17, 17, 10, 4}},
            {'W', {17, 17, 17, 21, 21, 27, 17}}, {'X', {17, 17, 10, 4, 10, 17, 17}},
            {'Y', {17, 17, 10, 4, 4, 4, 4}},     {'Z', {31, 1, 2, 4, 8, 16, 31}},
            {'0', {14, 17, 19, 21, 25, 17, 14}}, {'1', {4, 12, 4, 4, 4, 4, 14}},
            {'2', {14, 17, 1, 2, 4, 8, 31}},     {'3', {30, 1, 1, 14, 1, 1, 30}},
            {'4', {2, 6, 10, 18, 31, 2, 2}},     {'5', {31, 16, 16, 30, 1, 1, 30}},
            {'6', {14, 16, 16, 30, 17, 17, 14}}, {'7', {31, 1, 2, 4, 8, 8, 8}},
            {'8', {14, 17, 17, 14, 17, 17, 14}}, {'9', {14, 17, 17, 15, 1, 1, 14}},
            {'.', {0, 0, 0, 0, 0, 12, 12}},      {':', {0, 12, 12, 0, 12, 12, 0}},
            {'-', {0, 0, 0, 31, 0, 0, 0}},       {'/', {1, 1, 2, 4, 8, 16, 16}},
            {'_', {0, 0, 0, 0, 0, 0, 31}},       {'(', {2, 4, 8, 8, 8, 4, 2}},
            {')', {8, 4, 2, 2, 2, 4, 8}},        {'+', {0, 4, 4, 31, 4, 4, 0}},
            {'=', {0, 0, 31, 0, 31, 0, 0}},      {'[', {14, 8, 8, 8, 8, 8, 14}},
            {']', {14, 2, 2, 2, 2, 2, 14}},      {'?', {14, 17, 1, 2, 4, 0, 4}},
            {'!', {4, 4, 4, 4, 4, 0, 4}}};
        float x = text.x, y = text.y, unit = text.size / 7;
        for (unsigned char c : text.value) {
            if (c == '\n') {
                x = text.x;
                y += text.size * 1.4f;
                continue;
            }
            if (c >= 'a' && c <= 'z')
                c -= 32;
            if (c != ' ') {
                auto it = glyphs.find(static_cast<char>(c));
                auto pattern = it == glyphs.end()
                                   ? std::array<unsigned char, 7>{31, 17, 17, 17, 17, 17, 31}
                                   : it->second;
                for (int row = 0; row < 7; ++row)
                    for (int col = 0; col < 5; ++col)
                        if (pattern[row] & (1 << (4 - col)))
                            quad(data, {x + col * unit, y + row * unit, unit, unit, text.color});
            }
            x += 6 * unit;
        }
    }
    void render(const Snapshot& snapshot) {
        auto start = std::chrono::steady_clock::now();
        statistics.draw_calls = statistics.culled_meshes = statistics.gpu_label_count = 0;
        bool can_present = surface != VK_NULL_HANDLE;
        if (surface) {
            // A capture may render between normal event-loop iterations. Keep the window
            // system progressing without consuming events intended for the editor.
            SDL_PumpEvents();
            int w{}, h{};
            SDL_GetWindowSizeInPixels(window, &w, &h);
            can_present =
                w > 0 && h > 0 &&
                !(SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
            if (can_present && (dirty_swapchain || !swapchain))
                make_swapchain();
        }
        // Retire atlas/image resources no longer retained by a caller.
        for (auto it = textures.begin(); it != textures.end();) {
            if (it->first != white.get() && it->second.source.use_count() == 1) {
                destroy(it->second.image);
                vkFreeDescriptorSets(device, descriptor_pool, 1, &it->second.descriptor);
                it = textures.erase(it);
            } else
                ++it;
        }
        const VkDescriptorSet white_descriptor = upload_texture(white);
        for (const auto& q : snapshot.ui_quads)
            if (q.texture)
                upload_texture(q.texture);
        for (const auto& draw : snapshot.draws)
            if (draw.texture)
                upload_texture(draw.texture);
        for (const auto& sprite : snapshot.sprites)
            if (sprite.texture)
                upload_texture(sprite.texture);
        for (const auto& triangles : snapshot.ui_triangles)
            if (triangles.texture)
                upload_texture(triangles.texture);
        std::vector<GpuVertex> data;
        std::vector<Batch> scene_batches, shadow_batches, sprite_batches, ui_batches;
        for (const auto& item : snapshot.draws) {
            if (!item.mesh)
                continue;
            auto first = data.size();
            const auto& mesh = *item.mesh;
            auto emit = [&](std::uint32_t index) {
                if (index >= mesh.vertices.size())
                    throw std::out_of_range("Mesh index outside vertex range");
                data.push_back(gpu_vertex(mesh.vertices[index], item, snapshot.view_projection));
            };
            if (mesh.indices.empty())
                for (std::uint32_t i = 0; i < mesh.vertices.size(); ++i)
                    emit(i);
            else
                for (auto i : mesh.indices)
                    emit(i);
            auto count = static_cast<std::uint32_t>(data.size() - first);
            if (count % 3)
                throw std::invalid_argument(
                    "Mesh triangle vertex count must be divisible by three");
            if (!count)
                continue;
            Batch batch{static_cast<std::uint32_t>(first), count,
                        item.texture ? item.texture.get() : white.get()};
            if (item.cast_shadow)
                shadow_batches.push_back(batch);
            if (outside(data, first))
                ++statistics.culled_meshes;
            else
                scene_batches.push_back(batch);
        }
        struct OrderedSprite {
            const Sprite* sprite;
            float depth;
        };
        std::vector<OrderedSprite> ordered_sprites;
        ordered_sprites.reserve(snapshot.sprites.size());
        for (const auto& sprite : snapshot.sprites) {
            const auto clip =
                point(snapshot.view_projection,
                      {sprite.position[0], sprite.position[1], sprite.position[2], 1});
            const auto depth =
                clip[3] != 0 ? clip[2] / clip[3] : std::numeric_limits<float>::infinity();
            ordered_sprites.push_back(
                {&sprite, std::isfinite(depth) ? depth : std::numeric_limits<float>::infinity()});
        }
        std::stable_sort(ordered_sprites.begin(), ordered_sprites.end(),
                         [](const auto& a, const auto& b) {
                             if (a.sprite->layer != b.sprite->layer)
                                 return a.sprite->layer < b.sprite->layer;
                             return a.depth > b.depth;
                         });
        for (const auto& ordered : ordered_sprites) {
            const auto& sprite = *ordered.sprite;
            auto first = static_cast<std::uint32_t>(data.size());
            float c = std::cos(sprite.rotation), s = std::sin(sprite.rotation);
            for (auto i : {0, 1, 2, 0, 2, 3}) {
                const float corners[4][2] = {{-.5f, -.5f}, {.5f, -.5f}, {.5f, .5f}, {-.5f, .5f}};
                float x = corners[i][0] * sprite.size[0], y = corners[i][1] * sprite.size[1];
                auto clip = point(snapshot.view_projection,
                                  {sprite.position[0] + c * x - s * y,
                                   sprite.position[1] + s * x + c * y, sprite.position[2], 1});
                GpuVertex vertex{};
                std::copy(clip.begin(), clip.end(), vertex.clip);
                std::copy(sprite.color.begin(), sprite.color.end(), vertex.color);
                vertex.uv[0] = corners[i][0] + .5f;
                vertex.uv[1] = .5f - corners[i][1];
                vertex.material[0] = sprite.texture && sprite.texture->srgb ? 1.f : 0.f;
                data.push_back(vertex);
            }
            sprite_batches.push_back(
                {first, 6, sprite.texture ? sprite.texture.get() : white.get()});
        }
        for (const auto& q : snapshot.ui_quads) {
            auto first = static_cast<std::uint32_t>(data.size());
            quad(data, q);
            const Texture* texture = q.texture ? q.texture.get() : white.get();
            if (!ui_batches.empty() && ui_batches.back().texture == texture)
                ui_batches.back().count += 6;
            else
                ui_batches.push_back({first, 6, texture});
        }
        auto text_first = static_cast<std::uint32_t>(data.size());
        for (const auto& text : snapshot.ui_text)
            draw_debug_text(data, text);
        if (data.size() > text_first)
            ui_batches.push_back(
                {text_first, static_cast<std::uint32_t>(data.size() - text_first), white.get()});
        for (const auto& triangles : snapshot.ui_triangles) {
            if (triangles.vertices.size() % 3 != 0)
                throw std::invalid_argument("UI triangle list must contain complete triangles");
            const auto first = static_cast<std::uint32_t>(data.size());
            for (const auto& source : triangles.vertices) {
                GpuVertex vertex{};
                vertex.clip[0] = source.position[0] / float(width) * 2 - 1;
                vertex.clip[1] = source.position[1] / float(height) * 2 - 1;
                vertex.clip[3] = 1;
                std::copy(source.color.begin(), source.color.end(), vertex.color);
                std::copy(source.uv.begin(), source.uv.end(), vertex.uv);
                vertex.material[0] = triangles.texture && triangles.texture->srgb ? 1.f : 0.f;
                data.push_back(vertex);
            }
            ui_batches.push_back({first, static_cast<std::uint32_t>(triangles.vertices.size()),
                                  triangles.texture ? triangles.texture.get() : white.get(),
                                  triangles.clip_rect});
        }
        statistics.vertices = static_cast<std::uint32_t>(data.size());
        auto byte_count = std::max<std::size_t>(sizeof(GpuVertex), data.size() * sizeof(GpuVertex));
        if (vertices.size < byte_count) {
            destroy(vertices);
            vertices = make_buffer(byte_count, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        void* mapped{};
        check(vkMapMemory(device, vertices.memory, 0, vertices.size, 0, &mapped), "Map vertices");
        if (!data.empty())
            std::memcpy(mapped, data.data(), data.size() * sizeof(GpuVertex));
        vkUnmapMemory(device, vertices.memory);
        Vec3 direction = snapshot.light_direction;
        float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                 direction[2] * direction[2]);
        if (length < 1e-5f) {
            direction = {-.5f, -1, -.3f};
            length = std::sqrt(1.34f);
        }
        for (auto& v : direction)
            v /= length;
        Vec3 light_eye{-direction[0] * 30, -direction[1] * 30, -direction[2] * 30};
        Vec3 light_up = std::abs(direction[1]) > .98f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
        Push push{multiply(orthographic(-20, 20, -20, 20, .1f, 80),
                           look_at(light_eye, {0, 0, 0}, light_up)),
                  {direction[0], direction[1], direction[2], 0},
                  {snapshot.eye[0], snapshot.eye[1], snapshot.eye[2], 1}};
        std::optional<std::uint32_t> swap_index;
        if (can_present) {
            std::uint32_t index{};
            // Compositors can withhold images while a window is occluded. Rendering and
            // editor capture must remain available even when presentation cannot advance.
            const auto result =
                vkAcquireNextImageKHR(device, swapchain, 0, acquired, VK_NULL_HANDLE, &index);
            if (result == VK_ERROR_OUT_OF_DATE_KHR) {
                dirty_swapchain = true;
            } else if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) {
                swap_index = index;
                if (result == VK_SUBOPTIMAL_KHR)
                    dirty_swapchain = true;
            } else if (result != VK_NOT_READY && result != VK_TIMEOUT) {
                check(result, "Acquire swapchain image");
            }
        }
        begin();
        if (timestamp_pool) {
            vkCmdResetQueryPool(command, timestamp_pool, 0, 2);
            vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, timestamp_pool, 0);
        }
        VkDeviceSize offset{};
        vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
        vkCmdPushConstants(command, pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        auto set_viewport = [&](std::uint32_t w, std::uint32_t h) {
            VkViewport viewport{0, 0, float(w), float(h), 0, 1};
            VkRect2D scissor{{0, 0}, {w, h}};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
        };
        RenderGraph graph;
        auto add_pass = [&](std::string name, std::vector<std::string> reads,
                            std::vector<std::string> writes, RenderGraph::Callback callback) {
            const auto label_name = name;
            graph.add(std::move(name), std::move(reads), std::move(writes),
                      [this, label_name, callback = std::move(callback)] {
                          if (statistics.gpu_labels_enabled) {
                              VkDebugUtilsLabelEXT label{};
                              label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
                              label.pLabelName = label_name.c_str();
                              label.color[0] = .25f;
                              label.color[1] = .65f;
                              label.color[2] = .9f;
                              label.color[3] = 1.f;
                              begin_gpu_label(command, &label);
                              ++statistics.gpu_label_count;
                          }
                          struct EndLabel {
                              Impl& renderer;
                              ~EndLabel() {
                                  if (renderer.statistics.gpu_labels_enabled)
                                      renderer.end_gpu_label(renderer.command);
                              }
                          } end{*this};
                          callback();
                      });
        };
        add_pass("ShadowMap", {}, {"shadow"}, [&] {
            transition(command, shadow, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
            VkRenderingAttachmentInfo attachment{};
            attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            attachment.imageView = shadow.view;
            attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.clearValue.depthStencil = {1, 0};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, {shadow_size, shadow_size}};
            rendering.layerCount = 1;
            rendering.pDepthAttachment = &attachment;
            vkCmdBeginRendering(command, &rendering);
            set_viewport(shadow_size, shadow_size);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            for (auto batch : shadow_batches) {
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
            vkCmdEndRendering(command);
            transition(command, shadow, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
        });
        add_pass("ForwardAndUI", {"shadow"}, {"color", "depth"}, [&] {
            transition(command, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            transition(command, depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_ASPECT_DEPTH_BIT);
            VkRenderingAttachmentInfo ca{};
            ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            ca.imageView = color.view;
            ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            std::copy(snapshot.clear_color.begin(), snapshot.clear_color.end(),
                      ca.clearValue.color.float32);
            VkRenderingAttachmentInfo da{};
            da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            da.imageView = depth.view;
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            da.clearValue.depthStencil = {1, 0};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, {width, height}};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &ca;
            rendering.pDepthAttachment = &da;
            vkCmdBeginRendering(command, &rendering);
            set_viewport(width, height);
            if (snapshot.scene_rect[2] > 0 && snapshot.scene_rect[3] > 0) {
                auto r = snapshot.scene_rect;
                float x = std::clamp(r[0], 0.f, float(width)),
                      y = std::clamp(r[1], 0.f, float(height));
                float w = std::min(r[2], float(width) - x), h = std::min(r[3], float(height) - y);
                VkViewport viewport{x, y, w, h, 0, 1};
                VkRect2D scissor{{static_cast<int>(x), static_cast<int>(y)},
                                 {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)}};
                vkCmdSetViewport(command, 0, 1, &viewport);
                vkCmdSetScissor(command, 0, 1, &scissor);
            }
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                                    &white_descriptor, 0, nullptr);
            for (auto batch : scene_batches) {
                auto descriptor = textures.at(batch.texture).descriptor;
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                        0, 1, &descriptor, 0, nullptr);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, sprite_pipeline);
            for (auto batch : sprite_batches) {
                auto descriptor = textures.at(batch.texture).descriptor;
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                        0, 1, &descriptor, 0, nullptr);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
            set_viewport(width, height);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            for (auto batch : ui_batches) {
                VkRect2D scissor{{0, 0}, {width, height}};
                if (batch.clip_rect[2] > 0 && batch.clip_rect[3] > 0) {
                    const auto& clip = batch.clip_rect;
                    const float x = std::clamp(clip[0], 0.f, float(width));
                    const float y = std::clamp(clip[1], 0.f, float(height));
                    const float right = std::clamp(clip[0] + clip[2], x, float(width));
                    const float bottom = std::clamp(clip[1] + clip[3], y, float(height));
                    scissor.offset = {static_cast<int>(x), static_cast<int>(y)};
                    scissor.extent = {static_cast<unsigned>(right) - static_cast<unsigned>(x),
                                      static_cast<unsigned>(bottom) - static_cast<unsigned>(y)};
                }
                if (!scissor.extent.width || !scissor.extent.height)
                    continue;
                vkCmdSetScissor(command, 0, 1, &scissor);
                auto descriptor = textures.at(batch.texture).descriptor;
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                        0, 1, &descriptor, 0, nullptr);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
            vkCmdEndRendering(command);
        });
        add_pass("Readback", {"color"}, {"capture"}, [&] {
            transition(command, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {width, height, 1};
            vkCmdCopyImageToBuffer(command, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   readback.handle, 1, &copy);
        });
        if (swap_index)
            add_pass("Presentation", {"color"}, {"swapchain"}, [&] {
                auto index = *swap_index;
                transition(command, swap_images[index], swap_layouts[index],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {static_cast<int>(width), static_cast<int>(height), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[1] = {static_cast<int>(swap_extent.width),
                                      static_cast<int>(swap_extent.height), 1};
                vkCmdBlitImage(command, color.handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               swap_images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_NEAREST);
                transition(command, swap_images[index], swap_layouts[index],
                           VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
            });
        graph.execute();
        if (timestamp_pool)
            vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, timestamp_pool,
                                 1);
        submit(swap_index.has_value());
        if (timestamp_pool) {
            std::uint64_t stamps[2]{};
            check(vkGetQueryPoolResults(device, timestamp_pool, 0, 2, sizeof(stamps), stamps,
                                        sizeof(std::uint64_t),
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                  "Read GPU timestamps");
            auto delta = stamps[1] - stamps[0];
            if (timestamp_bits < 64)
                delta &= (std::uint64_t(1) << timestamp_bits) - 1;
            statistics.gpu_ms = double(delta) * timestamp_period / 1000000.0;
        }
        if (swap_index) {
            VkPresentInfoKHR present{};
            present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            present.waitSemaphoreCount = 1;
            present.pWaitSemaphores = &present_ready;
            present.swapchainCount = 1;
            present.pSwapchains = &swapchain;
            present.pImageIndices = &*swap_index;
            auto result = vkQueuePresentKHR(queue, &present);
            if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
                dirty_swapchain = true;
            else
                check(result, "Present frame");
            check(vkQueueWaitIdle(queue), "Wait presentation");
        }
        const auto readback_started = std::chrono::steady_clock::now();
        last_pixels.resize(std::size_t(width) * height * 4);
        check(vkMapMemory(device, readback.memory, 0, readback.size, 0, &mapped),
              "Map captured frame");
        std::memcpy(last_pixels.data(), mapped, last_pixels.size());
        vkUnmapMemory(device, readback.memory);
        statistics.readback_cpu_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - readback_started)
                                         .count();
        ++statistics.frame;
        statistics.gpu_allocated_bytes = vertices.allocation_size + readback.allocation_size +
                                         color.allocation_size + depth.allocation_size +
                                         shadow.allocation_size;
        statistics.texture_count = static_cast<std::uint32_t>(textures.size());
        for (const auto& [_, texture] : textures)
            statistics.gpu_allocated_bytes += texture.image.allocation_size;
        statistics.validation_errors = validation_errors.load();
        statistics.cpu_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
    }
};
Renderer::Renderer(const RendererConfig& config) : impl_(std::make_unique<Impl>()) {
    impl_->initialize(config);
}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;
void Renderer::render(const Snapshot& snapshot) {
    impl_->render(snapshot);
}
bool Renderer::reload_shaders(std::string& error) {
    auto& r = *impl_;
    check(vkDeviceWaitIdle(r.device), "Wait shader reload");
    auto previous_layout = r.pipeline_layout;
    auto previous = r.pipeline;
    auto previous_ui = r.ui_pipeline;
    auto previous_shadow = r.shadow_pipeline;
    auto previous_sprite = r.sprite_pipeline;
    r.pipeline_layout = {};
    r.pipeline = {};
    r.ui_pipeline = {};
    r.shadow_pipeline = {};
    r.sprite_pipeline = {};
    try {
        r.make_pipelines();
    } catch (const std::exception& exception) {
        if (r.pipeline)
            vkDestroyPipeline(r.device, r.pipeline, nullptr);
        if (r.ui_pipeline)
            vkDestroyPipeline(r.device, r.ui_pipeline, nullptr);
        if (r.shadow_pipeline)
            vkDestroyPipeline(r.device, r.shadow_pipeline, nullptr);
        if (r.sprite_pipeline)
            vkDestroyPipeline(r.device, r.sprite_pipeline, nullptr);
        if (r.pipeline_layout)
            vkDestroyPipelineLayout(r.device, r.pipeline_layout, nullptr);
        r.pipeline_layout = previous_layout;
        r.pipeline = previous;
        r.ui_pipeline = previous_ui;
        r.shadow_pipeline = previous_shadow;
        r.sprite_pipeline = previous_sprite;
        error = exception.what();
        return false;
    }
    vkDestroyPipeline(r.device, previous, nullptr);
    vkDestroyPipeline(r.device, previous_ui, nullptr);
    vkDestroyPipeline(r.device, previous_shadow, nullptr);
    vkDestroyPipeline(r.device, previous_sprite, nullptr);
    vkDestroyPipelineLayout(r.device, previous_layout, nullptr);
    error.clear();
    return true;
}
void Renderer::resize(std::uint32_t w, std::uint32_t h) {
    if (!w || !h)
        return;
    if (impl_->window) {
        SDL_SetWindowSize(impl_->window, static_cast<int>(w), static_cast<int>(h));
        impl_->dirty_swapchain = true;
    } else if (w != impl_->width || h != impl_->height) {
        impl_->width = w;
        impl_->height = h;
        impl_->make_targets();
    }
}
std::uint32_t Renderer::width() const {
    return impl_->width;
}
std::uint32_t Renderer::height() const {
    return impl_->height;
}
float Renderer::display_scale() const {
    const float scale = impl_->window ? SDL_GetWindowDisplayScale(impl_->window) : 1.f;
    return std::isfinite(scale) && scale > 0.f ? scale : 1.f;
}
bool Renderer::should_close() const {
    return impl_->close;
}
const FrameStats& Renderer::stats() const {
    return impl_->statistics;
}
std::vector<std::uint8_t> Renderer::pixels() const {
    return impl_->last_pixels;
}
void Renderer::capture(const std::filesystem::path& path) {
    if (impl_->last_pixels.empty())
        throw std::runtime_error("Cannot capture before a completed frame");
    std::ofstream out(faset::native_io_path(path), std::ios::binary);
    if (!out)
        throw std::runtime_error("Cannot write screenshot: " + faset::path_to_utf8(path));
    out << "P6\n" << width() << ' ' << height() << "\n255\n";
    for (std::size_t i = 0; i < impl_->last_pixels.size(); i += 4)
        out.write(reinterpret_cast<const char*>(impl_->last_pixels.data() + i), 3);
    if (!out)
        throw std::runtime_error("Screenshot write failed");
}
void Renderer::set_title(const std::string& title) {
    if (impl_->window)
        SDL_SetWindowTitle(impl_->window, title.c_str());
}
void Renderer::set_text_input(bool enabled) {
    if (!impl_->window)
        return;
    if (enabled)
        SDL_StartTextInput(impl_->window);
    else
        SDL_StopTextInput(impl_->window);
}
void Renderer::set_text_input_area(float x, float y, float width, float height) {
    if (!impl_->window)
        return;
    int w{}, h{}, pw{}, ph{};
    SDL_GetWindowSize(impl_->window, &w, &h);
    SDL_GetWindowSizeInPixels(impl_->window, &pw, &ph);
    float sx = pw > 0 ? float(w) / float(pw) : 1, sy = ph > 0 ? float(h) / float(ph) : 1;
    SDL_Rect rectangle{int(x * sx), int(y * sy), std::max(1, int(width * sx)),
                       std::max(1, int(height * sy))};
    if (!SDL_SetTextInputArea(impl_->window, &rectangle, 0))
        throw std::runtime_error(SDL_GetError());
}
void Renderer::set_clipboard(const std::string& text) {
    if (!impl_->window) {
        impl_->offscreen_clipboard = text;
        return;
    }
    if (!SDL_SetClipboardText(text.c_str()))
        throw std::runtime_error(SDL_GetError());
}
std::string Renderer::clipboard() const {
    if (!impl_->window)
        return impl_->offscreen_clipboard;
    char* text = SDL_GetClipboardText();
    if (!text)
        return {};
    std::string result = text;
    SDL_free(text);
    return result;
}
std::vector<Event> Renderer::poll_events() {
    std::vector<Event> result;
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
        Event item;
        bool emit = true;
        auto modifiers = SDL_GetModState();
        item.control = (modifiers & SDL_KMOD_CTRL) != 0;
        item.shift = (modifiers & SDL_KMOD_SHIFT) != 0;
        item.alt = (modifiers & SDL_KMOD_ALT) != 0;
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            item.type = Event::Type::Quit;
            impl_->close = true;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            item.type = Event::Type::Resize;
            item.x = float(event.window.data1);
            item.y = float(event.window.data2);
            impl_->dirty_swapchain = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            item.type = Event::Type::FocusGained;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            item.type = Event::Type::FocusLost;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            item.type = Event::Type::MouseMove;
            item.x = event.motion.x;
            item.y = event.motion.y;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            item.type = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? Event::Type::MouseDown
                                                                  : Event::Type::MouseUp;
            item.x = event.button.x;
            item.y = event.button.y;
            item.button = event.button.button;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            item.type = Event::Type::Wheel;
            item.x = event.wheel.x;
            item.y = event.wheel.y;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            item.type =
                event.type == SDL_EVENT_KEY_DOWN ? Event::Type::KeyDown : Event::Type::KeyUp;
            item.key = SDL_GetKeyName(event.key.key);
            item.repeat = event.key.repeat;
            break;
        case SDL_EVENT_TEXT_INPUT:
            item.type = Event::Type::TextInput;
            item.text = event.text.text;
            break;
        case SDL_EVENT_TEXT_EDITING:
            item.type = Event::Type::TextEditing;
            item.text = event.edit.text;
            item.edit_start = event.edit.start;
            item.edit_length = event.edit.length;
            break;
        default:
            emit = false;
        }
        // Rendering/UI coordinates use drawable pixels; SDL pointer events use logical window
        // units.
        if (impl_->window &&
            (item.type == Event::Type::MouseMove || item.type == Event::Type::MouseDown ||
             item.type == Event::Type::MouseUp)) {
            int w{}, h{}, pw{}, ph{};
            SDL_GetWindowSize(impl_->window, &w, &h);
            SDL_GetWindowSizeInPixels(impl_->window, &pw, &ph);
            if (w > 0 && h > 0) {
                item.x *= float(pw) / float(w);
                item.y *= float(ph) / float(h);
            }
        }
        if (emit)
            result.push_back(std::move(item));
    }
    return result;
}
} // namespace faset::render
