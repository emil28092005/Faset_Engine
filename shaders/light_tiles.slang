// Conservative depth-free 16x16 Forward+ construction. One invocation owns
// one tile, so indices remain in the same sorted order as the forward reference.
struct LocalLightGpu {
    float4 positionRange;
    float4 directionCosOuter;
    float4 colorIntensity;
    float4 coneTypeShadowView;
    float4 reserved;
};
struct TileBuildParameters {
    column_major float4x4 viewProjection;
    float4 viewport; // x, y, width, height in framebuffer pixels
    uint4 dimensions; // tilesX, tilesY, submitted local lights, capacity (<= 64)
};
[[vk::push_constant]] ConstantBuffer<TileBuildParameters> build;
[[vk::binding(0,0)]] StructuredBuffer<LocalLightGpu> localLights;
[[vk::binding(1,0)]] RWStructuredBuffer<uint> tileWords;

bool sphereTouchesPlane(float3 center, float radius, float4 plane) {
    // The final epsilon admits boundary/rounding cases rather than dropping a
    // light. We intentionally do not use scene depth or reject near-plane cuts.
    return dot(plane, float4(center, 1.0)) + radius * length(plane.xyz) >= -1e-4;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void lightTileMain(uint3 dispatchId : SV_DispatchThreadID) {
    uint tileId = dispatchId.x;
    uint tilesX = build.dimensions.x;
    uint tilesY = build.dimensions.y;
    if (tileId >= tilesX * tilesY) return;
    if (tileId == 0) {
        tileWords[0] = tilesX;
        tileWords[1] = 1;
        tileWords[2] = tilesY;
        tileWords[3] = min(build.dimensions.w, 64u);
    }
    uint tileX = tileId % tilesX;
    uint tileY = tileId / tilesX;
    float x0 = float(tileX * 16u);
    float y0 = float(tileY * 16u);
    float x1 = x0 + 16.0;
    float y1 = y0 + 16.0;
    float left = 2.0 * (x0 - build.viewport.x) / build.viewport.z - 1.0;
    float right = 2.0 * (x1 - build.viewport.x) / build.viewport.z - 1.0;
    float top = 2.0 * (y0 - build.viewport.y) / build.viewport.w - 1.0;
    float bottom = 2.0 * (y1 - build.viewport.y) / build.viewport.w - 1.0;
    float4 xRow = mul(float4(1, 0, 0, 0), build.viewProjection);
    float4 yRow = mul(float4(0, 1, 0, 0), build.viewProjection);
    float4 wRow = mul(float4(0, 0, 0, 1), build.viewProjection);
    float4 leftPlane = xRow - left * wRow;
    float4 rightPlane = right * wRow - xRow;
    float4 topPlane = yRow - top * wRow;
    float4 bottomPlane = bottom * wRow - yRow;
    uint base = 4u + tileId * 66u;
    uint count = 0;
    bool overflow = false;
    for (uint i = 0; i < build.dimensions.z; ++i) {
        LocalLightGpu light = localLights[i];
        if (light.colorIntensity.w <= 0.0) continue;
        float3 center = light.positionRange.xyz;
        float radius = light.positionRange.w;
        if (!sphereTouchesPlane(center, radius, leftPlane) ||
            !sphereTouchesPlane(center, radius, rightPlane) ||
            !sphereTouchesPlane(center, radius, topPlane) ||
            !sphereTouchesPlane(center, radius, bottomPlane)) continue;
        if (count < min(build.dimensions.w, 64u))
            tileWords[base + 2u + count] = i;
        else
            overflow = true;
        ++count;
    }
    tileWords[base] = min(count, min(build.dimensions.w, 64u));
    tileWords[base + 1u] = overflow ? 1u : 0u;
}
