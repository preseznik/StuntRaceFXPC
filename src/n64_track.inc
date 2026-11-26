namespace n64 {
// The bridge scenery and the road beneath it are separate Easy Ride placements.
constexpr unsigned truss_descriptor=0xb690, underpass_descriptor=0xe330;

// Sweep an I-section between the short ends of an existing truss panel.
// Its attachment points come from the course; only the rendered section changes.
template<class Normalize>
void steel_beam(std::vector<GpuVertex>& triangles,std::array<Vec,4> panel,Vec depth,const Normalize& normalize) {
    auto length=[](const Vec& a,const Vec& b){return std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);};
    if(length(panel[0],panel[1])>length(panel[1],panel[2]))std::rotate(panel.begin(),panel.begin()+1,panel.end());
    struct Section {float width,depth;};
    constexpr Section profile[]={{.15f,-64},{.24f,-64},{.24f,-12},{.76f,-12},{.76f,-64},{.85f,-64},
                                 {.85f,64},{.76f,64},{.76f,12},{.24f,12},{.24f,64},{.15f,64}};
    constexpr unsigned count=std::size(profile);
    std::array<std::array<GpuVertex,count>,2> rings;
    const float meters=length(panel[0],panel[3])/90;
    for(unsigned end=0;end<2;++end) {
        const auto& a=panel[end?3:0];const auto& b=panel[end?2:1];
        for(unsigned i=0;i<count;++i) {
            Vec world;
            for(unsigned axis=0;axis<3;++axis)
                world[axis]=a[axis]+(b[axis]-a[axis])*profile[i].width+depth[axis]*profile[i].depth;
            auto p=normalize(world);
            rings[end][i]={p[0],p[1],p[2],1,1,1,end*meters,profile[i].width*3+profile[i].depth/90,-5};
        }
    }
    auto quad=[&](GpuVertex a,GpuVertex b,GpuVertex c,GpuVertex d,float shade) {
        for(auto v:{a,b,c,a,c,d}) {
            v.r=.62f*shade;v.g=.25f*shade;v.b=.16f*shade;
            triangles.push_back(v);
        }
    };
    constexpr float lighting[]={.97f,.71f,.79f,.66f,.97f,1.16f,.92f,.72f,.82f,.65f,.93f,.83f};
    for(unsigned i=0;i<count;++i) {
        const auto j=(i+1)%count;
        quad(rings[0][i],rings[0][j],rings[1][j],rings[1][i],lighting[i]);
    }
    for(const auto& r:rings) {
        quad(r[0],r[1],r[10],r[11],.82f);quad(r[2],r[3],r[8],r[9],.74f);quad(r[4],r[5],r[6],r[7],.82f);
    }
}

