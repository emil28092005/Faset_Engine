struct VertexInput {
    float4 clip : POSITION;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float2 material : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
struct VertexOutput {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float2 material : TEXCOORD1;
    float2 uv : TEXCOORD2;
};
struct FrameParameters {
    column_major float4x4 lightViewProjection;
    float4 lightDirection;
    float4 eye;
};
[[vk::push_constant]] ConstantBuffer<FrameParameters> frame;
[[vk::binding(0,0)]] Texture2D<float> shadowMap;
[[vk::binding(1,0)]] SamplerState shadowSampler;
[[vk::binding(2,0)]] Texture2D<float4> colorMap;
[[vk::binding(3,0)]] SamplerState colorSampler;
// Shared Direct/P2 graphics ABI. The legacy material set remains set 0;
// GPU-only instance/visibility records occupy set 2.
struct LightingHeader {
    uint4 counts;                    // local count, sun enabled, sun shadow enabled, sun view count
    float4 sunDirectionIntensity;    // xyz world-space ray direction, w intensity
    float4 sunColor;
    float4 cameraForwardShadowDistance;
    float4 cascadeSplits;
};
struct LocalLightGpu {
    float4 positionRange;
    float4 directionCosOuter;
    float4 colorIntensity;
    float4 coneTypeShadowView;       // cos(inner), 0=point/1=spot, shadow view, flags
    float4 reserved;
};
struct ShadowViewGpu {
    column_major float4x4 viewProjection;
    float4 tileScaleOffset;
    float4 guardedClamp;
    float4 biasFlags;
};
[[vk::binding(0,1)]] StructuredBuffer<LightingHeader> lightingFrame;
[[vk::binding(1,1)]] StructuredBuffer<LocalLightGpu> localLights;
[[vk::binding(2,1)]] StructuredBuffer<ShadowViewGpu> shadowViews;
[[vk::binding(3,1)]] Texture2D<float> localShadowAtlas;
// 4-word header, then 66 words per 16x16 tile: count, overflow, 64 indices.
// An overflowing tile evaluates the full submitted list instead of losing light.
[[vk::binding(4,1)]] StructuredBuffer<uint> lightTileWords;
[shader("vertex")]
VertexOutput vertexMain(VertexInput v) {
    VertexOutput o;
    o.position=v.clip; o.world=v.world; o.normal=v.normal; o.color=v.color; o.material=v.material; o.uv=v.uv;
    return o;
}
[shader("vertex")]
float4 shadowMain(VertexInput v) : SV_Position { return mul(frame.lightViewProjection, float4(v.world,1)); }
float sampleSunCascade(uint index, float3 world, float nl) {
    ShadowViewGpu record = shadowViews[index];
    if (record.biasFlags.w < 0.5) return 1.0;
    float4 clip = mul(record.viewProjection, float4(world,1));
    if (clip.w <= 0.0) return 1.0;
    float3 projected = clip.xyz / clip.w;
    float2 localUV = projected.xy * 0.5 + 0.5;
    if (any(localUV < 0.0) || any(localUV > 1.0) ||
        projected.z < 0.0 || projected.z > 1.0) return 1.0;
    float2 atlasUV = localUV * record.tileScaleOffset.xy + record.tileScaleOffset.zw;
    float bias = max(record.biasFlags.x, record.biasFlags.y * (1.0 - nl));
    float visible = 0.0;
    for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x) {
        float2 tap = clamp(atlasUV + float2(x,y) * record.biasFlags.z,
                           record.guardedClamp.xy, record.guardedClamp.zw);
        float depth = shadowMap.SampleLevel(shadowSampler, tap, 0);
        visible += projected.z - bias <= depth ? 1.0 / 9.0 : 0.0;
    }
    return visible;
}
float sampleLocalFace(uint index, float3 world, float nl) {
    ShadowViewGpu record = shadowViews[index];
    if (record.biasFlags.w < 0.5) return 1.0;
    float4 clip = mul(record.viewProjection, float4(world,1));
    if (clip.w <= 0.0) return 1.0;
    float3 projected = clip.xyz / clip.w;
    float2 localUV = projected.xy * 0.5 + 0.5;
    if (any(localUV < 0.0) || any(localUV > 1.0) ||
        projected.z < 0.0 || projected.z > 1.0) return 1.0;
    float2 atlasUV = localUV * record.tileScaleOffset.xy + record.tileScaleOffset.zw;
    float bias = max(record.biasFlags.x, record.biasFlags.y * (1.0 - nl));
    float visible = 0.0;
    for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x) {
        float2 tap = clamp(atlasUV + float2(x,y) * record.biasFlags.z,
                           record.guardedClamp.xy, record.guardedClamp.zw);
        float depth = localShadowAtlas.SampleLevel(shadowSampler, tap, 0);
        visible += projected.z - bias <= depth ? 1.0 / 9.0 : 0.0;
    }
    return visible;
}
uint pointShadowFace(float3 lightToFragment) {
    float3 magnitude = abs(lightToFragment);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
        return lightToFragment.x >= 0.0 ? 0 : 1;
    if (magnitude.y >= magnitude.z)
        return lightToFragment.y >= 0.0 ? 2 : 3;
    return lightToFragment.z >= 0.0 ? 4 : 5;
}
float3 directBRDF(float3 base, float rough, float metal, float3 n, float3 view, float3 l) {
    const float pi = 3.14159265;
    float nl = max(dot(n,l),0.0);
    if (nl <= 0.0) return float3(0);
    float3 halfVector = l + view;
    float halfLengthSquared = dot(halfVector, halfVector);
    float3 h = halfLengthSquared > 1e-8 ? halfVector * rsqrt(halfLengthSquared) : n;
    float nv=max(dot(n,view),0.001), nh=max(dot(n,h),0.0), vh=max(dot(view,h),0.0);
    float a=rough*rough, a2=a*a, denom=nh*nh*(a2-1.0)+1.0;
    float d=a2/(pi*denom*denom+0.0001);
    float k=(rough+1.0)*(rough+1.0)/8.0;
    float g=(nl/(nl*(1.0-k)+k))*(nv/(nv*(1.0-k)+k));
    float3 f0=lerp(float3(0.04),base,metal), fresnel=f0+(1.0-f0)*pow(1.0-vh,5.0);
    float3 spec=d*g*fresnel/max(4.0*nv*nl,0.001);
    return ((1.0-fresnel)*(1.0-metal)*base/pi+spec)*nl;
}
float4 shadeScene(VertexOutput v) {
    float4 sampled = colorMap.Sample(colorSampler, v.uv);
    if (dot(v.normal,v.normal) < 1e-12) {
        // UI/sprite tint is in display space; sRGB textures were decoded by Vulkan.
        if (v.material.x > 0.5) sampled.rgb = lerp(sampled.rgb * 12.92, 1.055 * pow(max(sampled.rgb,0),float3(1.0/2.4)) - 0.055, step(0.0031308, sampled.rgb));
        return v.color * sampled;
    }
    float4 base = v.color * sampled;
    LightingHeader lighting = lightingFrame[0];
    float3 n=normalize(v.normal);
    float3 viewDelta=frame.eye.xyz-v.world;
    float viewLengthSquared=dot(viewDelta,viewDelta);
    float3 view=viewLengthSquared > 1e-8 ? viewDelta*rsqrt(viewLengthSquared) : n;
    float rough=clamp(v.material.x,0.08,1.0), metal=saturate(v.material.y);
    float3 linear=base.rgb*.12;
    if (lighting.counts.y != 0 && lighting.sunDirectionIntensity.w > 0) {
        float3 l=normalize(-lighting.sunDirectionIntensity.xyz);
        float nl=max(dot(n,l),0.0);
        float visibility=1.0;
        if (lighting.counts.z != 0 && lighting.counts.w != 0 && nl > 0) {
            if (lighting.counts.w == 1) {
                visibility = sampleSunCascade(0, v.world, nl);
            } else {
                float cameraDepth = dot(v.world - frame.eye.xyz,
                                        lighting.cameraForwardShadowDistance.xyz);
                if (cameraDepth >= 0.0 &&
                    cameraDepth <= lighting.cameraForwardShadowDistance.w) {
                    uint cascade = 0;
                    while (cascade + 1 < lighting.counts.w &&
                           cameraDepth > lighting.cascadeSplits[cascade]) ++cascade;
                    visibility = sampleSunCascade(cascade, v.world, nl);
                    if (cascade + 1 < lighting.counts.w) {
                        float previousSplit = cascade == 0 ? 0.0 :
                                              lighting.cascadeSplits[cascade-1];
                        float blendWidth = max(0.2,
                            0.1 * (lighting.cascadeSplits[cascade] - previousSplit));
                        float blend = saturate((cameraDepth -
                            (lighting.cascadeSplits[cascade] - blendWidth)) / blendWidth);
                        if (blend > 0.0)
                            visibility = lerp(visibility,
                                sampleSunCascade(cascade+1, v.world, nl), blend);
                    }
                }
            }
        }
        linear += directBRDF(base.rgb, rough, metal, n, view, l) *
                  lighting.sunColor.rgb * (lighting.sunDirectionIntensity.w * 3.0 * visibility);
    }
    uint candidateCount = lighting.counts.x;
    uint tileBase = 0;
    bool tileList = false;
    if (lightTileWords[1] != 0 && lightTileWords[0] != 0 && lightTileWords[2] != 0) {
        uint tileX = min(uint(v.position.x) / 16u, lightTileWords[0] - 1u);
        uint tileY = min(uint(v.position.y) / 16u, lightTileWords[2] - 1u);
        tileBase = 4u + (tileY * lightTileWords[0] + tileX) * 66u;
        if (lightTileWords[tileBase + 1u] == 0) {
            candidateCount = min(lightTileWords[tileBase], lighting.counts.x);
            tileList = true;
        }
    }
    for (uint candidate=0; candidate<candidateCount; ++candidate) {
        uint i = tileList ? lightTileWords[tileBase + 2u + candidate] : candidate;
        LocalLightGpu light=localLights[i];
        float3 delta=light.positionRange.xyz-v.world;
        float distanceSquared=max(dot(delta,delta),1e-6);
        float distance=sqrt(distanceSquared);
        float range=max(light.positionRange.w,1e-4);
        if (distance >= range || light.colorIntensity.w <= 0) continue;
        float3 l=delta/distance;
        float relative=distance/range;
        float cutoff=1.0-relative*relative*relative*relative;
        float attenuation=cutoff*cutoff/(1.0+distanceSquared);
        if (light.coneTypeShadowView.y > 0.5) {
            float cosAngle=dot(-l,normalize(light.directionCosOuter.xyz));
            float denominator=max(light.coneTypeShadowView.x-light.directionCosOuter.w,1e-4);
            float cone=saturate((cosAngle-light.directionCosOuter.w)/denominator);
            attenuation *= cone*cone*(3.0-2.0*cone);
        }
        float visibility = 1.0;
        float nl = max(dot(n,l), 0.0);
        if (light.coneTypeShadowView.w > 0.5 && nl > 0.0 && attenuation > 0.0) {
            uint face = light.coneTypeShadowView.w > 1.5 ?
                pointShadowFace(v.world - light.positionRange.xyz) : 0;
            uint viewIndex = uint(light.coneTypeShadowView.z + 0.5) + face;
            visibility = sampleLocalFace(viewIndex, v.world, nl);
        }
        linear += directBRDF(base.rgb, rough, metal, n, view, l) *
                  light.colorIntensity.rgb * (light.colorIntensity.w * attenuation * visibility);
    }
    linear=linear/(1.0+linear);
    return float4(pow(max(linear,0),float3(1.0/2.2)),base.a);
}
[shader("fragment")]
float4 fragmentMain(VertexOutput v) : SV_Target { return shadeScene(v); }

