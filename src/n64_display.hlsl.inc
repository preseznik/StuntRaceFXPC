R"(
Texture2D<float4> scene:register(t0);
float3 rgb555(int2 pixel,int2 size) {
    pixel=clamp(pixel,0,size-1);
    // The RDP magic-square pattern repeats in framebuffer coordinates.
    static const int magic[16]={0,6,1,7,4,2,5,3,3,5,2,4,7,1,6,0};
    float threshold=(magic[(pixel.y&3)*4+(pixel.x&3)]+.5)/8;
    return floor(saturate(scene.Load(int3(pixel,0)).rgb)*31+threshold)/31;
}
float4 display_pixel(float4 pos:SV_Position,float2 uv:TEXCOORD):SV_Target {
    uint w,h;scene.GetDimensions(w,h);
    int2 size=int2(w,h);
    float2 texel=uv*size-.5;
    int2 nearest=int2(floor(texel+.5));
    if(abs(ddx(texel.x)-1)<.001 && abs(ddy(texel.y)-1)<.001)
        return float4(rgb555(nearest,size),1);
    int2 i=int2(floor(texel));
    float2 f=smoothstep(.12,.88,frac(texel));
    return float4(lerp(lerp(rgb555(i,size),rgb555(i+int2(1,0),size),f.x),
                       lerp(rgb555(i+int2(0,1),size),rgb555(i+1,size),f.x),f.y),1);
}
)"
