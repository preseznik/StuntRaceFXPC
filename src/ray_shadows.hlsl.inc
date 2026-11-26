R"(
#if RAY_SUN_SHADOWS || RAY_WET_REFLECTIONS || RAY_INDIRECT
struct RayNode { float3 low; uint end; float3 high; uint first, count; };
struct RayFace { float3 a, ab, ac; float2 uv, uv_ab, uv_ac; float tile_layer; float3 color; };
StructuredBuffer<RayNode> ray_nodes : register(t10);
StructuredBuffer<RayFace> ray_faces : register(t11);

bool ray_box(RayNode node,float3 origin,float3 inverse,float limit) {
    float3 a=(node.low-origin)*inverse,b=(node.high-origin)*inverse;
    float3 near_t=min(a,b),far_t=max(a,b);
    return min(limit,min(far_t.x,min(far_t.y,far_t.z)))>=max(0,max(near_t.x,max(near_t.y,near_t.z)));
}

bool ray_triangle(RayFace f,float3 origin,float3 direction,float limit,out float t,out float3 uv,out float2 bary) {
    t=0;uv=0;bary=0;
    float3 p=cross(direction,f.ac);
    float det=dot(f.ab,p);
    if(abs(det)<1e-14)return false;
    float3 delta=origin-f.a,q=cross(delta,f.ab);
    float u=dot(delta,p)/det,v=dot(direction,q)/det;
    t=dot(f.ac,q)/det;
    if(u<0 || v<0 || u+v>1 || t<=0 || t>=limit)return false;
    bary=float2(u,v);
    uv=float3(f.uv+u*f.uv_ab+v*f.uv_ac,f.tile_layer);
    return f.tile_layer<=0 || tiles.SampleLevel(tile_sampler,uv,0).a>=.5;
}

#if RAY_SUN_SHADOWS || RAY_INDIRECT
bool blocks_sun(float3 origin,float3 direction,uint node_count) {
    float3 inverse=(step(0,direction)*2-1)/max(abs(direction),1e-10);
    uint i=0;
    [loop] while(i<node_count) {
        RayNode node=ray_nodes[i];
        if(!ray_box(node,origin,inverse,1e20)) {i=node.end;continue;}
        [loop] for(uint j=0;j<node.count;++j) {
            float t;float3 uv;float2 bary;
            if(ray_triangle(ray_faces[node.first+j],origin,direction,1e20,t,uv,bary))return true;
        }
        ++i;
    }
    return false;
}

#if RAY_SUN_SHADOWS
float traced_sunlight(float3 position,float3 normal) {
    if(sun.y<=.01 || dot(normal,sun.xyz)<=0)return 1;
    uint nodes,stride;
    ray_nodes.GetDimensions(nodes,stride);
    // Position is normalized to the course. Convert a centimetre to those units.
    float3 origin=position+normal*(.01/max(1,sun.w));
#if SUN_RAYS == 1
    return blocks_sun(origin,sun.xyz,nodes)?0:1;
#else
    float3 horizontal=normalize(float3(sun.z,0,-sun.x));
    float3 vertical=cross(sun.xyz,horizontal);
    // Fixed, symmetric positions on the solar disc keep edges stable in motion.
    float visible=0;
    [unroll] for(uint sample_index=0;sample_index<SUN_RAYS;++sample_index) {
        float2 offset=float2((sample_index&1)?1:-1,(sample_index&2)?1:-1);
#if SUN_RAYS == 2
        offset.y=offset.x;
#endif
        float3 direction=sun.xyz+.0024*(horizontal*offset.x+vertical*offset.y);
        visible+=blocks_sun(origin,direction,nodes)?0:1.0/SUN_RAYS;
    }
    return visible;
#endif
}
#endif
#endif

#if RAY_WET_REFLECTIONS || RAY_INDIRECT
StructuredBuffer<RayNode> actor_nodes : register(t12);
StructuredBuffer<RayFace> actor_faces : register(t13);
struct SceneHit { float t; float3 uv, color, normal; };

