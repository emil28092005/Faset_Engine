// GPU-visible opaque scene. All host records use 16-byte lanes; reflected strides
// are validated before pipelines are created. Sprites/UI and shadow caster selection
// remain independent of camera culling.
struct GpuSceneVertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};
struct GpuSceneOutput {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float2 material : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
struct InstanceRecord {
    column_major float4x4 model;  // 0..63
    float4 normalRow0;            // 64..79, inverse-transpose 3x3
    float4 normalRow1;            // 80..95
    float4 normalRow2;            // 96..111
    float4 color;                 // 112..127
    float4 material;              // 128..143: roughness, metallic, texture flags
    float4 currentCenter;         // 144..159: world AABB center
    float4 currentExtent;         // 160..175: world AABB half extents
    float4 previousCenter;        // 176..191
    float4 previousExtent;        // 192..207
    uint4 metadata;               // 208..223: x bits 0=HZB, 1=temporal prior valid; y=stableSlot,
                                  // z=generation low 32, w=generation high 32 (zero = untracked)
};
struct ViewRecord {
    column_major float4x4 currentViewProjection;  // 0..63
    column_major float4x4 previousViewProjection; // 64..127
    float4 currentViewport;   // 128..143: x/y/width/height in target pixels
    float4 previousViewport;  // 144..159
    uint4 currentHzbSize;     // 160..175: width/height/mipCount/reserved
    uint4 previousHzbSize;    // 176..191
    uint4 flags;              // 192..207: x=historyValid
};
struct BinRecord {
    uint candidateFirst, candidateCount, visibleBase, capacity;
};
struct Candidate {
    uint instanceId, binIndex, flags, reserved;
};
// Exactly VkDrawIndirectCommand: vertexCount, instanceCount, firstVertex, firstInstance.
struct IndirectArgs {
    uint vertexCount, instanceCount, firstVertex, firstInstance;
};

#if defined(FASET_GPU_GRAPHICS)
struct GpuFrameParameters {
    column_major float4x4 lightViewProjection; // same first 96 bytes as baseline fragment
    float4 lightDirection;
    float4 eye;
    uint4 drawInfo; // x=visible ID range base; firstInstance is always zero
};
[[vk::push_constant]] ConstantBuffer<GpuFrameParameters> gpuFrame;
[[vk::binding(0,2)]] StructuredBuffer<InstanceRecord> gfxInstances;
[[vk::binding(1,2)]] StructuredBuffer<uint> gfxVisibleIds;
[[vk::binding(2,2)]] StructuredBuffer<ViewRecord> gfxViews;

// All indirect commands use firstInstance=0. The raw Vulkan index avoids the
// BaseInstance read that Slang adds for SV_InstanceID (DrawParameters feature).
[shader("vertex")]
GpuSceneOutput gpuVertexMain(GpuSceneVertex vertex, uint drawInstance : SV_VulkanInstanceID) {
    InstanceRecord instance = gfxInstances[gfxVisibleIds[gpuFrame.drawInfo.x + drawInstance]];
    float4 world = mul(instance.model, float4(vertex.position, 1));
    GpuSceneOutput output;
    output.position = mul(gfxViews[0].currentViewProjection, world);
    output.world = world.xyz;
    float3 normal = float3(dot(instance.normalRow0.xyz, vertex.normal),
                           dot(instance.normalRow1.xyz, vertex.normal),
                           dot(instance.normalRow2.xyz, vertex.normal));
    float normalLength = length(normal);
    output.normal = normalLength > 1e-8 ? normal / normalLength : float3(0, 0, 0);
    output.color = vertex.color * instance.color;
    output.material = instance.material.xy;
    output.uv = vertex.uv;
    return output;
}

[shader("vertex")]
float4 gpuShadowMain(GpuSceneVertex vertex, uint drawInstance : SV_VulkanInstanceID) : SV_Position {
    InstanceRecord instance = gfxInstances[gfxVisibleIds[gpuFrame.drawInfo.x + drawInstance]];
    return mul(gpuFrame.lightViewProjection, mul(instance.model, float4(vertex.position, 1)));
}

#elif defined(FASET_GPU_CULL)
struct CullParameters {
    uint candidateCount;
    uint deferredCapacity;
    uint reserved0;
    uint reserved1;
};
[[vk::push_constant]] ConstantBuffer<CullParameters> cullParameters;
[[vk::binding(0,0)]] StructuredBuffer<InstanceRecord> cullInstances;
[[vk::binding(1,0)]] StructuredBuffer<Candidate> candidates;
[[vk::binding(2,0)]] StructuredBuffer<BinRecord> bins;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> visibleIds;
[[vk::binding(4,0)]] RWStructuredBuffer<IndirectArgs> args;
[[vk::binding(5,0)]] RWStructuredBuffer<uint> deferredIds;
[[vk::binding(6,0)]] RWStructuredBuffer<uint> deferredCount;
[[vk::binding(7,0)]] Texture2D<float> previousHzb;
[[vk::binding(8,0)]] Texture2D<float> currentHzb;
[[vk::binding(9,0)]] StructuredBuffer<ViewRecord> cullViews;

float4 boundsCorner(float4 center, float4 extent, uint corner) {
    return float4(center.xyz + extent.xyz * float3((corner & 1) != 0 ? 1 : -1,
                                                    (corner & 2) != 0 ? 1 : -1,
                                                    (corner & 4) != 0 ? 1 : -1), 1);
}
bool finiteClip(float4 clip) {
    return all(isfinite(clip));
}
// A plane may reject an AABB only when all eight corners are strictly outside.
// Nonfinite arithmetic fails open so malformed data cannot cause disappearing meshes.
bool inFrustum(float4 center, float4 extent, float4x4 viewProjection) {
    uint rejected[6] = {0, 0, 0, 0, 0, 0};
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        float4 clip = mul(viewProjection, boundsCorner(center, extent, corner));
        if (!finiteClip(clip)) return true;
        float planes[6] = {clip.x + clip.w, clip.w - clip.x,
                           clip.y + clip.w, clip.w - clip.y,
                           clip.z, clip.w - clip.z};
        [unroll] for (uint plane = 0; plane < 6; ++plane)
            rejected[plane] += planes[plane] < 0 ? 1 : 0;
    }
    [unroll] for (uint plane = 0; plane < 6; ++plane)
        if (rejected[plane] == 8) return false;
    return true;
}
// Ordinary Z: the HZB contains the furthest depth (maximum) in every footprint.
// The nearest candidate depth must be farther than *all* those samples to occlude.
bool occluded(float4 center, float4 extent, float4x4 viewProjection,
              float4 viewport, uint4 pyramidSize, Texture2D<float> pyramid) {
    if (pyramidSize.x == 0 || pyramidSize.y == 0 || pyramidSize.z == 0 ||
        viewport.z <= 0 || viewport.w <= 0) return false;
    float2 minPixel = float2(1e30, 1e30), maxPixel = float2(-1e30, -1e30);
    float nearestDepth = 1;
    [unroll] for (uint corner = 0; corner < 8; ++corner) {
        float4 clip = mul(viewProjection, boundsCorner(center, extent, corner));
        // Near-plane crossings and perspective singularities are always visible.
        if (!finiteClip(clip) || clip.w <= 0 || clip.z <= 0 || clip.z >= clip.w) return false;
        float3 projected = clip.xyz / clip.w;
        float2 pixel = viewport.xy + (projected.xy * 0.5 + 0.5) * viewport.zw;
        if (!all(isfinite(pixel)) || !isfinite(projected.z)) return false;
        minPixel = min(minPixel, pixel);
        maxPixel = max(maxPixel, pixel);
        nearestDepth = min(nearestDepth, projected.z);
    }
    if (minPixel.x < 0 || minPixel.y < 0 || maxPixel.x >= pyramidSize.x ||
        maxPixel.y >= pyramidSize.y) return false;
    float extentPixels = max(maxPixel.x - minPixel.x, maxPixel.y - minPixel.y);
    uint mip = min((uint)ceil(log2(max(extentPixels, 1.0))), pyramidSize.z - 1);
    uint2 size = max(uint2(1, 1), (pyramidSize.xy + ((1u << mip) - 1)) >> mip);
    uint2 first = min((uint2)floor(minPixel / (1u << mip)), size - 1);
    uint2 last = min((uint2)floor(maxPixel / (1u << mip)), size - 1);
    float furthest = 0;
    for (uint y = first.y; y <= last.y; ++y)
        for (uint x = first.x; x <= last.x; ++x)
            furthest = max(furthest, pyramid.Load(int3(x, y, mip)));
    return nearestDepth > furthest + 0.0001;
}
bool appendVisible(uint binIndex, uint instanceId) {
    BinRecord bin = bins[binIndex];
    // Every candidate is dispatched once and each bin reserves its full candidate
    // count. A fetch-add therefore needs no retry on valid host-generated input.
    uint slot;
    InterlockedAdd(args[binIndex].instanceCount, 1u, slot);
    if (slot < bin.capacity) {
        visibleIds[bin.visibleBase + slot] = instanceId;
        return true;
    }
    // Keep the final indirect count bounded even if a malformed candidate table
    // violates that invariant. No raster pass reads it until this dispatch ends.
    uint ignored;
    InterlockedAdd(args[binIndex].instanceCount, 0xffffffffu, ignored);
    return false;
}
bool appendDeferred(uint candidateIndex) {
    uint slot;
    InterlockedAdd(deferredCount[0], 1u, slot);
    if (slot < cullParameters.deferredCapacity) {
        deferredIds[slot] = candidateIndex;
        return true;
    }
    uint ignored;
    InterlockedAdd(deferredCount[0], 0xffffffffu, ignored);
    return false;
}
[shader("compute")]
[numthreads(64, 1, 1)]
void gpuCullMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint index = dispatchId.x + cullParameters.reserved0;
    if (index >= cullParameters.candidateCount) return;
    Candidate candidate = candidates[index];
    InstanceRecord instance = cullInstances[candidate.instanceId];
    ViewRecord view = cullViews[0];
    if (!inFrustum(instance.currentCenter, instance.currentExtent,
                   view.currentViewProjection)) return;
    bool guessedHidden = view.flags.x != 0 && (instance.metadata.x & 1u) != 0 &&
        occluded(instance.previousCenter, instance.previousExtent,
                 view.previousViewProjection, view.previousViewport,
                 view.previousHzbSize, previousHzb);
    if (guessedHidden && appendDeferred(index)) return;
    appendVisible(candidate.binIndex, candidate.instanceId);
}
[shader("compute")]
[numthreads(64, 1, 1)]
void gpuPostCullMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint index = dispatchId.x + cullParameters.reserved0;
    if (index >= cullParameters.deferredCapacity || index >= deferredCount[0]) return;
    Candidate candidate = candidates[deferredIds[index]];
    InstanceRecord instance = cullInstances[candidate.instanceId];
    ViewRecord view = cullViews[0];
    if (!occluded(instance.currentCenter, instance.currentExtent,
                  view.currentViewProjection, view.currentViewport,
                  view.currentHzbSize, currentHzb))
        appendVisible(candidate.binIndex, candidate.instanceId);
}

