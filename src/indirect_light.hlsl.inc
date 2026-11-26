R"(
#if RAY_INDIRECT
// Every course shares a fixed lattice centred on its normalized world bounds.
// The four RGB coefficients describe the mean incident light and its first
// directional moments. Cosine filtering reconstructs diffuse irradiance.
float3 probe_first(float3 dims) {
    return -.5*(dims-1);
}

#ifdef COMPUTE_PROBES
RWTexture3D<float4> probe_output : register(u0);
float3 bounce_albedo(SceneHit hit) {
    // Diffuse transport needs the area-average colour, including synthetic
    // materials whose vertex colours encode parameters rather than RGB.
    float id=hit.uv.z;
    if(id==-1 || id==-21 || id==-26 || id==-32)return float3(.305,.322,.325);
    if(id==-2 || id==-6 || id==-7 || id==-29 || id==-30)return float3(.56,.54,.48);
    if(id==-31)return float3(.23,.29,.30); // steel deck tread
    if(id==-43 || id==-50) {
        float3 wall=hit.color;
        if(id==-50 && hit.uv.y<.68)wall*=float3(.32,.46,.40);
        return lerp(wall,float3(.56,.55,.49),.43)*.94;
    }
    if(id==-44)return lerp(float3(.31,.39,.42),hit.color,.23);
    if(id==-45 || id==-47)return float3(.40,.385,.34)*(id==-47?1-.36*hit.color.r:1);
    if(id==-48)return lerp(hit.color,float3(.35,.22,.14),.20);
    if(id==-49)return hit.color*.98;
    if(id==-41 || id==-42)return hit.color*.94; // canyon bedrock and scree
    if(id==-40)return lerp(float3(.43,.38,.28),lerp(float3(.38,.47,.25)*hit.color.r,float3(.70,.65,.49),hit.color.g),hit.color.b);
    if(id==-39)return lerp(float3(.43,.38,.28),float3(.35,.45,.23),hit.color.r);
    if(id==-3)return float3(.35,.45,.23);
    if(id==-10)return float3(.37,.59,.60);
    if(id==-9)return lerp(float3(.38,.47,.25)*hit.color.r,float3(.70,.65,.49),hit.color.g);
    if(id>0)return tiles.SampleLevel(tile_sampler,hit.uv,0).rgb*hit.color;
    return saturate(hit_color(hit));
}
[numthreads(4,4,4)]
void trace_probes(uint3 cell:SV_DispatchThreadID) {
    uint w,h,d;
    probe_output.GetDimensions(w,h,d);
    uint depth=d/4;
    float3 origin=(probe_first(float3(w,h,depth))+cell)*probe_step.xyz/sun.w;
    {
        // Move buried probes above the road or its underlying terrain.
        [loop] for(int surface=0;surface<2;++surface) {
            SceneHit above=(SceneHit)0;
            float reach=80/sun.w;
            above.t=reach;
            closest_surface(ray_nodes,ray_faces,origin,float3(0,1,0),float3(1e10,1,1e10),above);
            float m=above.uv.z;
            if(above.t>=reach || !(m==-3 || m==-39 || m==-1 || m==-21 || m==-26 || m==-32 || m==-33))break;
            origin.y+=above.t+1/sun.w;
        }
    }
    float3 sky=ambient_color(sun.xyz);
    float3 sunlight=sun_color(sun.xyz)*smoothstep(-.08,.17,sun.y)*(1-.89*cloud_cover);
    uint node_count=ray_nodes[0].end;
    float3 mean=0,mx=0,my=0,mz=0;
    const uint samples=24;
    [loop] for(uint i=0;i<samples;++i) {
        // A fixed spherical Fibonacci sequence keeps the lighting stable in motion.
        float y=1-2*(i+.5)/samples;
        float a=i*2.39996323;
        float3 dir=float3(cos(a)*sqrt(1-y*y),y,sin(a)*sqrt(1-y*y));
        float3 inv=(step(0,dir)*2-1)/max(abs(dir),1e-10);
        SceneHit hit=(SceneHit)0;
        float limit=68/sun.w;
        hit.t=limit;
        closest_surface(ray_nodes,ray_faces,origin,dir,inv,hit);
        float3 radiance=sky*(.85+.32*y);
        if(hit.t<limit) {
            float3 n=normalize(hit.normal);
            if(dot(n,dir)>0)n=-n;
            float cosine=saturate(dot(n,sun.xyz));
            float3 p=origin+dir*hit.t+n*(.025/sun.w);
            if(cosine>0 && sun.y>.01 && blocks_sun(p,sun.xyz,node_count))cosine=0;
            float3 bounced=bounce_albedo(hit)*(sky*.43+sunlight*cosine);
            radiance=lerp(bounced,radiance,smoothstep(54,68,hit.t*sun.w));
        }
        mean+=radiance; mx+=dir.x*radiance; my+=dir.y*radiance; mz+=dir.z*radiance;
    }
    probe_output[cell]=float4(mean/samples,1);
    probe_output[cell+uint3(0,0,depth)]=float4(2*mx/samples,0);
    probe_output[cell+uint3(0,0,2*depth)]=float4(2*my/samples,0);
    probe_output[cell+uint3(0,0,3*depth)]=float4(2*mz/samples,0);
}
#else
Texture3D<float4> probes : register(t14);
SamplerState probe_sampler : register(s2);
float3 indirect_ambient(float3 base,float3 pos,float3 normal) {
    uint w,h,d;
    probes.GetDimensions(w,h,d);
    float3 dims=float3(w,h,d/4);
    float3 cell=(pos*sun.w+normal*1.52)/probe_step.xyz-probe_first(dims);
    float3 uv=(clamp(cell,0,dims-1)+.5)/float3(w,h,d);
    float3 irradiance=probes.SampleLevel(probe_sampler,uv,0).rgb;
    irradiance+=normal.x*probes.SampleLevel(probe_sampler,uv+float3(0,0,.25),0).rgb;
    irradiance+=normal.y*probes.SampleLevel(probe_sampler,uv+float3(0,0,.50),0).rgb;
    irradiance+=normal.z*probes.SampleLevel(probe_sampler,uv+float3(0,0,.75),0).rgb;
    // The existing renderer stores display-encoded fill. Blend energies so
    // the native subtractive tunnel palette retains readable road and car detail.
    float3 traced=max(irradiance,0);
    return sqrt(lerp(base*base,traced*traced,.72));
}
#endif
#endif
)"