// Dress the upper edge of vertical barrier panels. Work in world units
// so a banked or curved section carries the rail and its reflector with it.
enum class BarrierMarker { painted, reflector, guide_lamp };
template<class Normalize>
void barrier_trim(std::vector<GpuVertex>& triangles,std::array<Vec,4> panel,const Normalize& normalize,BarrierMarker marker) {
    std::sort(panel.begin(),panel.end(),[](const Vec& a,const Vec& b) {
        return std::tie(a[0],a[2],a[1])<std::tie(b[0],b[2],b[1]);
    });
    const auto& start=panel[0];const auto& end=panel[2];
    auto upright=[](const Vec& a,const Vec& b){return a[0]==b[0] && a[2]==b[2] && b[1]>a[1];};
    if(!upright(start,panel[1]) || !upright(end,panel[3]))return;
    float dx=end[0]-start[0],dz=end[2]-start[2];
    float length=std::hypot(dx,dz);
    if(length<100)return;
    dx/=length; dz/=length;
    auto point=[&](float along,float across,float height) {
        return normalize(Vec{start[0]+along*dx+across*dz,start[1]+along/length*(end[1]-start[1])-height,
                             start[2]+along*dz-across*dx});
    };
    auto bar=[&](float from,float to,float width,float bottom,float top,Vec color,bool lens=false) {
        const std::array corners{point(from,-width,bottom),point(to,-width,bottom),point(to,width,bottom),point(from,width,bottom),
                                 point(from,-width,top),point(to,-width,top),point(to,width,top),point(from,width,top)};
        auto quad=[&](int a,int b,int c,int d,float shade,float texture=0) {
            // Authored barriers can face either way. Give the five exposed faces both windings.
            for(auto i:{a,b,c,a,c,d,c,b,a,d,c,a}) {
                const auto& p=corners[i];
                triangles.push_back({p[0],p[1],p[2],color[0]*shade,color[1]*shade,color[2]*shade,0,0,texture});
            }
        };
        quad(4,5,6,7,1);quad(0,1,5,4,.76f);quad(2,3,7,6,.76f);
        quad(0,4,7,3,.63f,lens?-46.f:0.f);quad(1,2,6,5,.63f,lens?-46.f:0.f);
    };
    bar(0,length,11,-3,10,{.75f,.76f,.71f});
    const auto middle=length*.5f;
    if(marker==BarrierMarker::guide_lamp && length>800) {
        // Low, two-ended lamps follow the grade on both approaches to a crest.
        bar(middle-38,middle+38,24,10,26,{.32f,.36f,.40f});
        bar(middle-32,middle+32,18,26,53,{.14f,.72f,1.f},true);
    } else if(length>1200) {
        bar(middle-12,middle+12,10,10,61,{.19f,.23f,.23f});
        if(marker==BarrierMarker::reflector)bar(middle-7,middle+7,19,61,91,{1.f,.67f,.17f},true);
        else bar(middle-12,middle+12,10,61,77,{1.f,.69f,.19f});
    }
}

// AF74 is a two-sided panel in the local Y/Z plane. Fit a steel housing
// around it, leaving both original faces visible as the bonus gate pitches.
template<class Place>
void cruise_gate_housing(std::vector<GpuVertex>& mesh,const Place& place) {
    auto corner=[&](int x,int y,int z,float material) {
        const auto p=place(srf::Vertex{x,y,z});
        const Vec color=material==-46 ? Vec{1,.67f,.17f} : Vec{.45f,.49f,.52f};
        return GpuVertex{p[0],p[1],p[2],color[0],color[1],color[2],float(z)/90,-float(y)/90,material};
    };
    auto ring=[&](int x,int margin) {
        return std::array{corner(x,-768-margin,-640-margin,-48),corner(x,-768-margin,640+margin,-48),
                          corner(x,-128+margin,640+margin,-48),corner(x,-128+margin,-640-margin,-48)};
    };
    auto quad=[&](GpuVertex a,GpuVertex b,GpuVertex c,GpuVertex d) {
        mesh.insert(mesh.end(),{a,b,c,a,c,d});
    };
    const auto front=ring(-20,24),back=ring(20,24),opening=ring(0,0);
    for(unsigned i=0;i<4;++i) {
        const auto j=(i+1)%4;
        quad(front[i],front[j],opening[j],opening[i]);
        quad(back[j],back[i],opening[i],opening[j]);
        quad(front[j],front[i],back[i],back[j]);
    }
    // Short amber strips on the upper lip catch the approaching headlights.
    for(int z:{-570,450}) {
        quad(corner(-19,-790,z,-46),corner(-19,-790,z+120,-46),
             corner(-11,-780,z+120,-46),corner(-11,-780,z,-46));
    }
}

