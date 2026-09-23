#include "shader_contract.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <faset/core/io.hpp>
#include <faset/render/render_graph.hpp>
#include <faset/render/renderer.hpp>
#include <faset/render/visibility.hpp>
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
// The GPU path keeps local geometry separate from the instance table. All layouts
// below are mirrored by gpu_scene.slang and checked by shader reflection tests.
struct SceneVertex {
    float position[3], normal[3], color[4], uv[2];
};
static_assert(sizeof(SceneVertex) == 48);
struct SceneInstance {
    Mat4 model;
    std::array<float, 4> normal0, normal1, normal2;
    std::array<float, 4> color, material;
    std::array<float, 4> center_extent, half_extent;
    std::array<float, 4> previous_center_extent, previous_half_extent;
    std::array<std::uint32_t, 4> metadata;
};
static_assert(sizeof(SceneInstance) == 224);
struct SceneView {
    Mat4 current_vp, previous_vp;
    std::array<float, 4> viewport, previous_viewport;
    std::array<std::uint32_t, 4> hzb_dimensions, previous_hzb_dimensions, flags;
};
static_assert(sizeof(SceneView) == 208);
struct SceneBin {
    std::uint32_t candidate_first, candidate_count, visible_base, capacity;
};
struct SceneCandidate {
    std::uint32_t instance_id, bin_index, flags, unused;
};
struct SceneIndirect {
    std::uint32_t vertex_count, instance_count, first_vertex, first_instance;
};
static_assert(sizeof(SceneBin) == 16 && sizeof(SceneCandidate) == 16 &&
              sizeof(SceneIndirect) == 16);
