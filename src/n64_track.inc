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

// Dress the upper edge of Easy Ride's vertical barrier panels. Work in world units
// so a banked or curved section carries the rail and its reflector with it.
template<class Normalize>
void barrier_trim(std::vector<GpuVertex>& triangles,std::array<Vec,4> panel,const Normalize& normalize) {
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
    auto bar=[&](float from,float to,float width,float bottom,float top,Vec color) {
        const std::array corners{point(from,-width,bottom),point(to,-width,bottom),point(to,width,bottom),point(from,width,bottom),
                                 point(from,-width,top),point(to,-width,top),point(to,width,top),point(from,width,top)};
        auto quad=[&](int a,int b,int c,int d,float shade) {
            // Authored barriers can face either way. Give the five exposed faces both windings.
            for(auto i:{a,b,c,a,c,d,c,b,a,d,c,a}) {
                const auto& p=corners[i];
                triangles.push_back({p[0],p[1],p[2],color[0]*shade,color[1]*shade,color[2]*shade});
            }
        };
        quad(4,5,6,7,1);quad(0,1,5,4,.76f);quad(2,3,7,6,.76f);quad(0,4,7,3,.63f);quad(1,2,6,5,.63f);
    };
    bar(0,length,11,-3,10,{.75f,.76f,.71f});
    if(length>1200) {
        const auto middle=length*.5f;
        bar(middle-12,middle+12,10,10,61,{.19f,.23f,.23f});
        bar(middle-12,middle+12,10,61,77,{1.f,.69f,.19f});
    }
}
} // namespace n64