// Night Owl's four straight viaduct spans share this local frame. The pier
// sits near the joint, clear of the three lower road crossings.
template<class Place>
void owl_pier(std::vector<GpuVertex>& triangles,const Place& place) {
    constexpr int center=3400;
    auto section=[&](int top,int bottom,int upper_width,int lower_width,int depth) {
        const std::array<srf::Vertex,8> corners{{
            {-upper_width,top,center-depth},{upper_width,top,center-depth},
            {upper_width,top,center+depth},{-upper_width,top,center+depth},
            {-lower_width,bottom,center-depth},{lower_width,bottom,center-depth},
            {lower_width,bottom,center+depth},{-lower_width,bottom,center+depth}}};
        for(unsigned side=0;side<4;++side) {
            const unsigned next=(side+1)%4;
            for(auto index:{side,next,next+4,side,next+4,side+4}) {
                const auto& v=corners[index];const auto p=place(v);
                triangles.push_back({p[0],p[1],p[2],.47f,.46f,.42f,
                    float(side%2?v.z:v.x)/90,-float(v.y)/90,-43});
            }
        }
    };
    section(638,818,750,240,135);
    section(818,1600,240,240,135);
}
// Extend each side of the flooded roadbed outwards. Following the native
// vertices keeps the shore joined across the shallow dips between panels.
template<class Place>
void sea_gravel_edge(std::vector<GpuVertex>& mesh,srf::Vertex a,srf::Vertex b,const Place& place) {
    auto corner=[&](srf::Vertex v,int width,int rise,float wetness) {
        // Taper into the ramps at either end of the crossing.
        float taper=std::clamp((3840-std::abs(v.z))/960.f,.14f,1.f);
        v.x+=(v.x<0?-1:1)*int(width*taper);
        v.y-=int(rise*taper);
        const auto p=place(v);
        return GpuVertex{p[0],p[1],p[2],wetness,0,0,float(v.x)/90,float(v.z)/90,-47};
    };
    const auto a0=corner(a,0,0,1),a1=corner(a,175,30,.30f),a2=corner(a,310,-48,.85f);
    const auto b0=corner(b,0,0,1),b1=corner(b,175,30,.30f),b2=corner(b,310,-48,.85f);
    mesh.insert(mesh.end(),{a0,a1,b1,a0,b1,b0,a1,a2,b2,a1,b2,b1});
}

// Seat a chipped slab on a native triangular scree shelf. Limit its footprint
// to the centroid's clearance from the edges, including on narrow switchbacks.
template<class Place>
void ravine_shard(std::vector<GpuVertex>& mesh,const srf::Vertex& a,const srf::Vertex& b,
                  const srf::Vertex& c,Vec color,const Place& place) {
    const float bx=float(b.x-a.x),bz=float(b.z-a.z),cx=float(c.x-a.x),cz=float(c.z-a.z);
    const float area=bx*cz-cx*bz;
    const float longest=std::max({std::hypot(bx,bz),std::hypot(cx,cz),std::hypot(bx-cx,bz-cz)});
    if(std::abs(area)<60*longest || longest==0)return;
    const float radius=std::min(84.f,std::abs(area)/(3*longest));
    auto vertex=[&](float dx,float dz,float height,float shade) {
        const float x=(a.x+b.x+c.x)/3.f+dx*radius,z=(a.z+b.z+c.z)/3.f+dz*radius;
        const float u=((x-a.x)*cz-(z-a.z)*cx)/area,v=(bx*(z-a.z)-bz*(x-a.x))/area;
        const float y=a.y+u*(b.y-a.y)+v*(c.y-a.y)-height*radius;
        const auto p=place(srf::Vertex{int(std::round(x)),int(std::round(y)),int(std::round(z))});
        return GpuVertex{p[0],p[1],p[2],color[0]*shade,color[1]*shade,color[2]*shade,x/90,z/90,-42};
    };
    const auto foot=std::array{vertex(-.92f,-.20f,-.07f,.73f),vertex(-.32f,.66f,-.07f,.79f),
                               vertex(.86f,.28f,-.07f,.70f),vertex(.55f,-.67f,-.07f,.82f)};
    const auto ridge=vertex(-.37f,.04f,.50f,1.12f),tip=vertex(.43f,-.20f,.28f,1.05f);
    mesh.insert(mesh.end(),{foot[0],foot[1],ridge,foot[1],foot[2],tip,foot[1],tip,ridge,
                            foot[2],foot[3],tip,foot[3],foot[0],ridge,foot[3],ridge,tip});
}