// The static ray mesh outlives the day-to-dusk transition. Its V coordinate
// identifies the native road swatch used by the current frame.
float3 hit_color(SceneHit hit) {
    if(palette_pavement(hit.uv.z) && hit.uv.y>=0) {
        uint packed=colors.Load(int3(528+int(round(hit.uv.y)),0,0));
        return float3(packed&31,(packed>>5)&31,(packed>>10)&31)/31;
    }
    return hit.color;
}

void closest_surface(StructuredBuffer<RayNode> nodes,StructuredBuffer<RayFace> faces,
                     float3 origin,float3 direction,float3 inverse,inout SceneHit hit) {
    uint end=nodes[0].end; // A null SRV reads zero when there are no actors.
    uint i=0;
    [loop] while(i<end) {
        RayNode node=nodes[i];
        if(!ray_box(node,origin,inverse,hit.t)) {i=node.end;continue;}
        [loop] for(uint j=0;j<node.count;++j) {
            RayFace f=faces[node.first+j];
            float t;float3 uv;float2 bary;
            if(ray_triangle(f,origin,direction,hit.t,t,uv,bary)) {
                hit.t=t;
                hit.uv=uv;
                hit.color=f.color;
#if ROAD_DETAIL
                if(f.tile_layer==-26)hit.color.r=dot(f.color,float3(1-bary.x-bary.y,bary));
#endif
                hit.normal=cross(f.ab,f.ac);
            }
        }
        ++i;
    }
}

#endif

#if RAY_WET_REFLECTIONS
float3 wet_road_reflection(float3 road,float3 position,float3 normal,float3 to_eye,float wet,float2 road_uv,bool detailed_pavement) {
#if MATERIAL_ROUGHNESS
    float3 direction=reflect(-to_eye,normal);
#else
    // Small world-anchored variations break up the image on coarse wet pavement.
    float2 slope=float2(grain(road_uv*3.7),grain(road_uv*3.7+19.1))-.5;
    float3 direction=reflect(-to_eye,normalize(normal+float3(slope.x,0,slope.y)*.012));
#endif
#if ROAD_DETAIL && !MATERIAL_ROUGHNESS
    // Detailed pavement already supplies its worn or pebbled film normal.
    if(detailed_pavement)direction=reflect(-to_eye,normal);
#endif
    if(dot(normal,to_eye)<0)normal=-normal;
    float3 origin=position+normal*(.015/sun.w);
    float3 inverse=(step(0,direction)*2-1)/max(abs(direction),1e-10);
    float limit=180/sun.w;
    SceneHit hit=(SceneHit)0;
    hit.t=limit;
    closest_surface(actor_nodes,actor_faces,origin,direction,inverse,hit);
    closest_surface(ray_nodes,ray_faces,origin,direction,inverse,hit);
    if(hit.t>=limit)return road;
    float3 albedo=hit_color(hit);
    float across=albedo.r;
#if ROAD_DETAIL
    across=road_coordinates(hit.uv,albedo,(origin+direction*hit.t).xz*sun.w);
#endif
    if(hit.uv.z<0)albedo=scene_material(hit.uv,albedo,weather_seconds,across);
    else if(hit.uv.z>0)albedo*=tiles.SampleLevel(tile_sampler,hit.uv,0).rgb;
    if((hit.uv.z==-33 || hit.uv.z < -36.5) && panorama_fog>0)albedo=lerp(albedo,panorama_fog_color.rgb,panorama_fog);
    float3 n=normalize(hit.normal);
    if(dot(n,direction)>0)n=-n;
    float3 light=ambient_color(sun.xyz)*(.83+.17*saturate(n.y))+
        sun_color(sun.xyz)*saturate(dot(n,sun.xyz))*.63*smoothstep(-.08,.17,sun.y)*(1-.89*cloud_cover);
    float meters=hit.t*sun.w;
    float grazing=1-saturate(dot(normal,to_eye));
    float patches=.55+.45*grain(road_uv*.29);
    float weight=wet*patches*(.24+.57*grazing*grazing)*(1-smoothstep(100,180,meters));
    return lerp(road,air_perspective(albedo*light,meters,sun.xyz),weight);
}
#endif
#endif
)"
