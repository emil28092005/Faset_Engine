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
[shader("vertex")]
VertexOutput vertexMain(VertexInput v) {
    VertexOutput o;
    o.position=v.clip; o.world=v.world; o.normal=v.normal; o.color=v.color; o.material=v.material; o.uv=v.uv;
    return o;
}
[shader("vertex")]
float4 shadowMain(VertexInput v) : SV_Position { return mul(frame.lightViewProjection, float4(v.world,1)); }
[shader("fragment")]
float4 fragmentMain(VertexOutput v) : SV_Target {
    float4 base = v.color * colorMap.Sample(colorSampler, v.uv);
    if (dot(v.normal,v.normal) < 0.01) return base;
    const float pi = 3.14159265;
    float3 n=normalize(v.normal), l=normalize(-frame.lightDirection.xyz), view=normalize(frame.eye.xyz-v.world), h=normalize(l+view);
    float nl=max(dot(n,l),0.0), nv=max(dot(n,view),0.001), nh=max(dot(n,h),0.0), vh=max(dot(view,h),0.0);
    float rough=clamp(v.material.x,0.08,1.0), metal=saturate(v.material.y);
    float a=rough*rough, a2=a*a, denom=nh*nh*(a2-1.0)+1.0;
    float d=a2/(pi*denom*denom+0.0001);
    float k=(rough+1.0)*(rough+1.0)/8.0;
    float g=(nl/(nl*(1.0-k)+k))*(nv/(nv*(1.0-k)+k));
    float3 f0=lerp(float3(0.04),base.rgb,metal), fresnel=f0+(1.0-f0)*pow(1.0-vh,5.0);
    float3 spec=d*g*fresnel/max(4.0*nv*nl,0.001);
    float4 lightClip=mul(frame.lightViewProjection,float4(v.world,1));
    float3 projected=lightClip.xyz/lightClip.w;
    float2 uv=projected.xy*.5+.5;
    float visibility=1.0;
    if(all(uv>=0.0)&&all(uv<=1.0)&&projected.z>=0.0&&projected.z<=1.0) {
        visibility=0.0;
        for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
            float depth=shadowMap.SampleLevel(shadowSampler,uv+float2(x,y)/1024.0,0);
            visibility += projected.z-max(0.0008,0.003*(1.0-nl)) <= depth ? 1.0/9.0 : 0.0;
        }
    }
    float3 linear=base.rgb*.12 + ((1.0-fresnel)*(1.0-metal)*base.rgb/pi+spec)*nl*3.0*visibility;
    linear=linear/(1.0+linear);
    return float4(pow(max(linear,0),float3(1.0/2.2)),base.a);
}