// C404 has two grated leaves. Follow each posed hinge-to-tip edge so the
// steel straps keep their length and sit above the grate throughout the lift.
template<class Place>
void lake_hinge_plates(std::vector<GpuVertex>& mesh,const std::vector<srf::Vertex>& pose,const Place& place) {
    for(const auto edge:{std::pair{2,10},std::pair{6,12}}) {
        const auto& hinge=pose.at(edge.first);
        const auto& tip=pose.at(edge.second);
        const float length=std::hypot(float(tip.y-hinge.y),float(tip.z-hinge.z));
        const float dy=(tip.y-hinge.y)/length,dz=(tip.z-hinge.z)/length;
        const float ny=-std::abs(dz),nz=std::copysign(1.f,dz)*dy;
        for(int side:{-1,1}) {
            auto vertex=[&](int inset,int distance,int height,float shade) {
                const auto p=place(srf::Vertex{side*(hinge.x-inset),
                    hinge.y+int(std::round(distance*dy+height*ny)),
                    hinge.z+int(std::round(distance*dz+height*nz))});
                return GpuVertex{p[0],p[1],p[2],.44f*shade,.49f*shade,.50f*shade,
                                 inset/90.f,distance/90.f,-48};
            };
            // A raised knuckle across a tapered strap; keep the center grating open.
            auto quad=[&](GpuVertex a,GpuVertex b,GpuVertex c,GpuVertex d) {
                if(side*dz>0)std::swap(b,d); // mirrored side or opposing leaf
                mesh.insert(mesh.end(),{a,b,c,a,c,d});
            };
            std::array<GpuVertex,4> prev;
            for(const auto [distance,width,height]:{std::array{0,154,26},std::array{28,154,41},
                    std::array{58,154,22},std::array{160,154,16},std::array{390,100,5}}) {
                const std::array row{vertex(14,distance,0,.6f),vertex(14,distance,height,.9f),
                                     vertex(width,distance,height,1),vertex(width,distance,0,.6f)};
                if(distance==0)quad(row[0],row[3],row[2],row[1]);
                else for(unsigned j=0;j<3;++j)quad(prev[j],prev[j+1],row[j+1],row[j]);
                if(distance==390)quad(row[0],row[1],row[2],row[3]);
                prev=row;
            }
        }
    }
}

// Gravel collects inside the two fixed approach kerbs. Taper each end
// into the road before the joint with the moving deck.
template<class Place>
void lake_approach_gravel(std::vector<GpuVertex>& mesh,const Place& place) {
    for(int side:{-1,1}) {
        std::array<GpuVertex,2> prev;
        for(int z:{-950,-700,-180,320,720,950}) {
            const int width=std::abs(z)==950?8:125+int(24*std::sin(z*.009f));
            auto vertex=[&](int inset,int height,float wetness) {
                const int x=side*(960-inset);
                const auto p=place(srf::Vertex{x,-height,z});
                return GpuVertex{p[0],p[1],p[2],wetness,0,0,x/90.f,z/90.f,-47};
            };
            const std::array row{vertex(0,10,.85f),vertex(width,3,.45f)};
            if(z!=-950)mesh.insert(mesh.end(),{prev[0],prev[1],row[1],prev[0],row[1],row[0]});
            prev=row;
        }
    }
}

// The four leaves spread out from a square hub as the sign collapses.
// All four top corners stay level and descend together in its native poses.
template<class Place>
void harbor_hinges(std::vector<GpuVertex>& mesh,int hub_y,const Place& place) {
    constexpr std::array<std::array<int,2>,6> ring{{{0,6},{5,3},{5,-3},{0,-6},{-5,-3},{-5,3}}};
    for(int axis=0;axis<2;++axis)for(int side:{-1,1})for(int center:{-44,44}) {
        auto vertex=[&](int along,std::array<int,2> section) {
            const int edge=side*(80+section[0]);
            const auto p=place(srf::Vertex{axis?edge:along,hub_y+section[1]-2,axis?along:edge});
            return GpuVertex{p[0],p[1],p[2],.63f,.68f,.70f,along/90.f,section[1]/90.f,-5};
        };
        for(unsigned i=0;i<ring.size();++i) {
            const auto a=ring[i],b=ring[(i+1)%ring.size()];
            const auto l=vertex(center-18,a),r=vertex(center+18,a);
            const auto ln=vertex(center-18,b),rn=vertex(center+18,b);
            mesh.insert(mesh.end(),{l,r,rn,l,rn,ln});
        }
    }
}

