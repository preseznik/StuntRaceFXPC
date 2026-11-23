namespace n64 {
constexpr bool is_crest(unsigned descriptor) {
    return descriptor==0xed94 || descriptor==0xede8;
}

constexpr bool asphalt_material(unsigned descriptor,unsigned material) {
    if(is_crest(descriptor))return material>=0x2a && material<=0x2e;
    return material==0x2c || material==0x2d;
}

constexpr bool graded_road(unsigned opcode,unsigned material) {
    return opcode==2 && material>=0x2a && material<=0x2f;
}

constexpr bool sea_breeze_road(unsigned opcode,unsigned material) {
    return graded_road(opcode,material) || (opcode==2 && material==0x29);
}

constexpr bool aqua_tunnel_rib(unsigned descriptor) {
    return descriptor==0xd46c || descriptor==0xd450 || descriptor==0xeffc;
}

constexpr bool aqua_driving_surface(unsigned descriptor,unsigned opcode,unsigned material) {
    if(opcode!=2)return false;
    // The submerged arches use the regular road palette on their ceilings.
    // Narrow deck ribs and the two solid floor sections carry different indices.
    if(aqua_tunnel_rib(descriptor))return material==0x73 || material==0xde;
    return material>=0x29 && material<=0x2f;
}

// The transparent deck is defined by edge lines and opaque cross ribs. Tie
// the glass panel seams to their endpoints, including around the curved bays.
template<class Point>
void aqua_panel_seams(std::vector<GpuVertex>& mesh,unsigned descriptor,const Point& point) {
    using Bay=std::array<unsigned,4>; // near left/right, far left/right
    constexpr Bay straight[]={{2,3,5,4},{11,10,30,31},{36,37,42,43}};
    constexpr Bay curved[]={{7,6,22,13},{22,13,41,15},{44,18,52,24},
                             {52,24,55,25},{57,27,61,29},{61,29,62,30}};
    const auto bays=descriptor==0xd450?std::span<const Bay>(curved):std::span<const Bay>(straight);
    for(const auto& bay:bays) {
        const auto a=point(bay[0]),b=point(bay[1]),c=point(bay[2]),d=point(bay[3]);
        auto vertex=[&](float across,float along) {
            Vec p;
            for(unsigned i=0;i<3;++i)p[i]=std::lerp(std::lerp(a[i],b[i],across),std::lerp(c[i],d[i],across),along);
            return GpuVertex{p[0],p[1],p[2],.34f,.59f,.62f,0,0,-5};
        };
        auto strip=[&](float l,float r,float start,float end) {
            const auto v0=vertex(l,start),v1=vertex(r,start),v2=vertex(r,end),v3=vertex(l,end);
            mesh.insert(mesh.end(),{v0,v1,v2,v0,v2,v3,v2,v1,v0,v3,v2,v0});
        };
        strip(0,.008f,0,1);
        strip(.992f,1,0,1);
        for(float u:{.25f,.75f})strip(u-.0015f,u+.0015f,0,1);
        strip(0,1,.496f,.504f);
    }
}

// A road face supplies a local grade and a footprint. In original world units
// Y increases downwards. The lower envelope keeps the underpass open.
struct RoadGrade {
    float x0,x1,z0,z1,y0,y1,dx,dz,intercept;
};

template<class Normalize>
void level_terrain(std::vector<GpuVertex>& mesh,float ground_y,float material,const Normalize& normalize,Vec tint={1,.60f,0}) {
    constexpr int edge=49152,step=2048;
    auto vertex=[&](int x,int z) {
        const auto p=normalize(Vec{float(x),ground_y,float(z)});
        return GpuVertex{p[0],p[1],p[2],tint[0],tint[1],tint[2],x/90.f,z/90.f,material};
    };
    // N64 distance fog is evaluated at vertices, including on flat ground.
    for(int z=-edge;z<edge;z+=step)for(int x=-edge;x<edge;x+=step) {
        const auto a=vertex(x,z),b=vertex(x+step,z),c=vertex(x+step,z+step),d=vertex(x,z+step);
        mesh.insert(mesh.end(),{a,b,c,a,c,d});
    }
}

template<class Normalize>
void coastal_terrain(std::vector<GpuVertex>& mesh,const srf::World& world,
                    const std::map<unsigned,const srf::WorldModel*>& models,const Normalize& normalize) {
    std::vector<RoadGrade> roads;
    float xmin=1e9f,xmax=-1e9f,zmin=1e9f,zmax=-1e9f,lowest=-1e9f;
    for(const auto& instance:world.instances) {
        auto model=models.find(instance.visual_descriptor);
        if(model==models.end())continue;
        const auto& vertices=model->second->frames.front();
        const bool crest=is_crest(instance.visual_descriptor);
        for(const auto& face:model->second->primitives) {
            const unsigned material=face.attributes[2];
            const bool crest_bank=crest && material>=0x55 && material<=0x59;
            const bool forecourt=instance.visual_descriptor==fuel_station && material==0x01;
            if(face.indices.size()<3 || !(asphalt_material(instance.visual_descriptor,material) || crest_bank || forecourt))continue;
            auto point=[&](unsigned i) {
                auto p=srf::world_position(vertices[face.indices[i]],instance);
                // Both halves include the entire raised embankment. Join the
                // surrounding meadow to its foot, all the way to the outer verge.
                if(crest)p[1]=instance.position.y;
                return p;
            };
            const auto a=point(0);
            for(unsigned i=1;i+1<face.indices.size();++i) {
                const auto b=point(i),c=point(i+1);
                const auto ux=b[0]-a[0],uy=b[1]-a[1],uz=b[2]-a[2];
                const auto vx=c[0]-a[0],vy=c[1]-a[1],vz=c[2]-a[2];
                const auto det=ux*vz-uz*vx;
                if(std::abs(det)<1)continue;
                const auto dx=(uy*vz-uz*vy)/det,dz=(ux*vy-uy*vx)/det;
                const RoadGrade road{float(std::min({a[0],b[0],c[0]})),float(std::max({a[0],b[0],c[0]})),
                    float(std::min({a[2],b[2],c[2]})),float(std::max({a[2],b[2],c[2]})),
                    float(std::min({a[1],b[1],c[1]})),float(std::max({a[1],b[1],c[1]})),
                    float(dx),float(dz),float(a[1]-dx*a[0]-dz*a[2])};
                roads.push_back(road);
                xmin=std::min(xmin,road.x0);xmax=std::max(xmax,road.x1);
                zmin=std::min(zmin,road.z0);zmax=std::max(zmax,road.z1);lowest=std::max(lowest,road.y1);
            }
        }
    }
    if(roads.empty())return;
    constexpr float step=560,margin=13440;
    const float sea_level=lowest+630;
    auto smooth=[](float x){x=std::clamp(x,0.f,1.f);return x*x*(3-2*x);};
    auto vertex=[&](float x,float y,float z,float shade,float sand,float material) {
        const auto p=normalize(Vec{x,y,z});
        return GpuVertex{p[0],p[1],p[2],shade,sand,0,x/90,z/90,material};
    };
    const unsigned cols=unsigned(std::ceil((xmax-xmin+margin*2)/step))+1;
    const unsigned rows=unsigned(std::ceil((zmax-zmin+margin*2)/step))+1;
    std::vector<GpuVertex> grid;
    grid.reserve(rows*cols);
    for(unsigned row=0;row<rows;++row)for(unsigned col=0;col<cols;++col) {
        const float x=xmin-margin+col*step,z=zmin-margin+row*step;
        float nearest=1e20f,nearest_y=0,safe_y=-1e9f;
        for(const auto& road:roads) {
            const float px=std::clamp(x,road.x0,road.x1),pz=std::clamp(z,road.z0,road.z1);
            const float distance=(x-px)*(x-px)+(z-pz)*(z-pz);
            const float y=std::clamp(road.dx*px+road.dz*pz+road.intercept,road.y0,road.y1);
            if(distance<nearest) {nearest=distance;nearest_y=y;}
            // Every cell touching pavement must remain below its steepest grade,
            // including cells at the joint between adjacent road pieces.
            if(distance<=2*step*step)
                safe_y=std::max(safe_y,std::min(road.y1,y+step*(std::abs(road.dx)+std::abs(road.dz))));
        }
        const float distance=std::sqrt(nearest);
        const float rolls=.55f*std::sin(x/1590+std::sin(z/2100))+.45f*std::cos(z/1150-x/3600);
        const float shore=smooth((distance+1050*std::sin(x/2900+z/3700)-5500)/5300);
        float y=std::max(nearest_y,safe_y)+18-360*rolls*smooth((distance-800)/1900);
        y=std::lerp(y,sea_level+270,shore);
        grid.push_back(vertex(x,y,z,.94f+.11f*rolls,smooth((y-sea_level+320)/230),-9));
    }
    for(unsigned r=1;r<rows;++r)for(unsigned c=1;c<cols;++c) {
        const unsigned i=r*cols+c;
        for(auto j:{i-cols-1,i-cols,i,i-cols-1,i,i-1})mesh.push_back(grid[j]);
    }
    // A level water surface intersects the beach and has parallax as the car turns.
    constexpr float horizon=480000;
    for(auto xz:{Vec{-1,0,-1},Vec{1,0,-1},Vec{1,0,1},Vec{-1,0,-1},Vec{1,0,1},Vec{-1,0,1}})
        mesh.push_back(vertex(xz[0]*horizon,sea_level,xz[2]*horizon,1,0,-10));
}
}
