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
    uint4 counts;                    // local count, sun enabled, sun shadow enabled, view count
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
[shader("vertex")]
VertexOutput vertexMain(VertexInput v) {
    VertexOutput o;
    o.position=v.clip; o.world=v.world; o.normal=v.normal; o.color=v.color; o.material=v.material; o.uv=v.uv;
    return o;
}
[shader("vertex")]
float4 shadowMain(VertexInput v) : SV_Position { return mul(frame.lightViewProjection, float4(v.world,1)); }
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
[shader("fragment")]
float4 fragmentMain(VertexOutput v) : SV_Target {
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
        if (lighting.counts.z != 0 && nl > 0) {
            float4 lightClip=mul(frame.lightViewProjection,float4(v.world,1));
            float3 projected=lightClip.xyz/lightClip.w;
            float2 uv=projected.xy*.5+.5;
            if(all(uv>=0.0)&&all(uv<=1.0)&&projected.z>=0.0&&projected.z<=1.0) {
                visibility=0.0;
                for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
                    float depth=shadowMap.SampleLevel(shadowSampler,uv+float2(x,y)/1024.0,0);
                    visibility += projected.z-max(0.0008,0.003*(1.0-nl)) <= depth ? 1.0/9.0 : 0.0;
                }
            }
        }
        linear += directBRDF(base.rgb, rough, metal, n, view, l) *
                  lighting.sunColor.rgb * (lighting.sunDirectionIntensity.w * 3.0 * visibility);
    }
    for (uint i=0; i<lighting.counts.x; ++i) {
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
        linear += directBRDF(base.rgb, rough, metal, n, view, l) *
                  light.colorIntensity.rgb * (light.colorIntensity.w * attenuation);
    }
    linear=linear/(1.0+linear);
    return float4(pow(max(linear,0),float3(1.0/2.2)),base.a);
}