// The A-frame folds about its local X axis. Both pin barrels use the
// same model transform as the native legs while the sign turns on impact.
template<class Place>
void sea_barrier_hinges(std::vector<GpuVertex>& mesh,const Place& place) {
    constexpr std::array<std::array<int,2>,6> ring{{{0,15},{13,8},{13,-8},{0,-15},{-13,-8},{-13,8}}};
    auto vertex=[&](int x,int y,int z) {
        const auto p=place(srf::Vertex{x,y-184,z});
        return GpuVertex{p[0],p[1],p[2],.46f,.50f,.51f,(x+z)/90.f,y/90.f,-48};
    };
    for(int side:{-1,1})for(unsigned i=0;i<ring.size();++i) {
        const auto a=ring[i],b=ring[(i+1)%ring.size()];
        const int left=side*167-18,right=side*167+18;
        const auto l=vertex(left,a[0],a[1]),r=vertex(right,a[0],a[1]);
        const auto ln=vertex(left,b[0],b[1]),rn=vertex(right,b[0],b[1]);
        mesh.insert(mesh.end(),{l,r,rn,l,rn,ln,vertex(left,0,0),l,ln,vertex(right,0,0),rn,r});
    }
}
// Sweep a low drift outside an authored pavement edge. The long windward
// slope, short lee lip and buried ends share vertices between sections.
template<class Normalize>
void snowbank(std::vector<GpuVertex>& mesh,const std::vector<srf::Vertex>& pose,
              std::initializer_list<unsigned> edge,int side,const srf::WorldInstance& instance,const Normalize& normalize) {
    constexpr unsigned steps=4;
    const unsigned last=unsigned(edge.size()-1)*steps;
    std::array<GpuVertex,5> previous;
    for(unsigned i=0;i<=last;++i) {
        const unsigned segment=std::min(i/steps,unsigned(edge.size()-2));
        const float t=float(i-segment*steps)/steps;
        const auto& a=pose.at(edge.begin()[segment]);
        const auto& b=pose.at(edge.begin()[segment+1]);
        // Interpolate the two vertex normals to round off polygon joints.
        auto outward=[&](unsigned k) {
            const auto& p=pose.at(edge.begin()[k?k-1:k]);
            const auto& q=pose.at(edge.begin()[std::min(k+1,unsigned(edge.size()-1))]);
            const float length=std::hypot(float(q.z-p.z),float(q.x-p.x));
            return std::array{side*(q.z-p.z)/length,side*(p.x-q.x)/length};
        };
        const auto n=outward(segment),m=outward(segment+1);
        const float dx=std::lerp(n[0],m[0],t),dz=std::lerp(n[1],m[1],t);
        const float x=std::lerp(float(a.x),float(b.x),t),z=std::lerp(float(a.z),float(b.z),t);
        const float taper=std::min({1.f,i/4.f,(last-i)/4.f});
        const float drift=.5f+.5f*std::sin(x*.0017f+z*.0011f);
        const float width=285+210*drift,height=taper*(68+45*drift);
        constexpr float across[]{0,.32f,.62f,.77f,1},rise[]{0,.53f,1,.88f,0};
        std::array<GpuVertex,5> row;
        for(unsigned k=0;k<row.size();++k) {
            const float offset=96+width*across[k];
            const srf::Vertex local{int(x+dx*offset),int(std::lerp(float(a.y),float(b.y),t)+14-height*rise[k]),int(z+dz*offset)};
            const auto w=srf::world_position(local,instance);
            const auto p=normalize(Vec{float(w[0]),float(w[1]),float(w[2])});
            // The hard lip catches the light above the blue, compacted lee face.
            constexpr float shade[]{.95f,.84f,1.f,.69f,.94f};
            row[k]={p[0],p[1],p[2],.79f*shade[k],.87f*shade[k],.94f*shade[k],float(w[0])/90,float(w[2])/90,-49};
        }
        if(i)for(unsigned k=1;k<row.size();++k)
            mesh.insert(mesh.end(),{previous[k-1],previous[k],row[k],previous[k-1],row[k],row[k-1]});
        previous=row;
    }
}
} // namespace n64
