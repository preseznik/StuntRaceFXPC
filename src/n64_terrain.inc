namespace n64 {
// Reuse the footprint of each paint quad, keeping both ends flush with the
// adjoining strip. The middle 68% of the curb is a raised, flat crown.
void beveled_curb(std::vector<GpuVertex>& triangles,std::array<GpuVertex,4> q,float height) {
    auto length=[](const GpuVertex& a,const GpuVertex& b){return std::hypot(a.x-b.x,a.z-b.z);};
    if(length(q[0],q[1])>length(q[1],q[2]))std::rotate(q.begin(),q.begin()+1,q.end());
    auto inset=[height](GpuVertex a,const GpuVertex& b) {
        a.x=std::lerp(a.x,b.x,.16f); a.z=std::lerp(a.z,b.z,.16f); a.y+=height;
        a.u=std::lerp(a.u,b.u,.16f); a.v=std::lerp(a.v,b.v,.16f);
        return a;
    };
    auto panel=[&](GpuVertex a,GpuVertex b,GpuVertex c,GpuVertex d,float material) {
        a.texture=b.texture=c.texture=d.texture=material;
        triangles.insert(triangles.end(),{a,b,c,a,c,d});
    };
    auto a=inset(q[0],q[1]), b=inset(q[1],q[0]), c=inset(q[2],q[3]), d=inset(q[3],q[2]);
    panel(q[0],a,d,q[3],-28);
    panel(a,b,c,d,-27);
    panel(b,q[1],q[2],c,-28);
}

// Signed distance to the closed driving line, in the same units as road UVs.
float road_offset(const std::array<double,3>& p,std::span<const srf::ScenePoint> route,bool match_height) {
    double best=DBL_MAX,offset=0;
    for(size_t i=0;i<route.size();++i) {
        const auto& a=route[i].position;
        const auto& b=route[(i+1)%route.size()].position;
        const double dx=b.x-a.x,dz=b.z-a.z,length2=dx*dx+dz*dz;
        if(length2==0)continue;
        const double x=p[0]-a.x,z=p[2]-a.z;
        const double t=std::clamp((x*dx+z*dz)/length2,0.,1.);
        const double rx=x-t*dx,rz=z-t*dz,d2=rx*rx+rz*rz;
        // The flyover courses cross their own roads at different elevations. Select the
        // deck in 3D, but measure the wheel paths across the road in plan view.
        const double height=match_height ? p[1]-std::lerp(double(a.y),double(b.y),t) : 0.;
        const double distance2=d2+height*height;
        if(distance2<best){best=distance2;offset=std::copysign(std::sqrt(d2),dz*x-dx*z);}
    }
    return float(offset/90.);
}

constexpr bool road_curb(unsigned descriptor,unsigned face,unsigned material,bool linked_turns) {
    if(material!=0x1e && material!=0x32)return false;
    // On Night Cruise these five bends use red and white exclusively on the edge strips.
    if(descriptor==0xf280 || descriptor==0xf29c || descriptor==0xd664 ||
       descriptor==0xd648 || descriptor==0xd62c)return true;
    // The coastal and snow courses share turns with edge strips and flat centre dashes.
    if(linked_turns) {
        if(descriptor==0xef00)return face>=0xe && face<=0x13;
        if(descriptor==0xd920)return face==9 || face==0xa || face==0xb || face==0xd;
        if(descriptor==0xd904)return face==7 || face==8;
        if(descriptor==0xd8e8)return face==9 || face==0xa || face==0xc || face==0xe;
    }
    // The mirrored long bends share a white centre dash at face 0x13.
    if(descriptor==0xedb0 || descriptor==0xedcc)return face!=0x13;
    // The narrow Sky Ramp bends use both paint indices only on the inside curb.
    if(descriptor==0xbff8)return true;
    // Aqua's quarter-turn has three separate white centre dashes at 14..16.
    if(descriptor==0xd7b4)return face==0x11 || face==0x13 || face==0x17 || face==0x19;
    // Night Owl's flat and elevated tight bends use the same curb geometry
    // with different face numbering. The three isolated white dashes stay flat.
    if(descriptor==0xc5fc)return face==3 || face==6 || face==0xb || face==0xc;
    if(descriptor==0xd8cc)return face==0xe || face==0x13 || face==0x15 || face==0x16;
    if(descriptor==0xc0d8)return face>=7 && face<=0xc;
    if(descriptor==0xef54)return face==0xe || face==0x11 || face==0x12;
    // On the tighter quarter-turn, 8, 9 and A are dashes inside the road.
    return descriptor==0xf7dc && (face==4 || face==5 || face==0xc || face==0xd);
}

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

constexpr bool forest_bridge_surface(unsigned descriptor,unsigned material) {
    if(descriptor==0xfa0c)return material==0xc9; // moving plank tops
    return (descriptor==0xc1b8 || descriptor==0xc1d4) && (material==0x62 || material==0x63);
}

constexpr bool sea_breeze_road(unsigned opcode,unsigned material) {
    return graded_road(opcode,material) || (opcode==2 && material==0x29);
}

constexpr bool aqua_tunnel_rib(unsigned descriptor) {
    return descriptor==0xd46c || descriptor==0xd450 || descriptor==0xeffc;
}

// The outer approach quad runs from the right wall to the left at both portals.
void aqua_approach(std::vector<GpuVertex>& mesh,std::array<GpuVertex,4> q) {
    auto inset=[](GpuVertex a,const GpuVertex& b) {
        // A metre of gravel along each edge of the native 3840-unit pavement.
        constexpr float fraction=90.f/3840;
        a.x=std::lerp(a.x,b.x,fraction); a.y=std::lerp(a.y,b.y,fraction); a.z=std::lerp(a.z,b.z,fraction);
        a.u=std::lerp(a.u,b.u,fraction); a.v=std::lerp(a.v,b.v,fraction);
        a.r=std::lerp(a.r,b.r,fraction); // signed lane offset for detailed asphalt
        return a;
    };
    auto quad=[&](GpuVertex a,GpuVertex b,GpuVertex c,GpuVertex d,bool gravel) {
        if(gravel)a.texture=b.texture=c.texture=d.texture=-45;
        mesh.insert(mesh.end(),{a,b,c,a,c,d});
    };
    const auto a=inset(q[0],q[1]),b=inset(q[1],q[0]),c=inset(q[2],q[3]),d=inset(q[3],q[2]);
    quad(q[0],a,d,q[3],true);
    quad(a,b,c,d,false);
    quad(b,q[1],q[2],c,true);
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
void aqua_panel_seams(std::vector<GpuVertex>& mesh,unsigned descriptor,bool detailed,const Point& point) {
    using Bay=std::array<unsigned,4>; // near left/right, far left/right
    constexpr Bay straight[]={{2,3,5,4},{11,10,30,31},{36,37,42,43}};
    constexpr Bay curved[]={{7,6,22,13},{22,13,41,15},{44,18,52,24},
                             {52,24,55,25},{57,27,61,29},{61,29,62,30}};
    const auto bays=descriptor==0xd450?std::span<const Bay>(curved):std::span<const Bay>(straight);
    for(const auto& bay:bays) {
        const auto a=point(bay[0]),b=point(bay[1]),c=point(bay[2]),d=point(bay[3]);
        Vec tint{.34f,.59f,.62f};
        auto vertex=[&](float across,float along) {
            Vec p;
            for(unsigned i=0;i<3;++i)p[i]=std::lerp(std::lerp(a[i],b[i],across),std::lerp(c[i],d[i],across),along);
            return GpuVertex{p[0],p[1],p[2],tint[0],tint[1],tint[2],0,0,-5};
        };
        auto strip=[&](float l,float r,float start,float end) {
            const auto v0=vertex(l,start),v1=vertex(r,start),v2=vertex(r,end),v3=vertex(l,end);
            mesh.insert(mesh.end(),{v0,v1,v2,v0,v2,v3,v2,v1,v0,v3,v2,v0});
        };
        strip(0,.008f,0,1);
        strip(.992f,1,0,1);
        for(float u:{.25f,.75f})strip(u-.0015f,u+.0015f,0,1);
        strip(0,1,.496f,.504f);
        if(detailed) {
            // Rubber seals border the central glazing strips. Steel clamps
            // grip the glass at intervals without filling the transparent bays.
            for(float u:{.25f,.75f}) {
                tint={.11f,.16f,.17f};
                for(float start:{0.f,.504f}) {
                    const float end=start==0?.496f:1.f;
                    strip(u-.004f,u-.0015f,start,end);
                    strip(u+.0015f,u+.004f,start,end);
                }
                tint={.61f,.69f,.68f};
                for(float v:{.18f,.50f,.82f}) {
                    strip(u-.010f,u-.004f,v-.014f,v-.005f);
                    strip(u+.004f,u+.010f,v+.005f,v+.014f);
                }
            }
        }
    }
}

// A road face supplies a local grade and a footprint. In original world units
// Y increases downwards. The lower envelope keeps the underpass open.
struct RoadGrade {
    float x0,x1,z0,z1,y0,y1,dx,dz,intercept;
};

template<class Normalize>
void level_terrain(std::vector<GpuVertex>& mesh,float ground_y,float material,const Normalize& normalize,Vec tint={1,.60f,0},std::span<const forest_roadside::Edge> verges={}) {
    constexpr int edge=49152,step=2048;
    auto vertex=[&](int x,int z) {
        const auto p=normalize(Vec{float(x),ground_y,float(z)});
        return GpuVertex{p[0],p[1],p[2],tint[0],tint[1],tint[2],x/90.f,z/90.f,material};
    };
    // N64 distance fog is evaluated at vertices, including on flat ground.
    for(int z=-edge;z<edge;z+=step)for(int x=-edge;x<edge;x+=step) {
        if(!verges.empty() && forest_roadside::verge_tile(mesh,x,z,step,verges,vertex))continue;
        const auto a=vertex(x,z),b=vertex(x+step,z),c=vertex(x+step,z+step),d=vertex(x,z+step);
        mesh.insert(mesh.end(),{a,b,c,a,c,d});
    }
}

template<class Normalize>
void coastal_terrain(std::vector<GpuVertex>& mesh,const srf::World& world,
                    const std::map<unsigned,const srf::WorldModel*>& models,const Normalize& normalize,
                    std::span<const forest_roadside::Edge> verges,unsigned foliage_layer) {
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
        auto ground=vertex(x,y,z,.94f+.11f*rolls,smooth((y-sea_level+320)/230),-9);
        if(!verges.empty()) {
            ground.texture=-40;ground.b=forest_roadside::verge_distance(verges,{x,y,z});
        }
        grid.push_back(ground);
    }
    for(unsigned r=1;r<rows;++r)for(unsigned c=1;c<cols;++c) {
        const unsigned i=r*cols+c;
        for(auto j:{i-cols-1,i-cols,i,i-cols-1,i,i-1})mesh.push_back(grid[j]);
    }
    if(foliage_layer)for(const auto& instance:world.instances) {
        if(instance.visual_descriptor!=0xa104)continue;
        // Place the native tree on the same coastal triangle that will be drawn.
        const float col=(instance.position.x-xmin+margin)/step,row=(instance.position.z-zmin+margin)/step;
        if(col<0 || row<0 || col>=cols-1 || row>=rows-1)continue;
        const unsigned i=unsigned(row)*cols+unsigned(col);
        const float tx=col-unsigned(col),tz=row-unsigned(row);
        const float nw=grid[i].y,ne=grid[i+1].y,sw=grid[i+cols].y,se=grid[i+cols+1].y;
        const float surface=nw+(tx>=tz ? tx*(ne-nw)+tz*(se-ne) : tz*(sw-nw)+tx*(se-sw));
        const float dy=surface-normalize(Vec{float(instance.position.x),float(instance.position.y),float(instance.position.z)})[1];
        forest_roadside::tree(mesh,instance,float(foliage_layer),[&](Vec p) {
            auto q=normalize(p);q[1]+=dy;return q;
        });
    }
    // A level water surface intersects the beach and has parallax as the car turns.
    constexpr float horizon=480000;
    for(auto xz:{Vec{-1,0,-1},Vec{1,0,-1},Vec{1,0,1},Vec{-1,0,-1},Vec{1,0,1},Vec{-1,0,1}})
        mesh.push_back(vertex(xz[0]*horizon,sea_level,xz[2]*horizon,1,0,-10));
}
}