struct Push {
    Mat4 light_view_projection;
    std::array<float, 4> light_direction, eye;
};
static_assert(sizeof(Push) == 96, "Slang FrameParameters layout");
struct ScenePush {
    Push frame;
    std::array<std::uint32_t, 4> draw_info;
};
static_assert(sizeof(ScenePush) == 112);
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
    std::vector<VkImageView> mip_views;
    std::uint32_t mip_levels{1};
    VkImageLayout layout{VK_IMAGE_LAYOUT_UNDEFINED};
    VkDeviceSize allocation_size{};
};
struct Batch {
    std::uint32_t first{}, count{};
    const Texture* texture{};
    std::array<float, 4> clip_rect{};
};
struct SceneDrawBin {
    const Mesh* mesh{};
    const Texture* texture{};
    SceneIndirect command{};
    SceneBin range{};
};
struct PreparedScene {
    std::vector<SceneVertex> vertices;
    std::vector<SceneInstance> instances;
    std::vector<SceneCandidate> candidates;
    std::vector<SceneBin> bins;
    std::vector<SceneIndirect> commands;
    std::vector<const Texture*> textures;
    SceneView view{};
    std::uint32_t candidate_count{};
};
float projected_pixels(const Bounds& bounds, const Mat4& view_projection,
                       const std::array<float, 4>& viewport) {
    float min_x = std::numeric_limits<float>::infinity();
    float max_x = -min_x, min_y = min_x, max_y = -min_x;
    for (unsigned corner = 0; corner < 8; ++corner) {
        Vec3 p{corner & 1 ? bounds.max[0] : bounds.min[0],
               corner & 2 ? bounds.max[1] : bounds.min[1],
               corner & 4 ? bounds.max[2] : bounds.min[2]};
        auto clip = point(view_projection, {p[0], p[1], p[2], 1});
        if (!std::isfinite(clip[0]) || !std::isfinite(clip[1]) ||
            !std::isfinite(clip[3]) || clip[3] <= 0)
            return std::numeric_limits<float>::max();
        const float x = (clip[0] / clip[3] * .5f + .5f) * viewport[2];
        const float y = (clip[1] / clip[3] * .5f + .5f) * viewport[3];
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    return std::max(max_x - min_x, max_y - min_y);
}
bool opaque_texture(const Texture* texture) {
    if (!texture)
        return true;
    for (std::size_t i = 3; i < texture->rgba.size(); i += 4)
        if (texture->rgba[i] != 255)
            return false;
    return true;
}
struct SceneResources {
    Buffer vertices, instances, candidates, bins, view;
    Buffer main_ids, post_ids, main_args, post_args, deferred_ids, deferred_count;
    Image hzb[2];
    VkDescriptorSetLayout graphics_layout{}, cull_layout{}, hzb_layout{};
    VkDescriptorPool descriptor_pool{};
    VkDescriptorSet graphics_main{}, graphics_post{}, cull_main{}, cull_post{};
    std::vector<VkDescriptorSet> hzb_sets[2];
    VkPipelineLayout graphics_pipeline_layout{}, cull_pipeline_layout{}, hzb_pipeline_layout{};
    VkPipeline graphics_pipeline{}, cull_pipeline{}, post_pipeline{}, hzb_pipeline{};
    std::array<std::string, 5> shader_layouts{};
    std::uint32_t hzb_mips{}, hzb_current{};
    bool hzb_history_valid{}, available{};
    bool hzb_supported{}, hzb_extent_supported{};
    Mat4 previous_vp{identity};
    Mat4 previous_projection{identity};
    std::array<float, 4> previous_viewport{};
    std::string previous_view_id;
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
    std::uint32_t max_compute_groups_x{}, max_storage_buffer_range{},
                  max_image_dimension{};
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
    SceneResources scene;
    InstanceTracker instance_tracker;
    std::unordered_map<std::string, std::size_t> previous_lods;
    struct CachedBounds {
        std::weak_ptr<const Mesh> owner;
        Bounds local;
    };
    struct CachedOpacity {
        std::weak_ptr<const Texture> owner;
        std::uint64_t revision{};
        bool opaque{};
    };
    std::unordered_map<const Mesh*, CachedBounds> bounds_cache;
    std::unordered_map<const Texture*, CachedOpacity> opacity_cache;
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
            for (auto view : i.mip_views)
                vkDestroyImageView(device, view, nullptr);
            if (i.view)
                vkDestroyImageView(device, i.view, nullptr);
            if (i.handle)
                vkDestroyImage(device, i.handle, nullptr);
            if (i.memory)
                vkFreeMemory(device, i.memory, nullptr);
        }
        i = {};
    }
    void destroy_scene_interfaces() {
        if (!device)
            return;
        for (auto* pipeline : {&scene.graphics_pipeline, &scene.cull_pipeline,
                               &scene.post_pipeline, &scene.hzb_pipeline}) {
            if (*pipeline)
                vkDestroyPipeline(device, *pipeline, nullptr);
            *pipeline = {};
        }
        for (auto* layout : {&scene.graphics_pipeline_layout, &scene.cull_pipeline_layout,
                             &scene.hzb_pipeline_layout}) {
            if (*layout)
                vkDestroyPipelineLayout(device, *layout, nullptr);
            *layout = {};
        }
        if (scene.descriptor_pool)
            vkDestroyDescriptorPool(device, scene.descriptor_pool, nullptr);
        scene.descriptor_pool = {};
        for (auto* layout : {&scene.graphics_layout, &scene.cull_layout,
                             &scene.hzb_layout}) {
            if (*layout)
                vkDestroyDescriptorSetLayout(device, *layout, nullptr);
            *layout = {};
        }
        scene.graphics_main = scene.graphics_post = scene.cull_main =
            scene.cull_post = {};
        for (auto& sets : scene.hzb_sets)
            sets.clear();
        scene.shader_layouts = {};
    }
    void cleanup() {
        if (device)
            vkDeviceWaitIdle(device);
        for (auto& [_, texture] : textures)
            destroy(texture.image);
        destroy(scene.vertices);
        destroy(scene.instances);
        destroy(scene.candidates);
        destroy(scene.bins);
        destroy(scene.view);
        destroy(scene.main_ids);
        destroy(scene.post_ids);
        destroy(scene.main_args);
        destroy(scene.post_args);
        destroy(scene.deferred_ids);
        destroy(scene.deferred_count);
        destroy(scene.hzb[0]);
        destroy(scene.hzb[1]);
        destroy(vertices);
        destroy(readback);
        destroy(color);
        destroy(depth);
        destroy(shadow);
        if (device) {
            destroy_scene_interfaces();
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
                     VkImageAspectFlags aspect, std::uint32_t mip_levels = 1) {
        Image image{};
        image.mip_levels = mip_levels;
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = {w, h, 1};
        info.mipLevels = mip_levels;
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
            view.subresourceRange = {aspect, 0, mip_levels, 0, 1};
            check(vkCreateImageView(device, &view, nullptr, &image.view), "Create image view");
            if (mip_levels > 1) {
                image.mip_views.reserve(mip_levels);
                for (std::uint32_t level = 0; level < mip_levels; ++level) {
                    view.subresourceRange = {aspect, level, 1, 0, 1};
                    VkImageView mip_view{};
                    check(vkCreateImageView(device, &view, nullptr, &mip_view),
                          "Create mip view");
                    image.mip_views.push_back(mip_view);
                }
            }
        } catch (...) {
            destroy(image);
            throw;
        }
        return image;
    }
    void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout& before, VkImageLayout after,
                    VkImageAspectFlags aspect, std::uint32_t mip_levels = 1) {
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
        barrier.subresourceRange = {aspect, 0, mip_levels, 0, 1};
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
        before = after;
    }
    void transition(VkCommandBuffer cmd, Image& image, VkImageLayout after,
                    VkImageAspectFlags aspect) {
        transition(cmd, image.handle, image.layout, after, aspect, image.mip_levels);
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
            VkPhysicalDeviceVulkan11Features f11{};
            f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
            VkPhysicalDeviceVulkan13Features f13{};
            f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
            f11.pNext = &f13;
            VkPhysicalDeviceFeatures2 features{};
            features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features.pNext = &f11;
            vkGetPhysicalDeviceFeatures2(gpu, &features);
            if (!f13.synchronization2 || !f13.dynamicRendering)
                continue;
            VkFormatProperties color_props{}, depth_props{}, hzb_props{};
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R8G8B8A8_UNORM, &color_props);
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_D32_SFLOAT, &depth_props);
            vkGetPhysicalDeviceFormatProperties(gpu, VK_FORMAT_R32_SFLOAT, &hzb_props);
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
                    max_compute_groups_x = properties.limits.maxComputeWorkGroupCount[0];
                    max_storage_buffer_range = properties.limits.maxStorageBufferRange;
                    max_image_dimension = properties.limits.maxImageDimension2D;
                    scene.available = (queues[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 &&
                                      f11.shaderDrawParameters &&
                                      properties.limits.maxPerStageDescriptorStorageBuffers >= 8 &&
                                      properties.limits.maxDescriptorSetStorageBuffers >= 8 &&
                                      properties.limits.maxComputeWorkGroupInvocations >= 64 &&
                                      properties.limits.maxComputeWorkGroupSize[0] >= 64 &&
                                      max_compute_groups_x > 0;
                    scene.hzb_supported = scene.available &&
                        (hzb_props.optimalTilingFeatures &
                         (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) ==
                        (VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                         VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                         VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                         VK_FORMAT_FEATURE_TRANSFER_DST_BIT);
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
        VkPhysicalDeviceVulkan11Features f11{};
        f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        f11.shaderDrawParameters = scene.available ? VK_TRUE : VK_FALSE;
        VkPhysicalDeviceVulkan13Features f13{};
        f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        f13.synchronization2 = VK_TRUE;
        f13.dynamicRendering = VK_TRUE;
        f11.pNext = &f13;
        VkDeviceCreateInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        di.pNext = &f11;
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
            query.queryCount = 12;
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
        if (scene.available && c.visibility_mode != VisibilityMode::Direct)
            make_scene_descriptors_and_pipelines();
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
        destroy(scene.hzb[0]);
        destroy(scene.hzb[1]);
        scene.hzb_history_valid = false;
        color = make_image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);
        depth = make_image(width, height, VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
        const auto padded_width = std::bit_ceil(width);
        const auto padded_height = std::bit_ceil(height);
        scene.hzb_extent_supported = scene.hzb_supported &&
            padded_width <= max_image_dimension && padded_height <= max_image_dimension &&
            (padded_width + 7u) / 8u <= max_compute_groups_x;
        if (scene.hzb_extent_supported &&
            config.visibility_mode == VisibilityMode::GpuOcclusion) {
            scene.hzb_mips = std::bit_width(std::max(padded_width, padded_height));
            for (auto& pyramid : scene.hzb)
                pyramid = make_image(padded_width, padded_height, VK_FORMAT_R32_SFLOAT,
                                     VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_STORAGE_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT, scene.hzb_mips);
            scene.hzb_current = 0;
        } else {
            scene.hzb_mips = 0;
        }
        // CPU reads this allocation every frame. Prefer cached coherent memory when available.
        readback =
            make_buffer(VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        last_pixels.clear();
        if (scene.graphics_layout)
            refresh_scene_descriptors();
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
    void refresh_scene_descriptors() {
        if (!scene.graphics_layout)
            return;
        if (scene.descriptor_pool)
            vkDestroyDescriptorPool(device, scene.descriptor_pool, nullptr);
        scene.graphics_main = scene.graphics_post = scene.cull_main = scene.cull_post = {};
        for (auto& sets : scene.hzb_sets)
            sets.clear();
        const std::uint32_t hzb_sets = scene.hzb_mips * 2;
        const std::array<VkDescriptorPoolSize, 3> sizes{{
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 64},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 16 + hzb_sets},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, hzb_sets}}};
        VkDescriptorPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool.maxSets = 4 + hzb_sets;
        pool.poolSizeCount = hzb_sets ? static_cast<std::uint32_t>(sizes.size()) : 2;
        pool.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(device, &pool, nullptr, &scene.descriptor_pool),
              "Create GPU visibility descriptor pool");
        const std::array<VkDescriptorSetLayout, 4> layouts{
            scene.graphics_layout, scene.graphics_layout, scene.cull_layout, scene.cull_layout};
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = scene.descriptor_pool;
        allocation.descriptorSetCount = 4;
        allocation.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, 4> sets{};
        check(vkAllocateDescriptorSets(device, &allocation, sets.data()),
              "Allocate GPU visibility descriptors");
        scene.graphics_main = sets[0];
        scene.graphics_post = sets[1];
        scene.cull_main = sets[2];
        scene.cull_post = sets[3];
        if (!scene.hzb_mips)
            return;
        for (std::uint32_t image = 0; image < 2; ++image) {
            auto& target_sets = scene.hzb_sets[image];
            target_sets.resize(scene.hzb_mips);
            std::vector<VkDescriptorSetLayout> mip_layouts(scene.hzb_mips, scene.hzb_layout);
            allocation.descriptorSetCount = scene.hzb_mips;
            allocation.pSetLayouts = mip_layouts.data();
            check(vkAllocateDescriptorSets(device, &allocation, target_sets.data()),
                  "Allocate HZB mip descriptors");
            for (std::uint32_t mip = 0; mip < scene.hzb_mips; ++mip) {
                const auto source = mip == 0 ? depth.view : scene.hzb[image].mip_views[mip - 1];
                const auto output = scene.hzb[image].mip_views.empty()
                    ? scene.hzb[image].view : scene.hzb[image].mip_views[mip];
                VkDescriptorImageInfo images[] = {
                    {VK_NULL_HANDLE, source,
                     mip == 0 ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
                              : VK_IMAGE_LAYOUT_GENERAL},
                    {VK_NULL_HANDLE, output, VK_IMAGE_LAYOUT_GENERAL}};
                VkWriteDescriptorSet writes[2]{};
                for (std::uint32_t binding = 0; binding < 2; ++binding) {
                    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[binding].dstSet = target_sets[mip];
                    writes[binding].dstBinding = binding;
                    writes[binding].descriptorCount = 1;
                    writes[binding].descriptorType = binding == 0
                        ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    writes[binding].pImageInfo = &images[binding];
                }
                vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
            }
        }
    }
    void make_scene_descriptors_and_pipelines() {
        std::array<VkDescriptorSetLayoutBinding, 3> graphics{};
        for (std::uint32_t i = 0; i < graphics.size(); ++i)
            graphics[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                           nullptr};
        VkDescriptorSetLayoutCreateInfo layout{};
        layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layout.bindingCount = static_cast<std::uint32_t>(graphics.size());
        layout.pBindings = graphics.data();
        check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &scene.graphics_layout),
              "Create GPU scene descriptor layout");
        std::array<VkDescriptorSetLayoutBinding, 10> cull{};
        for (std::uint32_t i = 0; i < cull.size(); ++i)
            cull[i] = {i, i == 7 || i == 8 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                            : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                       1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        layout.bindingCount = static_cast<std::uint32_t>(cull.size());
        layout.pBindings = cull.data();
        check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &scene.cull_layout),
              "Create GPU cull descriptor layout");
        std::array<VkDescriptorSetLayoutBinding, 2> hzb{{
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        layout.bindingCount = static_cast<std::uint32_t>(hzb.size());
        layout.pBindings = hzb.data();
        check(vkCreateDescriptorSetLayout(device, &layout, nullptr, &scene.hzb_layout),
              "Create HZB descriptor layout");
        const std::array<VkDescriptorSetLayout, 2> scene_layouts{descriptor_layout,
                                                                  scene.graphics_layout};
        VkPushConstantRange graphics_push{VK_SHADER_STAGE_VERTEX_BIT |
                                              VK_SHADER_STAGE_FRAGMENT_BIT,
                                          0, sizeof(ScenePush)};
        VkPipelineLayoutCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipeline_info.setLayoutCount = 2;
        pipeline_info.pSetLayouts = scene_layouts.data();
        pipeline_info.pushConstantRangeCount = 1;
        pipeline_info.pPushConstantRanges = &graphics_push;
        check(vkCreatePipelineLayout(device, &pipeline_info, nullptr,
                                     &scene.graphics_pipeline_layout),
              "Create GPU scene pipeline layout");
        VkPushConstantRange compute_push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        pipeline_info.setLayoutCount = 1;
        pipeline_info.pSetLayouts = &scene.cull_layout;
        pipeline_info.pPushConstantRanges = &compute_push;
        check(vkCreatePipelineLayout(device, &pipeline_info, nullptr,
                                     &scene.cull_pipeline_layout),
              "Create GPU cull pipeline layout");
        pipeline_info.pSetLayouts = &scene.hzb_layout;
        check(vkCreatePipelineLayout(device, &pipeline_info, nullptr,
                                     &scene.hzb_pipeline_layout),
              "Create HZB pipeline layout");
        make_scene_pipelines();
        refresh_scene_descriptors();
    }
    void make_scene_pipelines() {
        const auto gpu_shaders = detail::load_gpu_shader_bundle(shader_directory());
        const auto baseline_shaders = detail::load_shader_bundle(shader_directory());
        for (std::size_t i = 0; i < gpu_shaders.size(); ++i)
            if (!scene.shader_layouts[i].empty() &&
                scene.shader_layouts[i] != gpu_shaders[i].layout_fingerprint)
                throw std::runtime_error("GPU shader layout changed; current pipeline preserved");
        VkShaderModule vertex = shader(gpu_shaders[0]);
        VkShaderModule fragment = shader(baseline_shaders[1]);
        VkShaderModule main_cull = shader(gpu_shaders[2]);
        VkShaderModule hzb_shader = scene.hzb_supported ? shader(gpu_shaders[3])
                                                        : VK_NULL_HANDLE;
        VkShaderModule post_cull = shader(gpu_shaders[4]);
        try {
            VkPipelineShaderStageCreateInfo stages[2]{};
            for (auto& stage : stages)
                stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = vertex;
            stages[0].pName = "main";
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = fragment;
            stages[1].pName = "main";
            VkVertexInputBindingDescription binding{0, sizeof(SceneVertex),
                                                    VK_VERTEX_INPUT_RATE_VERTEX};
            const std::array<VkVertexInputAttributeDescription, 4> attributes{{
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, position)},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SceneVertex, normal)},
                {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(SceneVertex, color)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SceneVertex, uv)}}};
            VkPipelineVertexInputStateCreateInfo input{};
            input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            input.vertexBindingDescriptionCount = 1;
            input.pVertexBindingDescriptions = &binding;
            input.vertexAttributeDescriptionCount = attributes.size();
            input.pVertexAttributeDescriptions = attributes.data();
            VkPipelineInputAssemblyStateCreateInfo assembly{};
            assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{};
            viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewport.viewportCount = viewport.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo raster{};
            raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            raster.polygonMode = VK_POLYGON_MODE_FILL;
            raster.cullMode = VK_CULL_MODE_NONE;
            raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            raster.lineWidth = 1;
            VkPipelineMultisampleStateCreateInfo samples{};
            samples.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo depth_state{};
            depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depth_state.depthTestEnable = VK_TRUE;
            depth_state.depthWriteEnable = VK_TRUE;
            depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
            VkPipelineColorBlendAttachmentState blend{};
            blend.colorWriteMask = 15;
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            VkPipelineColorBlendStateCreateInfo color_blend{};
            color_blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            color_blend.attachmentCount = 1;
            color_blend.pAttachments = &blend;
            const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                               VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{};
            dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamic.dynamicStateCount = dynamic_states.size();
            dynamic.pDynamicStates = dynamic_states.data();
            const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
            VkPipelineRenderingCreateInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachmentFormats = &color_format;
            rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
            VkGraphicsPipelineCreateInfo graphics_pipeline{};
            graphics_pipeline.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            graphics_pipeline.pNext = &rendering;
            graphics_pipeline.stageCount = 2;
            graphics_pipeline.pStages = stages;
            graphics_pipeline.pVertexInputState = &input;
            graphics_pipeline.pInputAssemblyState = &assembly;
            graphics_pipeline.pViewportState = &viewport;
            graphics_pipeline.pRasterizationState = &raster;
            graphics_pipeline.pMultisampleState = &samples;
            graphics_pipeline.pDepthStencilState = &depth_state;
            graphics_pipeline.pColorBlendState = &color_blend;
            graphics_pipeline.pDynamicState = &dynamic;
            graphics_pipeline.layout = scene.graphics_pipeline_layout;
            check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphics_pipeline,
                                            nullptr, &scene.graphics_pipeline),
                  "Create GPU scene pipeline");
            auto make_compute = [&](VkShaderModule module, VkPipelineLayout layout,
                                    VkPipeline& output) {
                VkComputePipelineCreateInfo info{};
                info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                info.stage.module = module;
                info.stage.pName = "main";
                info.layout = layout;
                check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr,
                                               &output), "Create GPU visibility compute pipeline");
            };
            make_compute(main_cull, scene.cull_pipeline_layout, scene.cull_pipeline);
            make_compute(post_cull, scene.cull_pipeline_layout, scene.post_pipeline);
            if (scene.hzb_supported)
                make_compute(hzb_shader, scene.hzb_pipeline_layout, scene.hzb_pipeline);
            for (std::size_t i = 0; i < gpu_shaders.size(); ++i)
                scene.shader_layouts[i] = gpu_shaders[i].layout_fingerprint;
        } catch (...) {
            for (auto module : {vertex, fragment, main_cull, hzb_shader, post_cull})
                vkDestroyShaderModule(device, module, nullptr);
            throw;
        }
        for (auto module : {vertex, fragment, main_cull, hzb_shader, post_cull})
            vkDestroyShaderModule(device, module, nullptr);
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
    void upload_scene_buffer(Buffer& buffer, const void* bytes, std::size_t count,
                             VkBufferUsageFlags usage) {
        const VkDeviceSize required = std::max<VkDeviceSize>(16, count);
        if (required > max_storage_buffer_range)
            throw std::overflow_error("GPU scene buffer exceeds maxStorageBufferRange");
        if (buffer.size < required) {
            destroy(buffer);
            buffer = make_buffer(required, usage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                 VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        }
        if (count && bytes) {
            void* mapped{};
            check(vkMapMemory(device, buffer.memory, 0, buffer.size, 0, &mapped),
                  "Map GPU scene buffer");
            std::memcpy(mapped, bytes, count);
            vkUnmapMemory(device, buffer.memory);
        }
    }
    template <typename T>
    void upload_scene_vector(Buffer& buffer, const std::vector<T>& values,
                             VkBufferUsageFlags usage = 0) {
        upload_scene_buffer(buffer, values.data(), values.size() * sizeof(T), usage);
    }
    void update_scene_descriptors(bool occlusion) {
        auto write_buffers = [&](VkDescriptorSet set, std::span<const Buffer* const> buffers,
                                 std::uint32_t first_binding) {
            std::vector<VkDescriptorBufferInfo> infos(buffers.size());
            std::vector<VkWriteDescriptorSet> writes(buffers.size());
            for (std::size_t i = 0; i < buffers.size(); ++i) {
                infos[i] = {buffers[i]->handle, 0, buffers[i]->size};
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = first_binding + static_cast<std::uint32_t>(i);
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo = &infos[i];
            }
            vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        };
        const std::array<const Buffer*, 3> main_graphics{&scene.instances, &scene.main_ids,
                                                          &scene.view};
        const std::array<const Buffer*, 3> post_graphics{&scene.instances, &scene.post_ids,
                                                          &scene.view};
        write_buffers(scene.graphics_main, main_graphics, 0);
        write_buffers(scene.graphics_post, post_graphics, 0);
        const std::array<const Buffer*, 7> main_cull{
            &scene.instances, &scene.candidates, &scene.bins, &scene.main_ids,
            &scene.main_args, &scene.deferred_ids, &scene.deferred_count};
        const std::array<const Buffer*, 7> post_cull{
            &scene.instances, &scene.candidates, &scene.bins, &scene.post_ids,
            &scene.post_args, &scene.deferred_ids, &scene.deferred_count};
        write_buffers(scene.cull_main, main_cull, 0);
        write_buffers(scene.cull_post, post_cull, 0);
        for (auto set : {scene.cull_main, scene.cull_post}) {
            const Image* previous = occlusion ? &scene.hzb[1 - scene.hzb_current] : &shadow;
            const Image* current = occlusion ? &scene.hzb[scene.hzb_current] : &shadow;
            const VkImageLayout previous_layout = occlusion ? VK_IMAGE_LAYOUT_GENERAL
                                                             : VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL;
            const VkImageLayout current_layout = previous == &shadow
                ? VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo images[] = {
                {VK_NULL_HANDLE, previous->view, previous_layout},
                {VK_NULL_HANDLE, current->view, current_layout}};
            VkWriteDescriptorSet writes[2]{};
            for (std::uint32_t i = 0; i < 2; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = 7 + i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[i].pImageInfo = &images[i];
            }
            vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
            const std::array<const Buffer*, 1> view{&scene.view};
            write_buffers(set, view, 9);
        }
    }
    void scene_barrier(VkPipelineStageFlags2 source_stages, VkAccessFlags2 source_access,
                       VkPipelineStageFlags2 destination_stages, VkAccessFlags2 destination_access) {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = source_stages;
        barrier.srcAccessMask = source_access;
        barrier.dstStageMask = destination_stages;
        barrier.dstAccessMask = destination_access;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
    }
    Bounds world_bounds(const std::shared_ptr<const Mesh>& mesh, const Mat4& model) {
        auto found = bounds_cache.find(mesh.get());
        if (found == bounds_cache.end() || found->second.owner.lock() != mesh) {
            auto local = local_bounds(*mesh);
            found = bounds_cache.insert_or_assign(mesh.get(), CachedBounds{mesh, local}).first;
        }
        return transformed_bounds(found->second.local, model);
    }
    bool is_opaque(const std::shared_ptr<const Texture>& texture) {
        if (!texture)
            return true;
        auto found = opacity_cache.find(texture.get());
        if (found == opacity_cache.end() || found->second.owner.lock() != texture ||
            found->second.revision != texture->revision) {
            const bool opaque = opaque_texture(texture.get());
            found = opacity_cache.insert_or_assign(
                texture.get(), CachedOpacity{texture, texture->revision, opaque}).first;
        }
        return found->second.opaque;
    }
    void render(const Snapshot& snapshot) {
        auto start = std::chrono::steady_clock::now();
        statistics.draw_calls = statistics.culled_meshes = statistics.gpu_label_count = 0;
        statistics.gpu_bins = statistics.gpu_visible_instances =
            statistics.gpu_frustum_rejected = statistics.gpu_occlusion_deferred =
                statistics.gpu_post_visible = 0;
        statistics.lod_counts = {};
        statistics.visibility_counters_valid = false;
        for (auto it = bounds_cache.begin(); it != bounds_cache.end();)
            it = it->second.owner.expired() ? bounds_cache.erase(it) : std::next(it);
        for (auto it = opacity_cache.begin(); it != opacity_cache.end();)
            it = it->second.owner.expired() ? opacity_cache.erase(it) : std::next(it);
        const bool gpu_active = scene.available &&
            config.visibility_mode != VisibilityMode::Direct;
        const bool occlusion = gpu_active && scene.hzb_supported && scene.hzb_mips &&
            config.visibility_mode == VisibilityMode::GpuOcclusion;
        statistics.gpu_visibility_active = gpu_active;
        statistics.hzb_valid = false;
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
        std::array<float, 4> scene_viewport{0, 0, float(width), float(height)};
        if (snapshot.scene_rect[2] > 0 && snapshot.scene_rect[3] > 0) {
            const auto& rect = snapshot.scene_rect;
            const float x = std::clamp(rect[0], 0.f, float(width));
            const float y = std::clamp(rect[1], 0.f, float(height));
            scene_viewport = {x, y, std::max(0.f, std::min(rect[2], float(width) - x)),
                              std::max(0.f, std::min(rect[3], float(height) - y))};
        }
        const std::string view_id = snapshot.view_id.empty() ? "default" : snapshot.view_id;
        const bool history_compatible = occlusion && scene.hzb_history_valid &&
            !snapshot.camera_cut && scene.previous_view_id == view_id &&
            scene.previous_viewport == scene_viewport &&
            scene.previous_projection == snapshot.projection;
        if (!history_compatible)
            instance_tracker.invalidate_view(view_id);
        if (occlusion)
            scene.hzb_current = history_compatible ? 1 - scene.hzb_current : 0;
        PreparedScene gpu_frame;
        gpu_frame.view.current_vp = snapshot.view_projection;
        gpu_frame.view.previous_vp = scene.previous_vp;
        gpu_frame.view.viewport = scene_viewport;
        gpu_frame.view.previous_viewport = scene.previous_viewport;
        if (scene.hzb_mips) {
            const auto dimensions = std::array<std::uint32_t, 4>{
                std::bit_ceil(width), std::bit_ceil(height), scene.hzb_mips, 0};
            gpu_frame.view.hzb_dimensions = dimensions;
            gpu_frame.view.previous_hzb_dimensions = dimensions;
        }
        gpu_frame.view.flags[0] = history_compatible ? 1 : 0;
        statistics.hzb_valid = history_compatible;
        struct SelectedDraw {
            const DrawItem* source;
            std::shared_ptr<const Mesh> mesh;
            bool gpu{};
            bool opaque{};
        };
        std::vector<SelectedDraw> selected_draws;
        selected_draws.reserve(snapshot.draws.size());
        struct BuildingBin {
            const Mesh* mesh{};
            const Texture* texture{};
            std::uint32_t first_vertex{}, vertex_count{};
            std::vector<std::uint32_t> instances;
        };
        std::vector<BuildingBin> building_bins;
        std::unordered_map<const Mesh*, std::pair<std::uint32_t, std::uint32_t>> mesh_ranges;
        std::unordered_map<std::string, std::size_t> current_lods;
        for (const auto& item : snapshot.draws) {
            if (!item.mesh || item.mesh->vertices.empty())
                continue;
            const auto source_bounds = world_bounds(item.mesh, item.model);
            std::vector<float> thresholds;
            std::vector<std::uint8_t> available;
            std::shared_ptr<const Mesh> selected_mesh = item.mesh;
            std::size_t lod = 0;
            if (!item.lod_meshes.empty()) {
                available.push_back(1);
                for (std::size_t level = 0; level < item.lod_meshes.size(); ++level) {
                    thresholds.push_back(192.f / float(std::uint32_t(1) <<
                                                      std::min<std::size_t>(level, 20)));
                    available.push_back(item.lod_meshes[level] &&
                                                !item.lod_meshes[level]->vertices.empty() ? 1 : 0);
                }
                auto previous = previous_lods.find(item.instance_key);
                lod = select_lod(projected_pixels(source_bounds, snapshot.view_projection,
                                                  scene_viewport),
                                 previous != previous_lods.end() && !item.instance_key.empty()
                                     ? previous->second : SIZE_MAX,
                                 thresholds, .12f, available);
                if (lod)
                    selected_mesh = item.lod_meshes[lod - 1];
            }
            statistics.lod_counts[std::min<std::size_t>(lod, 3)]++;
            if (!item.instance_key.empty())
                current_lods[item.instance_key] = lod;
            const auto bounds = selected_mesh == item.mesh
                ? source_bounds : world_bounds(selected_mesh, item.model);
            InstanceUpdate previous{};
            if (!item.instance_key.empty())
                previous = instance_tracker.update(item.instance_key, selected_mesh,
                                                   item.model, bounds, view_id);
            const bool opaque = item.color[3] >= 1.f && is_opaque(item.texture);
            const bool eligible = gpu_active && opaque;
            selected_draws.push_back({&item, selected_mesh, eligible, opaque});
            if (!eligible)
                continue;
            auto [range_it, inserted] = mesh_ranges.try_emplace(selected_mesh.get());
            if (inserted) {
                const std::size_t first = gpu_frame.vertices.size();
                auto emit = [&](std::uint32_t index) {
                    if (index >= selected_mesh->vertices.size())
                        throw std::out_of_range("Mesh index outside vertex range");
                    const auto& v = selected_mesh->vertices[index];
                    SceneVertex gpu_vertex{};
                    std::copy(v.position.begin(), v.position.end(), gpu_vertex.position);
                    std::copy(v.normal.begin(), v.normal.end(), gpu_vertex.normal);
                    std::copy(v.color.begin(), v.color.end(), gpu_vertex.color);
                    std::copy(v.uv.begin(), v.uv.end(), gpu_vertex.uv);
                    gpu_frame.vertices.push_back(gpu_vertex);
                };
                if (selected_mesh->indices.empty())
                    for (std::uint32_t i = 0; i < selected_mesh->vertices.size(); ++i)
                        emit(i);
                else
                    for (auto i : selected_mesh->indices)
                        emit(i);
                const auto count = gpu_frame.vertices.size() - first;
                if (count % 3 || first > UINT32_MAX || count > UINT32_MAX)
                    throw std::invalid_argument("GPU scene mesh must fit complete triangles");
                range_it->second = {static_cast<std::uint32_t>(first),
                                    static_cast<std::uint32_t>(count)};
            }
            if (!range_it->second.second)
                continue;
            auto bin_it = std::find_if(building_bins.begin(), building_bins.end(),
                                       [&](const BuildingBin& bin) {
                                           return bin.mesh == selected_mesh.get() &&
                                                  bin.texture == (item.texture
                                                      ? item.texture.get() : white.get());
                                       });
            if (bin_it == building_bins.end()) {
                const auto [first, count] = range_it->second;
                building_bins.push_back({selected_mesh.get(), item.texture
                                              ? item.texture.get() : white.get(),
                                         first, count, {}});
                bin_it = std::prev(building_bins.end());
            }
            const auto& m = item.model;
            Vec3 a{m[0], m[1], m[2]}, b{m[4], m[5], m[6]}, c{m[8], m[9], m[10]};
            auto cross = [](Vec3 x, Vec3 y) {
                return Vec3{x[1] * y[2] - x[2] * y[1],
                            x[2] * y[0] - x[0] * y[2],
                            x[0] * y[1] - x[1] * y[0]};
            };
            const auto ca = cross(b, c), cb = cross(c, a), cc = cross(a, b);
            const float determinant = a[0] * ca[0] + a[1] * ca[1] + a[2] * ca[2];
            const float sign = determinant < 0 ? -1.f : 1.f;
            SceneInstance instance{};
            instance.model = m;
            for (int row = 0; row < 3; ++row) {
                auto& normal = row == 0 ? instance.normal0
                             : row == 1 ? instance.normal1 : instance.normal2;
                normal = {ca[row] * sign, cb[row] * sign, cc[row] * sign, 0};
            }
            instance.color = item.color;
            instance.material = {item.roughness, item.metallic, 0, 0};
            for (int axis = 0; axis < 3; ++axis) {
                instance.center_extent[axis] = (bounds.min[axis] + bounds.max[axis]) * .5f;
                instance.half_extent[axis] = (bounds.max[axis] - bounds.min[axis]) * .5f;
                instance.previous_center_extent[axis] =
                    (previous.previous_bounds.min[axis] +
                     previous.previous_bounds.max[axis]) * .5f;
                instance.previous_half_extent[axis] =
                    (previous.previous_bounds.max[axis] -
                     previous.previous_bounds.min[axis]) * .5f;
            }
            instance.metadata[0] = previous.previous_valid && history_compatible ? 1 : 0;
            if (gpu_frame.instances.size() >= UINT32_MAX)
                throw std::overflow_error("GPU scene instance capacity exceeded");
            bin_it->instances.push_back(static_cast<std::uint32_t>(gpu_frame.instances.size()));
            gpu_frame.instances.push_back(instance);
        }
        previous_lods = std::move(current_lods);
        for (const auto& bin : building_bins) {
            if (gpu_frame.candidates.size() > UINT32_MAX - bin.instances.size())
                throw std::overflow_error("GPU scene candidate capacity exceeded");
            const auto first_candidate = static_cast<std::uint32_t>(gpu_frame.candidates.size());
            const auto visible_base = first_candidate;
            const auto bin_index = static_cast<std::uint32_t>(gpu_frame.bins.size());
            for (auto id : bin.instances)
                gpu_frame.candidates.push_back({id, bin_index, 0, 0});
            gpu_frame.bins.push_back({first_candidate,
                                      static_cast<std::uint32_t>(bin.instances.size()),
                                      visible_base,
                                      static_cast<std::uint32_t>(bin.instances.size())});
            gpu_frame.commands.push_back({bin.vertex_count, 0, bin.first_vertex, 0});
            gpu_frame.textures.push_back(bin.texture);
        }
        gpu_frame.candidate_count = static_cast<std::uint32_t>(gpu_frame.candidates.size());
        std::vector<GpuVertex> data;
        std::vector<Batch> scene_batches, transparent_batches, shadow_batches,
            sprite_batches, ui_batches;
        for (const auto& selected : selected_draws) {
            const auto& item = *selected.source;
            if (selected.gpu && !item.cast_shadow)
                continue;
            auto first = data.size();
            const auto& mesh = *selected.mesh;
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
            if (!selected.gpu) {
                if (outside(data, first))
                    ++statistics.culled_meshes;
                else if (gpu_active && !selected.opaque)
                    transparent_batches.push_back(batch);
                else
                    scene_batches.push_back(batch);
            }
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
        statistics.vertices = static_cast<std::uint32_t>(data.size() + gpu_frame.vertices.size());
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
        if (gpu_active) {
            statistics.gpu_bins = static_cast<std::uint32_t>(gpu_frame.bins.size());
            upload_scene_vector(scene.vertices, gpu_frame.vertices, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            upload_scene_vector(scene.instances, gpu_frame.instances);
            upload_scene_vector(scene.candidates, gpu_frame.candidates);
            upload_scene_vector(scene.bins, gpu_frame.bins);
            upload_scene_buffer(scene.view, &gpu_frame.view, sizeof(gpu_frame.view), 0);
            upload_scene_buffer(scene.main_ids, nullptr,
                                gpu_frame.candidate_count * sizeof(std::uint32_t), 0);
            upload_scene_buffer(scene.post_ids, nullptr,
                                gpu_frame.candidate_count * sizeof(std::uint32_t), 0);
            upload_scene_vector(scene.main_args, gpu_frame.commands,
                                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
            upload_scene_vector(scene.post_args, gpu_frame.commands,
                                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
            upload_scene_buffer(scene.deferred_ids, nullptr,
                                gpu_frame.candidate_count * sizeof(std::uint32_t), 0);
            const std::uint32_t zero = 0;
            upload_scene_buffer(scene.deferred_count, &zero, sizeof(zero), 0);
            update_scene_descriptors(occlusion);
        }
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
        std::uint32_t timestamp_cursor = 0;
        std::vector<std::string> timestamp_labels;
        if (timestamp_pool) {
            vkCmdResetQueryPool(command, timestamp_pool, 0, 12);
            vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                                 timestamp_pool, timestamp_cursor++);
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
        auto set_scene_viewport = [&] {
            VkViewport viewport{scene_viewport[0], scene_viewport[1],
                                scene_viewport[2], scene_viewport[3], 0, 1};
            VkRect2D scissor{{static_cast<int>(scene_viewport[0]),
                              static_cast<int>(scene_viewport[1])},
                             {static_cast<std::uint32_t>(scene_viewport[2]),
                              static_cast<std::uint32_t>(scene_viewport[3])}};
            vkCmdSetViewport(command, 0, 1, &viewport);
            vkCmdSetScissor(command, 0, 1, &scissor);
        };
        auto draw_transparent = [&] {
            if (transparent_batches.empty())
                return;
            vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(push), &push);
            for (auto batch : transparent_batches) {
                auto descriptor = textures.at(batch.texture).descriptor;
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipeline_layout, 0, 1, &descriptor, 0, nullptr);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
        };
        auto draw_sprites_and_ui = [&] {
            vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, sprite_pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            for (auto batch : sprite_batches) {
                auto descriptor = textures.at(batch.texture).descriptor;
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                        0, 1, &descriptor, 0, nullptr);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
            set_viewport(width, height);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline);
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
        };
        auto dispatch_scene = [&](VkPipeline compute_pipeline, VkDescriptorSet descriptor) {
            if (!gpu_frame.candidate_count)
                return;
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                              compute_pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    scene.cull_pipeline_layout, 0, 1, &descriptor, 0,
                                    nullptr);
            const std::uint64_t max_threads = std::uint64_t(max_compute_groups_x) * 64;
            for (std::uint64_t base = 0; base < gpu_frame.candidate_count;
                 base += max_threads) {
                const auto remaining = std::uint64_t(gpu_frame.candidate_count) - base;
                const auto groups = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(max_compute_groups_x,
                                            (remaining + 63) / 64));
                const std::array<std::uint32_t, 4> parameters{
                    gpu_frame.candidate_count, gpu_frame.candidate_count,
                    static_cast<std::uint32_t>(base), 0};
                vkCmdPushConstants(command, scene.cull_pipeline_layout,
                                   VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(parameters), parameters.data());
                vkCmdDispatch(command, groups, 1, 1);
            }
        };
        RenderGraph graph;
        auto add_pass = [&](std::string name, std::vector<std::string> reads,
                            std::vector<std::string> writes, RenderGraph::Callback callback) {
            const auto label_name = name;
            graph.add(std::move(name), std::move(reads), std::move(writes),
                      [this, &timestamp_cursor, &timestamp_labels, label_name,
                       callback = std::move(callback)] {
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
                          if (timestamp_pool && timestamp_cursor < 12) {
                              vkCmdWriteTimestamp2(command,
                                                   VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                                                   timestamp_pool, timestamp_cursor++);
                              timestamp_labels.push_back(label_name);
                          }
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
        if (gpu_active)
            add_pass("MainCull", {"shadow"},
                     {"main_indirect", "main_visible", "deferred_ids"}, [&] {
                if (occlusion) {
                    for (auto& pyramid : scene.hzb) {
                        if (pyramid.layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                            transition(command, pyramid, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_IMAGE_ASPECT_COLOR_BIT);
                            VkClearColorValue far_depth{};
                            far_depth.float32[0] = 1.f;
                            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                          pyramid.mip_levels, 0, 1};
                            vkCmdClearColorImage(command, pyramid.handle,
                                                 VK_IMAGE_LAYOUT_GENERAL, &far_depth, 1,
                                                 &range);
                        }
                    }
                    scene_barrier(VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                }
                scene_barrier(VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                if (gpu_frame.candidate_count) {
                    dispatch_scene(scene.cull_pipeline, scene.cull_main);
                    scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                }
            });
        add_pass(occlusion ? "MainRaster" : "ForwardAndUI",
                 gpu_active ? std::vector<std::string>{"shadow", "main_indirect", "main_visible"}
                            : std::vector<std::string>{"shadow"},
                 {"color", "depth"}, [&] {
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
            da.storeOp = occlusion ? VK_ATTACHMENT_STORE_OP_STORE
                                   : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            da.clearValue.depthStencil = {1, 0};
            VkRenderingInfo rendering{};
            rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            rendering.renderArea = {{0, 0}, {width, height}};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &ca;
            rendering.pDepthAttachment = &da;
            vkCmdBeginRendering(command, &rendering);
            set_scene_viewport();
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            vkCmdPushConstants(command, pipeline_layout,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1,
                                    &white_descriptor, 0, nullptr);
            if (gpu_active && !gpu_frame.bins.empty()) {
                VkDeviceSize scene_offset{};
                vkCmdBindVertexBuffers(command, 0, 1, &scene.vertices.handle, &scene_offset);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  scene.graphics_pipeline);
                for (std::uint32_t bin = 0; bin < gpu_frame.bins.size(); ++bin) {
                    auto descriptor = textures.at(gpu_frame.textures[bin]).descriptor;
                    const std::array<VkDescriptorSet, 2> sets{descriptor,
                                                               scene.graphics_main};
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                            scene.graphics_pipeline_layout, 0, sets.size(),
                                            sets.data(), 0, nullptr);
                    ScenePush draw_push{push, {gpu_frame.bins[bin].visible_base, 0, 0, 0}};
                    vkCmdPushConstants(command, scene.graphics_pipeline_layout,
                                       VK_SHADER_STAGE_VERTEX_BIT |
                                           VK_SHADER_STAGE_FRAGMENT_BIT,
                                       0, sizeof(draw_push), &draw_push);
                    vkCmdDrawIndirect(command, scene.main_args.handle,
                                      VkDeviceSize(bin) * sizeof(SceneIndirect), 1,
                                      sizeof(SceneIndirect));
                    ++statistics.draw_calls;
                }
                vkCmdBindVertexBuffers(command, 0, 1, &vertices.handle, &offset);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdPushConstants(command, pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT |
                                       VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof(push), &push);
            }
            for (auto batch : scene_batches) {
                auto descriptor = textures.at(batch.texture).descriptor;
                vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                        0, 1, &descriptor, 0, nullptr);
                vkCmdDraw(command, batch.count, 1, batch.first, 0);
                ++statistics.draw_calls;
            }
            if (!occlusion) {
                draw_transparent();
                draw_sprites_and_ui();
            }
            vkCmdEndRendering(command);
        });
        if (occlusion) {
            add_pass("BuildCurrentHZB", {"depth"}, {"current_hzb"}, [&] {
                transition(command, depth, VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
                auto& pyramid = scene.hzb[scene.hzb_current];
                transition(command, pyramid, VK_IMAGE_LAYOUT_GENERAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                  scene.hzb_pipeline);
                const auto padded_width = std::bit_ceil(width);
                const auto padded_height = std::bit_ceil(height);
                for (std::uint32_t mip = 0; mip < scene.hzb_mips; ++mip) {
                    const std::uint32_t source_width = mip == 0 ? width
                        : std::max(1u, padded_width >> (mip - 1));
                    const std::uint32_t source_height = mip == 0 ? height
                        : std::max(1u, padded_height >> (mip - 1));
                    const std::uint32_t output_width =
                        std::max(1u, padded_width >> mip);
                    const std::uint32_t output_height =
                        std::max(1u, padded_height >> mip);
                    const auto descriptor = scene.hzb_sets[scene.hzb_current][mip];
                    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                                            scene.hzb_pipeline_layout, 0, 1,
                                            &descriptor, 0, nullptr);
                    const std::array<std::uint32_t, 4> dimensions{
                        source_width, source_height, output_width, output_height};
                    vkCmdPushConstants(command, scene.hzb_pipeline_layout,
                                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                       sizeof(dimensions), dimensions.data());
                    vkCmdDispatch(command, (output_width + 7) / 8,
                                  (output_height + 7) / 8, 1);
                    scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                }
            });
            add_pass("PostCull", {"deferred_ids", "current_hzb"},
                     {"post_indirect", "post_visible"}, [&] {
                scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                              VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                              VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                if (gpu_frame.candidate_count) {
                    dispatch_scene(scene.post_pipeline, scene.cull_post);
                    scene_barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT |
                                      VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
                }
            });
            add_pass("PostRasterAndUI", {"color", "depth", "post_indirect", "post_visible"},
                     {"color", "depth"}, [&] {
                transition(command, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_COLOR_BIT);
                transition(command, depth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_ASPECT_DEPTH_BIT);
                VkRenderingAttachmentInfo ca{};
                ca.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                ca.imageView = color.view;
                ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                VkRenderingAttachmentInfo da{};
                da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                da.imageView = depth.view;
                da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
                da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
                VkRenderingInfo rendering{};
                rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                rendering.renderArea = {{0, 0}, {width, height}};
                rendering.layerCount = 1;
                rendering.colorAttachmentCount = 1;
                rendering.pColorAttachments = &ca;
                rendering.pDepthAttachment = &da;
                vkCmdBeginRendering(command, &rendering);
                set_scene_viewport();
                if (!gpu_frame.bins.empty()) {
                    VkDeviceSize scene_offset{};
                    vkCmdBindVertexBuffers(command, 0, 1, &scene.vertices.handle,
                                           &scene_offset);
                    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      scene.graphics_pipeline);
                    for (std::uint32_t bin = 0; bin < gpu_frame.bins.size(); ++bin) {
                        auto descriptor = textures.at(gpu_frame.textures[bin]).descriptor;
                        const std::array<VkDescriptorSet, 2> sets{descriptor,
                                                                   scene.graphics_post};
                        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                scene.graphics_pipeline_layout, 0,
                                                sets.size(), sets.data(), 0, nullptr);
                        ScenePush draw_push{push, {gpu_frame.bins[bin].visible_base,
                                                   0, 0, 0}};
                        vkCmdPushConstants(command, scene.graphics_pipeline_layout,
                                           VK_SHADER_STAGE_VERTEX_BIT |
                                               VK_SHADER_STAGE_FRAGMENT_BIT,
                                           0, sizeof(draw_push), &draw_push);
                        vkCmdDrawIndirect(command, scene.post_args.handle,
                                          VkDeviceSize(bin) * sizeof(SceneIndirect), 1,
                                          sizeof(SceneIndirect));
                        ++statistics.draw_calls;
                    }
                }
                draw_transparent();
                draw_sprites_and_ui();
                vkCmdEndRendering(command);
            });
        }
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
        submit(swap_index.has_value());
        if (timestamp_pool) {
            std::array<std::uint64_t, 12> stamps{};
            check(vkGetQueryPoolResults(device, timestamp_pool, 0, timestamp_cursor,
                                        timestamp_cursor * sizeof(std::uint64_t), stamps.data(),
                                        sizeof(std::uint64_t),
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                  "Read GPU timestamps");
            auto milliseconds = [&](std::uint64_t before, std::uint64_t after) {
                auto delta = after - before;
                if (timestamp_bits < 64)
                    delta &= (std::uint64_t(1) << timestamp_bits) - 1;
                return double(delta) * timestamp_period / 1000000.0;
            };
            statistics.gpu_ms = milliseconds(stamps[0], stamps[timestamp_cursor - 1]);
            statistics.gpu_main_cull_ms = statistics.gpu_main_raster_ms =
                statistics.gpu_hzb_ms = statistics.gpu_post_cull_ms =
                    statistics.gpu_post_raster_ms = 0;
            for (std::size_t i = 0; i < timestamp_labels.size(); ++i) {
                const auto elapsed = milliseconds(stamps[i], stamps[i + 1]);
                const auto& label = timestamp_labels[i];
                if (label == "MainCull") statistics.gpu_main_cull_ms = elapsed;
                else if (label == "MainRaster" || label == "ForwardAndUI")
                    statistics.gpu_main_raster_ms = elapsed;
                else if (label == "BuildCurrentHZB") statistics.gpu_hzb_ms = elapsed;
                else if (label == "PostCull") statistics.gpu_post_cull_ms = elapsed;
                else if (label == "PostRasterAndUI")
                    statistics.gpu_post_raster_ms = elapsed;
            }
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
        if (gpu_active && config.visibility_diagnostics) {
            void* counts{};
            check(vkMapMemory(device, scene.main_args.memory, 0, scene.main_args.size, 0,
                              &counts), "Read GPU scene indirect counts");
            auto* commands = static_cast<const SceneIndirect*>(counts);
            std::uint32_t main_visible{};
            for (std::size_t i = 0; i < gpu_frame.commands.size(); ++i)
                main_visible += commands[i].instance_count;
            vkUnmapMemory(device, scene.main_args.memory);
            check(vkMapMemory(device, scene.deferred_count.memory, 0, scene.deferred_count.size,
                              0, &counts), "Read GPU deferred count");
            statistics.gpu_occlusion_deferred = *static_cast<const std::uint32_t*>(counts);
            vkUnmapMemory(device, scene.deferred_count.memory);
            if (occlusion && statistics.gpu_occlusion_deferred) {
                check(vkMapMemory(device, scene.post_args.memory, 0, scene.post_args.size, 0,
                                  &counts), "Read GPU post counts");
                commands = static_cast<const SceneIndirect*>(counts);
                for (std::size_t i = 0; i < gpu_frame.commands.size(); ++i)
                    statistics.gpu_post_visible += commands[i].instance_count;
                vkUnmapMemory(device, scene.post_args.memory);
            }
            statistics.gpu_visible_instances = main_visible + statistics.gpu_post_visible;
            statistics.gpu_frustum_rejected = gpu_frame.candidate_count -
                std::min(gpu_frame.candidate_count,
                         main_visible + statistics.gpu_occlusion_deferred);
            statistics.culled_meshes += statistics.gpu_frustum_rejected;
            statistics.visibility_counters_valid = true;
        }
        instance_tracker.finish_frame();
        scene.previous_vp = snapshot.view_projection;
        scene.previous_projection = snapshot.projection;
        scene.previous_viewport = scene_viewport;
        scene.previous_view_id = view_id;
        scene.hzb_history_valid = occlusion;
        ++statistics.frame;
        statistics.gpu_allocated_bytes = vertices.allocation_size + readback.allocation_size +
                                         color.allocation_size + depth.allocation_size +
                                         shadow.allocation_size;
        statistics.texture_count = static_cast<std::uint32_t>(textures.size());
        for (const auto& [_, texture] : textures)
            statistics.gpu_allocated_bytes += texture.image.allocation_size;
        if (scene.available) {
            for (const Buffer* buffer : {&scene.vertices, &scene.instances, &scene.candidates,
                                         &scene.bins, &scene.view, &scene.main_ids,
                                         &scene.post_ids, &scene.main_args, &scene.post_args,
                                         &scene.deferred_ids, &scene.deferred_count})
                statistics.gpu_allocated_bytes += buffer->allocation_size;
            for (const auto& image : scene.hzb)
                statistics.gpu_allocated_bytes += image.allocation_size;
        }
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
    auto previous_layout_fingerprints = r.shader_layouts;
    auto previous_gpu_fingerprints = r.scene.shader_layouts;
    auto previous_gpu = r.scene.graphics_pipeline;
    auto previous_cull = r.scene.cull_pipeline;
    auto previous_post = r.scene.post_pipeline;
    auto previous_hzb = r.scene.hzb_pipeline;
    r.pipeline_layout = {};
    r.pipeline = {};
    r.ui_pipeline = {};
    r.shadow_pipeline = {};
    r.sprite_pipeline = {};
    r.scene.graphics_pipeline = r.scene.cull_pipeline = r.scene.post_pipeline =
        r.scene.hzb_pipeline = {};
    try {
        r.make_pipelines();
        if (r.scene.graphics_pipeline_layout)
            r.make_scene_pipelines();
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
        for (auto pipeline : {r.scene.graphics_pipeline, r.scene.cull_pipeline,
                              r.scene.post_pipeline, r.scene.hzb_pipeline})
            if (pipeline)
                vkDestroyPipeline(r.device, pipeline, nullptr);
        r.pipeline_layout = previous_layout;
        r.pipeline = previous;
        r.ui_pipeline = previous_ui;
        r.shadow_pipeline = previous_shadow;
        r.sprite_pipeline = previous_sprite;
        r.scene.graphics_pipeline = previous_gpu;
        r.scene.cull_pipeline = previous_cull;
        r.scene.post_pipeline = previous_post;
        r.scene.hzb_pipeline = previous_hzb;
        r.shader_layouts = previous_layout_fingerprints;
        r.scene.shader_layouts = previous_gpu_fingerprints;
        error = exception.what();
        return false;
    }
    vkDestroyPipeline(r.device, previous, nullptr);
    vkDestroyPipeline(r.device, previous_ui, nullptr);
    vkDestroyPipeline(r.device, previous_shadow, nullptr);
    vkDestroyPipeline(r.device, previous_sprite, nullptr);
    for (auto pipeline : {previous_gpu, previous_cull, previous_post, previous_hzb})
        if (pipeline)
            vkDestroyPipeline(r.device, pipeline, nullptr);
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
void Renderer::set_visibility_mode(VisibilityMode mode) {
    if (impl_->config.visibility_mode == mode)
        return;
    if (mode != VisibilityMode::Direct && impl_->scene.available &&
        !impl_->scene.graphics_layout) {
        try {
            impl_->make_scene_descriptors_and_pipelines();
        } catch (...) {
            impl_->destroy_scene_interfaces();
            throw;
        }
    }
    impl_->config.visibility_mode = mode;
    if (mode == VisibilityMode::GpuOcclusion && impl_->scene.hzb_supported &&
        !impl_->scene.hzb[0].handle)
        impl_->make_targets();
    impl_->scene.hzb_history_valid = false;
    impl_->instance_tracker.invalidate_view(impl_->scene.previous_view_id);
}
VisibilityMode Renderer::visibility_mode() const {
    return impl_->config.visibility_mode;
}
void Renderer::set_visibility_diagnostics(bool enabled) {
    impl_->config.visibility_diagnostics = enabled;
}
std::optional<HzbDebugImage> Renderer::hzb_debug_image(std::uint32_t mip) {
    auto& renderer = *impl_;
    if (!renderer.scene.hzb_history_valid || !renderer.scene.hzb_supported)
        return std::nullopt;
    if (mip >= renderer.scene.hzb_mips)
        throw std::out_of_range("HZB debug mip outside pyramid");
    const std::uint32_t w = std::max(1u, std::bit_ceil(renderer.width) >> mip);
    const std::uint32_t h = std::max(1u, std::bit_ceil(renderer.height) >> mip);
    auto staging = renderer.make_buffer(VkDeviceSize(w) * h * sizeof(float),
                                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    try {
        renderer.begin();
        auto& pyramid = renderer.scene.hzb[renderer.scene.hzb_current];
        renderer.transition(renderer.command, pyramid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(renderer.command, pyramid.handle,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.handle, 1,
                               &copy);
        renderer.transition(renderer.command, pyramid, VK_IMAGE_LAYOUT_GENERAL,
                            VK_IMAGE_ASPECT_COLOR_BIT);
        renderer.submit();
        HzbDebugImage result;
        result.width = w;
        result.height = h;
        result.rgba.resize(std::size_t(w) * h * 4);
        void* mapped{};
        check(vkMapMemory(renderer.device, staging.memory, 0, staging.size, 0, &mapped),
              "Map HZB debug image");
        const auto* depth = static_cast<const float*>(mapped);
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
            const float value = std::isfinite(depth[i]) ? std::clamp(depth[i], 0.f, 1.f) : 1.f;
            const auto gray = static_cast<std::uint8_t>(std::lround(std::pow(value, 32.f) * 255));
            result.rgba[4 * i] = result.rgba[4 * i + 1] =
                result.rgba[4 * i + 2] = gray;
            result.rgba[4 * i + 3] = 255;
        }
        vkUnmapMemory(renderer.device, staging.memory);
        renderer.destroy(staging);
        return result;
    } catch (...) {
        renderer.destroy(staging);
        throw;
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
