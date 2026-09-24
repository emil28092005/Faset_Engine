// Temporal scene resolve and full-resolution composite. No material/lighting
// descriptors are consumed here; the scene was shaded before this pass.
// Velocity target: xy = current-minus-prior scene-local UV, z = expected prior
// clip depth, w = opaque motion validity (zero for reactive/invalid pixels).
#if defined(FASET_TEMPORAL_RESOLVE)
struct TemporalResolveParameters {
    uint4 dimensions;         // output width/height, internal width/height
    float4 outputSceneRect;   // output-pixel x/y/width/height
    float4 internalSceneRect; // internal-pixel x/y/width/height
    uint4 flags;              // x = prior history valid; y = count pixel decisions
    float4 jitterMotion;      // xy = current-minus-prior local UV; z = static camera
};
[[vk::push_constant]] ConstantBuffer<TemporalResolveParameters> temporalParameters;
[[vk::binding(0,0)]] Texture2D<float4> currentSceneColor;
[[vk::binding(1,0)]] Texture2D<float> currentSceneDepth;
[[vk::binding(2,0)]] Texture2D<float4> currentSceneVelocity;
[[vk::binding(3,0)]] Texture2D<float4> previousHistoryColor;
[[vk::binding(4,0)]] Texture2D<float> previousHistoryDepth;
[[vk::binding(5,0)]] [vk::image_format("rgba16f")]
RWTexture2D<float4> nextHistoryColor;
[[vk::binding(6,0)]] [vk::image_format("r32f")]
RWTexture2D<float> nextHistoryDepth;
// A tiny GPU counter buffer is bound for the normal pipeline too, but touched
// only when explicit Editor diagnostics requests exact per-pixel statistics.
[[vk::binding(7,0)]] RWStructuredBuffer<uint> historyDecisionCounts;

