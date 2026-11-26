R"(
cbuffer View : register(b0) {
    float cy,sy,cp,sp,aspect,zoom,distance,road_pass; float3 target; float eye_enabled;
    float4 eye_offset,eye_right,eye_up,eye_forward,projection,car,axes,sun; float isometric;
};
cbuffer Volume : register(b3) {float4 viewport; float2 half_size; float density,forest;};
Texture2DArray<float4> tiles:register(t0);
SamplerState tile_sampler:register(s0);
Texture2D<float> light_depth:register(t16);
SamplerComparisonState shadow_sampler:register(s1);
#if MULTISAMPLE
Texture2DMS<float> view_depth:register(t17);
#else
Texture2D<float> view_depth:register(t17);
#endif
Texture2D<float2> scattering:register(t18);
struct Caster {float4 pos:SV_Position; float3 uv:TEXCOORD; float2 clip:SV_ClipDistance;};
float3 lamp_space(float3 relative) {
    return float3(dot(relative.xz,axes.xy),relative.y-.12,dot(relative.xz,axes.zw)-1.45);
}
float4 lamp_projection(float3 lamp,int index) {
    float w=lamp.z+.65;
    return float4((lamp.x-(index==0?-.85:.85))/.46,(lamp.y+.025*lamp.z)/.22,.16,w);
}
float3 sun_projection(float3 p) {
    float3 right=normalize(float3(sun.z,0,-sun.x));
    float3 up=cross(sun.xyz,right);
    // Anchor the 196-metre shadow map to whole texels in light space.
    float texel=196./512.;
    float2 center=float2(dot(car.xyz,right),dot(car.xyz,up))*sun.w/texel;
    float2 xy=float2(dot(p,right),dot(p,up))+(center-round(center))*texel;
    return float3(xy/98,.5+dot(p,sun.xyz)/450);
}
Caster caster_vertex(float3 position:POSITION,float3 uv:TEXCOORD,uint index:SV_InstanceID) {
    Caster o;float3 rel=(position-car.xyz)*sun.w;o.uv=uv;
    if(forest){o.pos=float4(sun_projection(rel),1);o.clip=1;}
    else {
        o.pos=lamp_projection(lamp_space(rel),index);
        o.clip=o.pos.w+float2(o.pos.x,-o.pos.x);
        o.pos.x=.5*(o.pos.x+(index==0?-o.pos.w:o.pos.w));
    }
    return o;
}
void caster_pixel(Caster p) {if(p.uv.z>0)clip(tiles.SampleLevel(tile_sampler,p.uv,0).a-.5);}
float scene_z(float2 uv) {
    int2 pixel=int2(viewport.xy+min(floor(uv*viewport.zw),ceil(viewport.zw)-1));
#if MULTISAMPLE
    float d=view_depth.Load(pixel,0);
#else
    float d=view_depth.Load(int3(pixel,0));
#endif
    return min(500,isometric?(1-d)*4*sun.w:.0005*sun.w/max(d,1e-7));
}
float visible(float2 ndc,float depth,int lamp) {
    float2 uv=ndc*float2(.5,-.5)+.5;
    if(any(uv<0)||any(uv>=1))return 0;
    if(!forest)uv.x=(uv.x+lamp)*.5;
    return depth>=light_depth.Load(int3(int2(uv*512),0));
}
float headlight(float3 p,int lamp) {
    float2 delta=float2(p.x-(lamp==0?-.85:.85),p.y+.025*p.z);
    float2 width=float2(.22,.16)+p.z*float2(.17,.054);
    float shape=saturate(1-dot(delta/width,delta/width));
    if(shape<=0)return 0;
    float4 proj=lamp_projection(p,lamp);
    return shape*shape*visible(proj.xy/proj.w,.16/max(.16,proj.w-.12),lamp);
}
float2 integrate(float4 pos:SV_Position,float2 uv:TEXCOORD):SV_Target {
    float3 right=float3(cy,0,sy),up=float3(sy*sp,cp,-cy*sp),forward=float3(-sy*cp,sp,cy*cp);
    float2 slope=(uv*float2(2,-2)+float2(-1,1))*float2(aspect,1)/zoom;
    float3 eye=(target-car.xyz)*sun.w-forward*distance*sun.w;
    float3 ray=forward;
    if(eye_enabled) {
        slope=(uv*float2(2,-2)+float2(-1,1)-projection.zw)/projection.xy;
        float3 dir=eye_right.xyz*slope.x+eye_up.xyz*slope.y+eye_forward.xyz;
        ray=right*dir.x+up*dir.y+forward*dir.z;
        eye+=sun.w*(right*eye_offset.x+up*eye_offset.y+forward*eye_offset.z);
    } else if(isometric)eye+=(slope.x*right+slope.y*up)*distance*sun.w;
    else ray+=slope.x*right+slope.y*up;
    float depth=scene_z(uv),lo=0,hi=depth;
    float3 origin=eye,step_dir=ray;
    if(!forest) {
        origin=lamp_space(eye);step_dir=lamp_space(eye+ray)-origin;
        // Clip the view ray to the union's bounds before sampling either beam.
        float3 inv=1/(step_dir+float3(step_dir.x>=0?1:-1,step_dir.y>=0?1:-1,step_dir.z>=0?1:-1)*.000001);
        float3 a=(float3(-13,-6,0)-origin)*inv,b=(float3(13,4,65)-origin)*inv;
        float3 enter=min(a,b),leave=max(a,b);
        lo=max(0,max(enter.x,max(enter.y,enter.z)));hi=min(depth,min(leave.x,min(leave.y,leave.z)));
    } else hi=min(hi,82);
    if(hi<=lo)return float2(0,depth);
    float dt=(hi-lo)/24,sum=0;
    [loop] for(int i=0;i<24;++i) {
        float travel=lo+(i+.5)*dt;
        float3 p=origin+travel*step_dir;
        if(forest) {
            float3 q=sun_projection(p);
            float2 uv_shadow=q.xy*float2(.5,-.5)+.5;
            float shadow=light_depth.SampleCmpLevelZero(shadow_sampler,uv_shadow,q.z+.001);
            float fade=smoothstep(.82,1,max(abs(q.x),abs(q.y)));
            sum+=lerp(shadow,1,fade)*saturate(1-travel/82);
        } else sum+=(headlight(p,0)+headlight(p,1))*(1-smoothstep(14,65,p.z));
    }
    float gain=forest?.15+.85*pow(saturate(dot(normalize(ray),sun.xyz)),2):1;
    return float2(1-exp(-sum*dt*length(ray)*density*gain),depth);
}
float4 composite(float4 pos:SV_Position,float2 uv:TEXCOORD):SV_Target {
    float depth=scene_z(uv);
    float2 cell=uv*half_size-.5,f=frac(cell);
    float total=0,weight=0;
    [unroll] for(int j=0;j<2;++j)[unroll] for(int i=0;i<2;++i) {
        int2 xy=clamp(int2(floor(cell))+int2(i,j),0,int2(half_size)-1);
        float2 v=scattering.Load(int3(xy,0));
        float w=(i?f.x:1-f.x)*(j?f.y:1-f.y)/ (1+4*abs(v.y-depth));
        total+=w*v.x;weight+=w;
    }
    float amount=total/max(.2,weight);
    float3 color=forest?lerp(float3(1,.61,.34),float3(1,.90,.71),saturate(sun.y)):float3(1,.87,.64);
    return float4(amount*color,amount*.55);
}
)"
