// Build-time HLSL compatibility fixture: compiled by the same Slang toolchain.
[[vk::binding(0, 0)]] RWStructuredBuffer<float4> outputValues;
[numthreads(1, 1, 1)]
void compatibilityMain(uint3 id : SV_DispatchThreadID) {
    outputValues[id.x] = float4(0.25, 0.5, 0.75, 1.0);
}