#elif defined(FASET_GPU_HZB)
struct HzbParameters {
    uint sourceWidth, sourceHeight, outputWidth, outputHeight;
};
[[vk::push_constant]] ConstantBuffer<HzbParameters> hzbParameters;
[[vk::binding(0,0)]] Texture2D<float> hzbSource;
[[vk::binding(1,0)]] [vk::image_format("r32f")] RWTexture2D<float> hzbOutput;
[shader("compute")]
[numthreads(8, 8, 1)]
void gpuHzbMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint2 pixel = dispatchId.xy;
    if (pixel.x >= hzbParameters.outputWidth || pixel.y >= hzbParameters.outputHeight) return;
    if (hzbParameters.outputWidth >= hzbParameters.sourceWidth &&
        hzbParameters.outputHeight >= hzbParameters.sourceHeight) {
        // Mip 0 copies depth into a power-of-two base. Missing edge texels are
        // ordinary-Z far depth, so a padded region can never hide geometry.
        hzbOutput[pixel] = pixel.x < hzbParameters.sourceWidth &&
                           pixel.y < hzbParameters.sourceHeight
                         ? hzbSource.Load(int3(pixel, 0)) : 1.0;
        return;
    }
    float furthest = 0;
    [unroll] for (uint y = 0; y < 2; ++y)
        [unroll] for (uint x = 0; x < 2; ++x) {
            uint2 child = pixel * 2 + uint2(x, y);
            // Ordinary-Z clear/far depth is 1. Padding therefore cannot occlude.
            float depth = child.x < hzbParameters.sourceWidth &&
                          child.y < hzbParameters.sourceHeight
                        ? hzbSource.Load(int3(child, 0)) : 1.0;
            furthest = max(furthest, depth);
        }
    hzbOutput[pixel] = furthest;
}
#else
#error Select FASET_GPU_GRAPHICS, FASET_GPU_CULL, or FASET_GPU_HZB.
#endif
