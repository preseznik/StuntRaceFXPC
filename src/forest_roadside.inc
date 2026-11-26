// Shared Remastered roadside scenery. Positions and dimensions use native course units (Y down).
namespace forest_roadside {
using n64::Vec;
constexpr float tau=6.2831853f;

constexpr float verge_width=1800;
struct Edge {
    Vec a,b;
    float distance_squared(Vec p) const {
        const float dx=b[0]-a[0],dy=b[1]-a[1],dz=b[2]-a[2];
        const float x=p[0]-a[0],y=p[1]-a[1],z=p[2]-a[2];
        const float t=std::clamp((x*dx+y*dy+z*dz)/std::max(1.f,dx*dx+dy*dy+dz*dz),0.f,1.f);
        const float rx=x-t*dx,ry=y-t*dy,rz=z-t*dz;
        return rx*rx+ry*ry+rz*rz;
    }
};

float verge_distance(std::span<const Edge> edges,Vec p) {
    float closest=verge_width*verge_width;
    for(const auto& edge:edges)closest=std::min(closest,edge.distance_squared(p));
    return std::sqrt(closest)/verge_width;
}

// Extract the outside edges of the paved polygons once per course load.
// Ground distance includes height so shoulders fade beneath the raised approaches.
std::vector<Edge> road_edges(const srf::World& world,const std::map<unsigned,const srf::WorldModel*>& models,
                             std::map<unsigned,std::vector<float>>& banks,bool (*paved)(unsigned,unsigned)) {
    std::vector<Edge> edges;
    for(const auto& instance:world.instances) {
        auto model=models.find(instance.visual_descriptor);
        if(instance.opcode!=2 || model==models.end())continue;
        const auto& surface=*model->second;
        const auto& vertices=surface.frames.front();
        std::map<std::pair<unsigned,unsigned>,unsigned> uses;
        for(const auto& face:surface.primitives) {
            if(face.indices.size()<3 || !paved(instance.opcode,face.attributes[2]))continue;
            for(size_t i=0;i<face.indices.size();++i)
                ++uses[std::minmax(face.indices[i],face.indices[(i+1)%face.indices.size()])];
        }
        if(uses.empty())continue;
        auto position=[&](unsigned i) {
            const auto p=srf::world_position(vertices.at(i),instance);
            return Vec{float(p[0]),float(p[1]),float(p[2])};
        };
        const auto first=edges.size();
        for(const auto& [pair,count]:uses)if(count==1)
            edges.push_back({position(pair.first),position(pair.second)});
        // All placements of this descriptor share the same bank geometry.
        auto [bank,inserted]=banks.try_emplace(instance.visual_descriptor);
        if(inserted)for(unsigned i=0;i<vertices.size();++i)
            bank->second.push_back(1.8f*verge_distance(std::span(edges).subspan(first),position(i)));
    }
    return edges;
}

// Refine just the meadow tiles near a road edge. Replace those triangles so
// the shoulder blends into grass without overlapping the existing terrain.
template<class Vertex>
bool verge_tile(std::vector<GpuVertex>& mesh,int x,int z,int step,std::span<const Edge> edges,const Vertex& vertex) {
    std::vector<Edge> nearby;
    const float reach=verge_width+.708f*step;
    for(const auto& edge:edges)
        if(edge.distance_squared({x+.5f*step,8,z+.5f*step})<=reach*reach)nearby.push_back(edge);
    if(nearby.empty())return false;
    auto sample=[&](int px,int pz) {
        auto v=vertex(px,pz);
        v.texture=-39;v.r=verge_distance(nearby,{float(px),8,float(pz)});
        return v;
    };
    const int spacing=step/4;
    for(int dz=0;dz<step;dz+=spacing)for(int dx=0;dx<step;dx+=spacing) {
        const auto a=sample(x+dx,z+dz),b=sample(x+dx+spacing,z+dz);
        const auto c=sample(x+dx+spacing,z+dz+spacing),d=sample(x+dx,z+dz+spacing);
        mesh.insert(mesh.end(),{a,b,c,a,c,d});
    }
    return true;
}

// A single original cutout tile supplies leaf-scale detail in colour, depth
// and ray queries. Build it locally once per scene, with transparent margins.
void foliage_tile(std::uint32_t* pixels) {
    constexpr int side=128;
    unsigned seed=8171;
    auto random=[&] {seed=1664525u*seed+1013904223u;return float(seed>>8)/16777216.f;};
    for(int i=0;i<940;++i) {
        const float angle=tau*random(),r=std::sqrt(random());
        const float radius=49+5*std::sin(5*angle)+4*std::sin(9*angle+.6f);
        const float cx=64+radius*r*std::cos(angle),cy=64+radius*r*std::sin(angle);
        const float tilt=angle+.9f*(random()-.5f),dx=std::cos(tilt),dy=std::sin(tilt);
        const float length=2+3*random(),width=1.3f+1.8f*random();
        const float light=.60f+.44f*random()+.18f*(1-cy/128);
        for(int y=int(cy)-6;y<=int(cy)+6;++y)for(int x=int(cx)-6;x<=int(cx)+6;++x) {
            const float u=((x-cx)*dx+(y-cy)*dy)/length,v=(-(x-cx)*dy+(y-cy)*dx)/width;
            if(std::abs(v)+u*u>1)continue;
            const float shade=light*(v<0?.87f:1.10f);
            pixels[y*side+x]=0xff000000u | (unsigned(54*shade)<<16) | (unsigned(139*shade)<<8) | unsigned(83*shade);
        }
    }
}

template<class Normalize>
void tree(std::vector<GpuVertex>& mesh,const srf::WorldInstance& instance,float foliage_layer,const Normalize& normalize) {
    unsigned seed=instance.command_offset;
    auto random=[&] {seed=1664525u*seed+1013904223u;return float(seed>>8)/16777216.f;};
    const auto origin=instance.position;
    // The small native canopy has half the diameter and sits half as high.
    const float scale=instance.visual_descriptor==0xa0cc?.5f:1.f;
    auto vertex=[&](Vec p,Vec color,float u,float v,float material) {
        const auto q=normalize(Vec{origin.x+p[0]*scale,origin.y+p[1]*scale,origin.z+p[2]*scale});
        return GpuVertex{q[0],q[1],q[2],color[0],color[1],color[2],u,v,material};
    };
    auto limb=[&](Vec a,Vec b,float base,float tip) {
        constexpr unsigned sides=8;
        auto ring=[&](Vec center,float radius,unsigned i) {
            float angle=tau*(i%sides)/sides;
            center[0]+=radius*std::cos(angle);center[2]+=radius*std::sin(angle);
            return vertex(center,{.34f,.24f,.14f},float(i)*1.5f,-center[1]/130,-37);
        };
        for(unsigned i=0;i<sides;++i) {
            const auto p=ring(a,base,i),q=ring(a,base,i+1),r=ring(b,tip,i+1),s=ring(b,tip,i);
            mesh.insert(mesh.end(),{p,q,r,p,r,s});
        }
    };
    // Each spray has three curved, intersecting leaf cards around the branch.
    // Their inclination and shape are fixed in tree space as the camera turns.
    auto spray=[&](Vec center,float radius,float height,float tint) {
        const float twist=tau*random();
        for(unsigned plane=0;plane<3;++plane) {
            const float angle=twist+plane*tau/3,dx=std::cos(angle),dz=std::sin(angle);
            const float tilt=.42f+.35f*random();
            auto leaf=[&](float u,float v,float bulge) {
                const float depth=v*radius*tilt+bulge;
                Vec p{center[0]+u*radius*dx-depth*dz,center[1]+v*height,center[2]+u*radius*dz+depth*dx};
                const float light=tint*(1.02f-.18f*v);
                auto result=vertex(p,{light,light,light},u*.5f+.5f,v*.5f+.5f,foliage_layer);
                result.finish=SurfaceFinish::foliage;
                return result;
            };
            const auto a=leaf(-1,-1,0),b=leaf(1,-1,0),c=leaf(1,1,0),d=leaf(-1,1,0),mid=leaf(0,0,.45f*radius);
            mesh.insert(mesh.end(),{a,b,mid,b,c,mid,c,d,mid,d,a,mid});
        }
    };
    Vec foot{0,12,0},fork{24,-740,-35},leader{-58,-1800,50};
    limb(foot,{12,-160,-16},190,115);limb({12,-160,-16},fork,115,88);limb(fork,leader,88,39);
    const float rotation=tau*random();
    for(unsigned i=0;i<13;++i) {
        float a=rotation+i*2.4f;
        float reach=700-30.f*i+90*random();
        Vec end{reach*std::cos(a),-1140-76.f*i-80*random(),reach*std::sin(a)};
        Vec start{fork[0],fork[1]-65.f*i,fork[2]};
        limb(start,end,68-2.f*i,25);
        float tint=.82f+.38f*random();
        spray(end,310+75*random(),310+85*random(),tint);
        for(unsigned j=0;j<3;++j) {
            float f=a+j*tau/3;
            spray({end[0]+205*std::cos(f),end[1]-145*random(),end[2]+205*std::sin(f)},
                  165+85*random(),185+80*random(),tint*(.86f+.24f*random()));
        }
    }
    spray({-45,-2200,25},390,410,1.04f);
}
} // namespace forest_roadside