int2 clampScenePixel(int2 pixel) {
    return clamp(pixel, int2(0), int2(temporalParameters.dimensions.zw) - 1);
}
float4 sceneColorAt(int2 pixel) {
    return currentSceneColor.Load(int3(clampScenePixel(pixel), 0));
}
float sceneDepthAt(int2 pixel) {
    return currentSceneDepth.Load(int3(clampScenePixel(pixel), 0));
}
float4 sceneVelocityAt(int2 pixel) {
    return currentSceneVelocity.Load(int3(clampScenePixel(pixel), 0));
}
float3 historyDepthAware(float2 uv, float expectedDepth, float tolerance,
                         bool farSilhouette, float3 currentColor,
                         out float acceptedWeight, out bool nearerOccluder) {
    float2 position = uv * float2(temporalParameters.dimensions.xy) - .5;
    int2 base = int2(floor(position));
    float2 fraction = position - float2(base);
    int2 limit = int2(temporalParameters.dimensions.xy) - 1;
    float3 sum = 0;
    acceptedWeight = 0;
    nearerOccluder = false;
    [unroll] for (int dy = 0; dy < 2; ++dy)
        [unroll] for (int dx = 0; dx < 2; ++dx) {
            const int2 tap = clamp(base + int2(dx, dy), int2(0), limit);
            const float weight = (dx == 0 ? 1 - fraction.x : fraction.x) *
                                 (dy == 0 ? 1 - fraction.y : fraction.y);
            const float depth = previousHistoryDepth.Load(int3(tap, 0));
            const bool matching = isfinite(depth) &&
                abs(depth - expectedDepth) <= tolerance;
            const bool far = farSilhouette && isfinite(depth) && depth >= .999;
            nearerOccluder = nearerOccluder ||
                (weight > 1e-5 && isfinite(depth) &&
                 depth + tolerance < expectedDepth);
            if (matching || far) {
                sum += weight * previousHistoryColor.Load(int3(tap, 0)).rgb;
                acceptedWeight += weight;
            } else if (farSilhouette) {
                // Old, closer occluders cannot bleed into a newly exposed edge.
                sum += weight * currentColor;
                acceptedWeight += weight;
            }
        }
    return acceptedWeight > 1e-5 ? sum / acceptedWeight : currentColor;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void temporalResolveMain(uint3 dispatchId : SV_DispatchThreadID) {
    const uint2 outputPixel = dispatchId.xy;
    const uint2 outputExtent = temporalParameters.dimensions.xy;
    const uint2 internalExtent = temporalParameters.dimensions.zw;
    if (outputPixel.x >= outputExtent.x || outputPixel.y >= outputExtent.y) return;

    const float2 center = float2(outputPixel) + .5;
    const float4 outputRect = temporalParameters.outputSceneRect;
    const float4 internalRect = temporalParameters.internalSceneRect;
    const bool insideScene = all(center >= outputRect.xy) &&
                             all(center < outputRect.xy + outputRect.zw) &&
                             all(outputRect.zw > 0);
    const float2 sceneLocalUV = insideScene
        ? (center - outputRect.xy) / outputRect.zw : float2(0);
    const float2 internalPosition = insideScene
        ? internalRect.xy + sceneLocalUV * internalRect.zw
        : center / float2(outputExtent) * float2(internalExtent);
    const int2 currentPixel = clampScenePixel(int2(floor(internalPosition)));
    const float4 currentColor = sceneColorAt(currentPixel);
    const float currentDepth = sceneDepthAt(currentPixel);
    float4 resolved = currentColor;
    bool acceptedHistory = false;

    if (insideScene && temporalParameters.flags.x != 0) {
        const float4 centerMotion = sceneVelocityAt(currentPixel);
        const bool centerValid = all(isfinite(centerMotion)) && centerMotion.w > 0 &&
                                 centerMotion.z >= 0 && centerMotion.z <= 1;
        float4 selectedMotion = centerMotion;
        bool stationarySilhouette = false;
        bool stationaryForegroundEdge = false;
        if (centerValid) {
            float selectedDepth = currentDepth;
            const float currentTolerance = .002 + .01 * currentDepth;
            bool touchesFar = false;
            [unroll] for (int dy = -1; dy <= 1; ++dy)
                [unroll] for (int dx = -1; dx <= 1; ++dx) {
                    const int2 neighbor = clampScenePixel(currentPixel + int2(dx, dy));
                    const float depth = sceneDepthAt(neighbor);
                    touchesFar = touchesFar || depth >= .999;
                    const float4 motion = sceneVelocityAt(neighbor);
                    if (all(isfinite(motion)) && motion.w > 0 && motion.z >= 0 &&
                        motion.z <= 1 && abs(depth - currentDepth) <= currentTolerance &&
                        depth < selectedDepth) {
                        selectedDepth = depth;
                        selectedMotion = motion;
                    }
                }
            const float2 mismatchPixels =
                (centerMotion.xy - temporalParameters.jitterMotion.xy) * outputRect.zw;
            stationaryForegroundEdge = touchesFar &&
                temporalParameters.jitterMotion.z > .5 &&
                dot(mismatchPixels, mismatchPixels) < .01;
        } else if (currentDepth >= .999 && temporalParameters.jitterMotion.z > .5) {
            // The far side of a *static* subpixel silhouette has no center
            // velocity. Borrow only a neighbor whose motion is indistinguishable
            // from camera jitter. Moving/revealed edges keep strict rejection.
            float bestDistance = 1e30;
            [unroll] for (int dy = -1; dy <= 1; ++dy)
                [unroll] for (int dx = -1; dx <= 1; ++dx) {
                    const int2 neighbor = clampScenePixel(currentPixel + int2(dx, dy));
                    const float depth = sceneDepthAt(neighbor);
                    const float4 motion = sceneVelocityAt(neighbor);
                    const float2 mismatchPixels =
                        (motion.xy - temporalParameters.jitterMotion.xy) * outputRect.zw;
                    const float distance = float(dx * dx + dy * dy);
                    if (depth < .999 && all(isfinite(motion)) && motion.w > 0 &&
                        motion.z >= 0 && motion.z <= 1 &&
                        dot(mismatchPixels, mismatchPixels) < .01 &&
                        distance < bestDistance) {
                        bestDistance = distance;
                        selectedMotion = motion;
                        stationarySilhouette = true;
                    }
                }
        }
        if (centerValid || stationarySilhouette) {
            // Velocity contains the raster jitter delta. Foreground history
            // tracks scene motion in output pixels; a far-side subpixel edge
            // still follows its prior jittered footprint to gather coverage.
            const float2 previousLocalUV = sceneLocalUV -
                (selectedMotion.xy -
                 (stationarySilhouette ? float2(0) : temporalParameters.jitterMotion.xy));
            if (all(isfinite(previousLocalUV)) && all(previousLocalUV >= 0) &&
                all(previousLocalUV < 1)) {
                const float2 previousOutputUV =
                    (outputRect.xy + previousLocalUV * outputRect.zw) / float2(outputExtent);
                if (all(previousOutputUV >= 0) && all(previousOutputUV < 1)) {
                    const int2 priorPixel = clamp(
                        int2(floor(previousOutputUV * float2(outputExtent))), int2(0),
                        int2(outputExtent) - 1);
                    const float priorDepth = previousHistoryDepth.Load(int3(priorPixel, 0));
                    const float depthTolerance = .002 + .01 * selectedMotion.z;
                    const bool matchingSurface = isfinite(priorDepth) &&
                        abs(priorDepth - selectedMotion.z) <= depthTolerance;
                    const bool matchingFar = (stationarySilhouette ||
                                              stationaryForegroundEdge) &&
                        isfinite(priorDepth) && priorDepth >= .999;
                    if (matchingSurface || matchingFar) {
                        float3 low = float3(1e30), high = float3(-1e30);
                        [unroll] for (int dy = -1; dy <= 1; ++dy)
                            [unroll] for (int dx = -1; dx <= 1; ++dx) {
                                const float3 color = sceneColorAt(currentPixel + int2(dx, dy)).rgb;
                                low = min(low, color);
                                high = max(high, color);
                            }
                        const float2 motionPixels = selectedMotion.xy * outputRect.zw;
                        // Edge weights are intentionally bounded: strong far
                        // history smears a moving reveal and loses wire contrast.
                        const float weight = stationarySilhouette ? .15
                            : matchingFar ? .11
                            : .9 * saturate(centerMotion.w) /
                                  (1 + .5 * length(motionPixels));
                        float acceptedWeight;
                        bool nearerOccluder;
                        const float3 sampled = historyDepthAware(
                            previousOutputUV, selectedMotion.z, depthTolerance,
                            stationarySilhouette || stationaryForegroundEdge,
                            currentColor.rgb, acceptedWeight, nearerOccluder);
                        if (acceptedWeight > 1e-5 &&
                            (!matchingFar || !nearerOccluder)) {
                            const float3 priorColor = clamp(sampled, low, high);
                            resolved.rgb = lerp(currentColor.rgb, priorColor, weight);
                            acceptedHistory = true;
                        }
                    }
                }
            }
        }
    }
    nextHistoryColor[outputPixel] = resolved;
    nextHistoryDepth[outputPixel] = currentDepth;
    if (insideScene && temporalParameters.flags.y != 0) {
        uint oldCount;
        InterlockedAdd(historyDecisionCounts[acceptedHistory ? 0 : 1], 1, oldCount);
    }
}

#elif defined(FASET_TEMPORAL_COMPOSITE)
[[vk::binding(0,0)]] Texture2D<float4> resolvedHistoryColor;

[shader("vertex")]
float4 temporalCompositeVertexMain(float4 clip : POSITION) : SV_Position {
    return clip;
}

[shader("fragment")]
float4 temporalCompositeFragmentMain(float4 position : SV_Position) : SV_Target {
    // Scene shading is already display-referred. No second tone or gamma pass.
    return resolvedHistoryColor.Load(int3(int2(position.xy), 0));
}
#else
#error Select FASET_TEMPORAL_RESOLVE or FASET_TEMPORAL_COMPOSITE.
#endif