struct TemporalVertexInput {
    float4 clip : POSITION;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float2 material : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 previousClip : TEXCOORD3;
    float motionValid : TEXCOORD4;
};
struct TemporalVertexOutput {
    float4 position : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : NORMAL;
    float4 color : COLOR0;
    float2 material : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float4 previousClip : TEXCOORD3;
    float4 currentClip : TEXCOORD4;
    float motionValid : TEXCOORD5;
};
[shader("vertex")]
TemporalVertexOutput temporalVertexMain(TemporalVertexInput v) {
    TemporalVertexOutput o;
    o.position=v.clip; o.world=v.world; o.normal=v.normal; o.color=v.color;
    o.material=v.material; o.uv=v.uv; o.previousClip=v.previousClip;
    o.currentClip=v.clip; o.motionValid=v.motionValid;
    return o;
}
struct TemporalFragmentOutput {
    float4 color : SV_Target0;
    float4 velocity : SV_Target1;
};
[shader("fragment")]
TemporalFragmentOutput temporalFragmentMain(TemporalVertexOutput v) {
    VertexOutput shading;
    shading.position=v.position; shading.world=v.world; shading.normal=v.normal;
    shading.color=v.color; shading.material=v.material; shading.uv=v.uv;
    TemporalFragmentOutput result;
    result.color=shadeScene(shading);
    result.velocity=float4(0);
    if (v.motionValid > .5 && result.color.a >= .999 &&
        all(isfinite(v.currentClip)) && all(isfinite(v.previousClip)) &&
        v.currentClip.w > 0 && v.previousClip.w > 0) {
        result.velocity.xy=(v.currentClip.xy / v.currentClip.w -
                            v.previousClip.xy / v.previousClip.w) * .5;
        result.velocity.z=v.previousClip.z / v.previousClip.w;
        result.velocity.w=1;
    }
    return result;
}
