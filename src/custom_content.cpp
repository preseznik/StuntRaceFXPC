#include "srf/custom_content.hpp"
#include <algorithm>
#include <cmath>

namespace srf {
namespace {
// The grid heads north along the west rim. The hooked eastern spur turns back
// into the central saddle before the southern dry wash.
using Point = std::array<int,2>;
constexpr std::array<Point,16> rim{{{-16000,0},{-15850,9300},{-11000,18000},
    {-1350,20500},{9200,17500},{17400,11600},{18600,4100},{12600,1000},
    {6500,3900},{5000,-4300},{9500,-13400},{1700,-18400},{-6100,-18300},{-9700,-12800},
    {-13800,-9600},{-16050,-5500}}};
constexpr int samples=4, surface=0, width=1780;
static_assert(rim.size()*samples==ember_mesa.route_points);

// Integer Catmull-Rom sampling makes the route identical on all peers.
int cubic(int a,int b,int c,int d,int t) {
    constexpr int n=samples;
    return (2*b*n*n*n+(c-a)*t*n*n+(2*a-5*b+4*c-d)*t*t*n+
            (3*b-a-3*c+d)*t*t*t)/(2*n*n*n);
}
// Height and crossfall ease in at the north rim and return to the old
// grade before the hooked infield. Positive offset is the driver's right.
double ease(int section,int start,int end) {
    const double t=std::clamp(double(section-start)/(end-start),0.,1.);
    return t*t*(3.-2.*t);
}
// Positive world Y points down. The cut opens out through the right apex
// so the covered west grandstand stays in view on the way back to the grid.
int wash_depth(int i) {
    return int(std::lround(680*(ease(i,43,50)-ease(i,53,61))));
}
int rim_height(int i) {
    return int(std::lround(1580*(ease(i,11,20)-ease(i,21,31))));
}
double crossfall(int i) {
    return .158*(ease(i,17,22)-ease(i,24,29)); // nine degrees into the right turn
}
constexpr int shoulder=width+500, toe=shoulder+2800;
constexpr std::array offsets{-toe,-shoulder,-width-66,-width+66,-14,14,width-66,width+66,shoulder,toe};
constexpr std::array<std::uint8_t,9> band_colors{7,6,5,0x2a,3,0x2a,5,6,7};
using Section=std::array<Vertex,offsets.size()>;
Vertex side(const Scene& scene,unsigned i,int offset,int height=0) {
    const auto count=scene.points.size();
    const auto& p=scene.points[i].position;
    const auto& a=scene.points[(i+count-1)%count].position;
    const auto& b=scene.points[(i+1)%count].position;
    // The northern sweeper opens its outside lane and has a broad gravel
    // apron inside the late apex. Both are part of the tyre-contact mesh.
    if(i>=2 && i<=12) {
        const double bend=ease(i,2,6)-ease(i,9,12);
        if(offset<0)offset+=int(std::lround(320*bend*std::max(-1.,double(offset)/width)));
        else if(offset>width+66)offset+=int(std::lround(1100*bend*
            std::min(1.,double(offset-width-66)/(shoulder-width-66))));
    }
    // Taper the inside of the two tight hairpins before the section normals
    // intersect. Keep the centre line and the outside racing line in place.
    if(offset<0 && (i==30 || i==33))offset=offset*3/4;
    if((offset<0 && (i==31 || i==32)) || (offset>0 && (i==39 || i==40)))
        offset/=2;
    // The eastern earthwork extends past the pavement. Shorten only its toe
    // on the inside of the sweeper, where a full-width fill would fold over.
    if(i>=23 && i<=29 && offset>shoulder)offset=shoulder+(offset-shoulder)/2;
    if(i>=28 && i<=30 && offset < -shoulder)offset=-shoulder+(offset+shoulder)/10;
    const auto length=std::hypot(b.x-a.x,b.z-a.z);
    return {p.x+int(std::lround(offset*(b.z-a.z)/length)),p.y+int(std::lround(offset*crossfall(i)))-height,
            p.z-int(std::lround(offset*(b.x-a.x)/length))};
}
Section cross_section(const Scene& scene,unsigned i) {
    Section row;
    for(unsigned k=0;k<row.size();++k)row[k]=side(scene,i,offsets[k]);
    const double apron=ease(i,3,5)-ease(i,8,10);
    if(apron>0)row[1]=side(scene,i,-shoulder-int(std::lround(1600*apron)));
    const int depth=wash_depth(i);
    if(depth) {
        // Shoulder gravel leads up to a shelf of exposed red sandstone.
        // Use the same cross-section for the visible cut and tyre contact.
        for(int sign:{-1,1}) {
            const bool left=sign<0;
            const auto shelf=left?1u:unsigned(row.size()-2);
            const auto lip=left?0u:unsigned(row.size()-1);
            row[shelf].y-=depth/6;
            row[lip]=side(scene,i,sign*(shoulder+depth*(left?1:2)));
        }
    } else if(!rim_height(i) && !crossfall(i)) {
        row.front()=row[1];
        row.back()=row[row.size()-2];
    }
    row.front().y=row.back().y=surface;
    return row;
}
bool raised(const Section& row) {
    return row[1].y!=surface || row[row.size()-2].y!=surface;
}
WorldModel model(std::uint16_t id) {
    WorldModel m{};m.descriptor_address=id;m.frames.emplace_back();return m;
}
void quad(WorldModel& mesh,Vertex a,Vertex b,Vertex c,Vertex d,std::uint8_t color) {
    auto& vertices=mesh.frames.front();
    const auto n=std::uint32_t(vertices.size());
    vertices.insert(vertices.end(),{a,b,c,d});
    const double ux=b.x-a.x,uy=b.y-a.y,uz=b.z-a.z,vx=c.x-a.x,vy=c.y-a.y,vz=c.z-a.z;
    const double x=uy*vz-uz*vy,y=uz*vx-ux*vz,z=ux*vy-uy*vx;
    const double length=std::max(1.,std::hypot(x,y,z));
    const auto normal=[&](double v){return std::uint8_t(int(std::lround(127*v/length)));};
    mesh.primitives.push_back({{n,n+1,n+2,n+3},{0,0,color,normal(x),normal(y),normal(z)}});
}
using Ring=std::array<Vertex,4>;
Ring ring(int x,int y,int z,int w,int l) {
    return {{{x-w,y,z-l},{x+w,y,z-l},{x+w,y,z+l},{x-w,y,z+l}}};
}
void band(WorldModel& mesh,const Ring& base,const Ring& top,std::uint8_t color) {
    for(unsigned i=0;i<4;++i){auto j=(i+1)%4;quad(mesh,base[i],base[j],top[j],top[i],color);}
}
void cap(WorldModel& mesh,const Ring& r,std::uint8_t color){quad(mesh,r[0],r[1],r[2],r[3],color);}
// Fill the plain in horizontal slabs between the inside and outside edges of
// the circuit. Sand and pavement meet at their boundary even at grazing angles.
void desert_floor(WorldModel& land,const Scene& scene) {
    constexpr int extent=36000;
    struct Edge { Vertex a,b; double x(int z) const {
        return a.x+double(z-a.z)*(b.x-a.x)/(b.z-a.z);
    }};
    std::vector<Edge> edges;
    std::vector<int> levels{-extent,extent};
    for(unsigned i=0;i<scene.points.size();++i) {
        const auto a=cross_section(scene,i),b=cross_section(scene,(i+1)%scene.points.size());
        for(unsigned k:{0u,unsigned(a.size()-1)}) {
            levels.push_back(a[k].z);
            if(a[k].z!=b[k].z)edges.push_back({a[k],b[k]});
        }
    }
    std::ranges::sort(levels);
    levels.erase(std::unique(levels.begin(),levels.end()),levels.end());
    for(unsigned i=1;i<levels.size();++i) {
        const int lo=levels[i-1],hi=levels[i];
        std::vector<std::array<int,2>> cuts{{-extent,-extent},{extent,extent}};
        for(const auto& e:edges)if(std::min(e.a.z,e.b.z)<=lo && std::max(e.a.z,e.b.z)>=hi)
            cuts.push_back({int(std::lround(e.x(lo))),int(std::lround(e.x(hi)))});
        std::ranges::sort(cuts,{},[](auto edge){return edge[0]+edge[1];});
        for(unsigned j=1;j<cuts.size();j+=2) {
            auto left=cuts[j-1],right=cuts[j];
            quad(land,{left[0],surface,lo},{right[0],surface,lo},
                      {right[1],surface,hi},{left[1],surface,hi},8);
        }
    }
}
Vertex between(Vertex a,Vertex b,int n,int d) {
    return {a.x+(b.x-a.x)*n/d,a.y+(b.y-a.y)*n/d,a.z+(b.z-a.z)*n/d};
}
void finish_asphalt(WorldModel& road,const Section& a,const Section& b) {
    // The checker occupies the first tenth of the grid segment.
    const auto l=between(a[3],b[3],1,10),r=between(a[6],b[6],1,10);
    for(int row=0;row<2;++row) {
        auto tile=[&](int col,int line) {
            return between(between(a[3],l,line,2),between(a[6],r,line,2),col,16);
        };
        for(int col=0;col<16;++col)
            quad(road,tile(col,row),tile(col+1,row),tile(col+1,row+1),tile(col,row+1),(col+row)%2?2:3);
    }
    quad(road,l,r,b[6],b[3],0x2a);
}
void grandstand(WorldModel& land,int direction,int middle,bool covered) {
    constexpr int length=1720,depth=230,rise=122;
    auto seat=[&](int across,int height,int along) -> Vertex {
        return {rim[0][0]+direction*(2560+across),-height,middle+along};
    };
    auto panel=[&](Vertex a,Vertex b,Vertex c,Vertex d,std::uint8_t color) {
        if(direction>0)quad(land,d,c,b,a,color);
        else quad(land,a,b,c,d,color);
    };
    // Four terraces with an open centre aisle. Each tread owns its riser.
    for(int row=0;row<4;++row) {
        const int front=row*depth,back=front+depth,h=(row+1)*rise;
        panel(seat(front,h-rise,-length),seat(front,h-rise,length),
                  seat(front,h,length),seat(front,h,-length),2);
        panel(seat(front,h,-length),seat(front,h,length),seat(back,h,length),seat(back,h,-length),3);
        for(int end:{-length,length})
            if(end>0)panel(seat(front,0,end),seat(back,0,end),seat(back,h,end),seat(front,h,end),4);
            else panel(seat(front,h,end),seat(back,h,end),seat(back,0,end),seat(front,0,end),4);
        for(int i=-8;i<=8;++i) {
            if(i==0 || (i+row+middle)%7==0)continue;
            const int along=i*192+(row%2)*32;
            const int variation=(i+8)*7+row*11+(covered?3:0);
            const auto torso=seat(front+100,h+102,along);
            const auto waist=ring(torso.x,-h,torso.z,32,44);
            const auto shoulders=ring(torso.x,torso.y,torso.z,40,55);
            constexpr std::array<std::uint8_t,6> shirts{4,5,9,3,10,11};
            const auto shirt=shirts[variation%shirts.size()];
            band(land,waist,shoulders,shirt);cap(land,shoulders,shirt);
            const auto chin=ring(torso.x,torso.y,torso.z,32,30);
            const auto crown=ring(torso.x,torso.y-58,torso.z,25,23);
            band(land,chin,crown,variation%3==0?12:8);cap(land,crown,2);
        }
    }
    panel(seat(4*depth,0,-length),seat(4*depth,0,length),
              seat(4*depth,4*rise,length),seat(4*depth,4*rise,-length),4);
    if(!covered)return;
    for(int z:{-length+35,0,length-35}) {
        auto base=seat(4*depth-35,4*rise,z);
        band(land,ring(base.x,base.y,base.z,26,26),ring(base.x,-1050,base.z,26,26),2);
    }
    const Ring soffit{seat(-90,1170,-length-100),seat(-90,1170,length+100),
                      seat(4*depth+70,1040,length+100),seat(4*depth+70,1040,-length-100)};
    auto roof=soffit;
    for(auto& v:roof)v.y-=70;
    quad(land,soffit[3],soffit[2],soffit[1],soffit[0],3);
    band(land,soffit,roof,5);cap(land,roof,4);
}

void start_area(WorldModel& land) {
    const int x=rim[0][0];
    for(int sign:{-1,1}) {
        const int post=x+sign*2130;
        auto foot=ring(post,0,0,145,190),pedestal=ring(post,-270,0,110,145);
        band(land,foot,pedestal,2);
        auto neck=ring(post,-1460,0,85,112);
        band(land,pedestal,neck,4);
        band(land,neck,ring(post,-1600,0,112,140),5);
    }
    // Inlay the lettering and checkerboard into both faces of the gantry.
    // Rows of equal colour share a quad, including the background.
    constexpr std::array<std::array<int,5>,10> letters{{
        {7,4,6,4,7},{5,7,7,5,5},{6,5,6,5,6},{7,4,6,4,7},{6,5,6,5,5},
        {},{5,7,7,5,5},{7,4,6,4,7},{7,4,7,1,7},{2,5,7,5,5}}};
    constexpr int cells=55,unit=82,edge=cells*unit/2;
    auto ink=[&](int col,int row) -> std::uint8_t {
        if(col<6 || col>=cells-6)return ((col+row)%2)?3:2;
        const int t=col-8;
        if(t<0 || t>=39 || t%4==3)return 2;
        const int bits=letters[t/4][row];
        return bits&(4>>(t%4))?3:2;
    };
    for(int face:{-1,1})for(int row=0;row<5;++row) {
        auto panel=[&](int c,int r) -> Vertex {return {x-face*(c*unit-edge),-2010+r*unit,face*140};};
        for(int col=0;col<cells;) {
            int end=col+1;auto color=ink(col,row);
            while(end<cells && ink(end,row)==color)++end;
            quad(land,panel(col,row+1),panel(end,row+1),panel(end,row),panel(col,row),color);
            col=end;
        }
    }
    const auto top=ring(x,-2010,0,edge,140),bottom=ring(x,-1600,0,edge,140);
    for(unsigned i:{1u,3u}) {
        const auto j=(i+1)%4;
        quad(land,bottom[i],bottom[j],top[j],top[i],5);
    }
    quad(land,bottom[3],bottom[2],bottom[1],bottom[0],2);
    const auto trim=ring(x,-2080,0,edge,140);band(land,top,trim,5);cap(land,trim,3);
    grandstand(land,-1,-1350,true);
    grandstand(land,1,2450,false);
}

// Place props in the road frame, with along measured in the driving direction.
Vertex roadside(const Scene& scene,unsigned i,int across,int along,int height) {
    auto p=side(scene,i,across,height);
    const auto& a=scene.points[i-1].position;
    const auto& b=scene.points[i+1].position;
    const double length=std::hypot(b.x-a.x,b.z-a.z);
    p.x+=int(std::lround(along*(b.x-a.x)/length));
    p.z+=int(std::lround(along*(b.z-a.z)/length));
    return p;
}
void eastern_landmarks(WorldModel& land,const Scene& scene) {
    // A continuous eroded slab. Both uneven rims share the same open aperture.
    constexpr std::array<Point,9> inner{{{-2340,-1900},{-2180,1080},{-1910,1970},
        {-1200,2360},{-260,2490},{840,2360},{1730,2030},{2180,1240},{2440,-1900}}};
    constexpr std::array<Point,9> outer{{{-4220,-1900},{-3530,1470},{-3110,2690},
        {-1480,3150},{-390,3060},{960,2980},{2590,2860},{3430,1680},{4350,-1900}}};
    constexpr std::array<int,9> depths{770,690,535,430,395,470,580,670,820};
    for(unsigned i=0;i+1<inner.size();++i) {
        auto rock=[&](unsigned k,int layer,int face) {
            const auto& a=inner[k];const auto& b=outer[k];
            const int offset=face>0?90:0;
            return roadside(scene,20,a[0]+(b[0]-a[0])*layer/10+offset,
                            face*(depths[k]+(layer==7?65:0)),
                            a[1]+(b[1]-a[1])*layer/10+offset);
        };
        for(int face:{-1,1})for(auto band:std::array<std::array<int,3>,3>{{{0,6,6},{6,7,8},{7,10,6}}})
            quad(land,rock(i,band[0],face),rock(i+1,band[0],face),
                      rock(i+1,band[1],face),rock(i,band[1],face),std::uint8_t(band[2]));
        quad(land,rock(i,0,-1),rock(i,0,1),rock(i+1,0,1),rock(i+1,0,-1),7);
        quad(land,rock(i,10,-1),rock(i+1,10,-1),rock(i+1,10,1),rock(i,10,1),8);
    }
    // The outside rail and the three right arrows mark the tightening descent.
    for(unsigned i=21;i<=26;++i) {
        constexpr int rail=-width-235;
        auto post=side(scene,i,rail);
        band(land,ring(post.x,post.y,post.z,25,25),ring(post.x,post.y-200,post.z,25,25),2);
        if(i<26)quad(land,side(scene,i,rail,130),side(scene,i+1,rail,130),
                          side(scene,i+1,rail,210),side(scene,i,rail,210),3);
        if(i%2==0) {
            // Tile the arrow into the board face so it reads from both sides
            // without relying on depth bias between overlapping polygons.
            auto sign=[&](int x,int h){return roadside(scene,i,rail+x,0,h);};
            constexpr std::array<int,3> heights{215,350,485};
            constexpr std::array<int,3> tips{-20,140,-20};
            for(unsigned row=0;row<2;++row) {
                const auto edge=[&](unsigned r){return std::array{-220,tips[r]-135,tips[r],220};};
                const auto low=edge(row),high=edge(row+1);
                for(unsigned t=0;t<3;++t)
                    quad(land,sign(low[t],heights[row]),sign(low[t+1],heights[row]),
                              sign(high[t+1],heights[row+1]),sign(high[t],heights[row+1]),t==1?3:2);
            }
        }
    }
}
// A desert pump wheel has many broad sails. A single blade pitch loops
// seamlessly because all ten sails are identical.
WorldModel pump_wheel() {
    auto wheel=model(mesa_wheel);
    constexpr int blades=10,poses=16;
    const double pitch=2*std::acos(-1.)/blades;
    for(int i=0;i<blades;++i) {
        auto point=[&](int radius,double offset,int z) -> Vertex {
            const double angle=(i+offset)*pitch;
            return {int(std::lround(radius*std::cos(angle))),int(std::lround(radius*std::sin(angle))),z};
        };
        auto sail=[&](Vertex a,Vertex b,Vertex c,Vertex d,std::uint8_t color) {
            quad(wheel,a,b,c,d,color);quad(wheel,d,c,b,a,color);
        };
        sail(point(55,0,0),point(55,.45,0),point(390,.16,0),point(390,0,0),2);
        sail(point(390,0,0),point(390,.42,50),point(1000,.67,95),point(980,0,-18),3);
        sail(point(980,0,-18),point(1000,.67,95),point(1110,.67,95),point(1090,0,-18),5);
    }
    for(int i=1;i<poses;++i) {
        const double angle=i*pitch/poses,c=std::cos(angle),s=std::sin(angle);
        auto pose=wheel.frames.front();
        for(auto& v:pose)v={int(std::lround(c*v.x-s*v.y)),int(std::lround(s*v.x+c*v.y)),v.z};
        wheel.frames.push_back(std::move(pose));
    }
    return wheel;
}

// All station structures stand on the plain. The rotor origin is the axle.
Vertex pumping_station(WorldModel& land,const Scene& scene) {
    const auto mill=side(scene,7,-shoulder-650);
    const int x=mill.x,z=mill.z;
    const auto feet=ring(x,0,z,390,390),neck=ring(x,-2240,z,95,95);
    for(unsigned i=0;i<4;++i) {
        const auto a=feet[i],b=neck[i],next=neck[(i+1)%4];
        band(land,ring(a.x,a.y,a.z,50,50),ring(b.x,b.y,b.z,32,32),7);
        // Braced pairs on each face keep the tower open against the sky.
        for(int tier=0;tier<2;++tier) {
            auto low=between(a,b,tier,3),high=between(feet[(i+1)%4],next,tier+1,3);
            Vertex low2=low,high2=high;low2.y-=60;high2.y-=60;
            quad(land,low,high,high2,low2,2);quad(land,low2,high2,high,low,2);
        }
    }
    const Vertex hub{x,-2240,z-190};
    const auto axle=ring(x,-2325,z,100,240),under=ring(x,-2155,z,100,240);
    band(land,under,axle,2);cap(land,axle,2);

    // A low gabled pump house behind the tower, with a shaded service door.
    const int hx=x-1600,hz=z+350;
    const auto base=ring(hx,0,hz,730,580),eaves=ring(hx,-620,hz,730,580);
    band(land,base,eaves,6);
    const Vertex front{hx,-940,hz-580},back{hx,-940,hz+580};
    quad(land,eaves[0],eaves[1],front,front,8);
    quad(land,eaves[3],back,eaves[2],eaves[2],8);
    quad(land,eaves[0],front,back,eaves[3],2);
    quad(land,front,eaves[1],eaves[2],back,7);
    const auto door=ring(hx-170,0,hz-595,175,25),lintel=ring(hx-170,-430,hz-595,175,25);
    band(land,door,lintel,2);cap(land,lintel,3);

    const auto tank=side(scene,12,shoulder+550);
    constexpr int panels=16;
    auto shell=[&](int i,int radius,int h) -> Vertex {
        const double a=i*2*std::acos(-1.)/panels;
        return {tank.x+int(std::lround(radius*std::cos(a))),-h,tank.z+int(std::lround(radius*std::sin(a)))};
    };
    // The stepped shell includes the concrete plinth, turquoise panels,
    // two pale hoops and a shallow conical lid.
    constexpr std::array<int,8> radius{970,970,870,870,890,870,870,925};
    constexpr std::array<int,8> height{0,160,200,600,665,680,1430,1500};
    constexpr std::array<std::uint8_t,7> color{7,8,4,3,3,4,3};
    for(int i=0;i<panels;++i) {
        const int j=(i+1)%panels;
        for(unsigned k=0;k<color.size();++k)
            quad(land,shell(i,radius[k],height[k]),shell(j,radius[k],height[k]),
                      shell(j,radius[k+1],height[k+1]),shell(i,radius[k+1],height[k+1]),color[k]);
        const Vertex peak{tank.x,-1740,tank.z};
        quad(land,shell(i,925,1500),shell(j,925,1500),peak,peak,4);
    }
    // The access ladder stands proud of the shell on its approach side.
    const int ladder_x=tank.x-913;
    for(int side:{-1,1}) {
        const int rail_z=tank.z+side*118;
        band(land,ring(ladder_x,-165,rail_z,18,17),ring(ladder_x,-1530,rail_z,18,17),2);
    }
    for(int h=290;h<1450;h+=165) {
        const auto rung=ring(ladder_x,-h,tank.z,17,118);
        const auto top=ring(ladder_x,-h-24,tank.z,17,118);
        band(land,rung,top,3);cap(land,top,3);
    }
    return hub;
}

#include "custom_city.inc"

WorldModel kestrel() {
    auto m=model(kestrel_shell);
    const auto sill=ring(0,24,-3,66,100),belt=ring(0,-18,-3,78,110);
    quad(m,sill[3],sill[2],sill[1],sill[0],2);
    const Ring deck{{{-70,-42,-106},{70,-42,-106},{63,-31,105},{-63,-31,105}}};
    band(m,sill,belt,2);band(m,belt,deck,4);cap(m,deck,4);
    const auto glass=ring(0,-44,-17,54,54),roof=ring(0,-89,-28,39,27);
    band(m,glass,roof,13);cap(m,roof,3);
    for(int x:{-50,50}) {
        band(m,ring(x,-42,-76,5,5),ring(x,-64,-76,5,5),2);
        quad(m,{x-10,-33,-110},{x+10,-33,-110},{x+10,-23,-112},{x-10,-23,-112},5);
    }
    const auto wing=ring(0,-64,-78,84,13),wing_top=ring(0,-70,-78,84,13);
    band(m,wing,wing_top,2);cap(m,wing_top,3);
    quad(m,{-7,-43,-90},{7,-43,-90},{7,-32,97},{-7,-32,97},3);
    return m;
}
}
void tune_kestrel(VehicleTuning& tuning) {
    // A shorter launch pull for the light sprint chassis. Byte zero is the
    // speed-to-bucket scale; the sixteen following bytes are torque samples.
    for(unsigned gear:{1U,2U})for(unsigned sample=1;sample<=16;++sample) {
        auto& torque=tuning.gear_curves[gear][sample];
        torque=std::uint8_t(std::min(255u,(unsigned(torque)*9+4)/8));
    }
}
template<std::size_t N>
static Scene loop_scene(const std::array<Point,N>& route,CourseId course) {
    Scene scene{};scene.authored=course;scene.surface_flags=0x108;
    for(unsigned i=0;i<route.size();++i)for(int t=0;t<samples;++t) {
        auto coord=[&](int axis){return cubic(route[(i+route.size()-1)%route.size()][axis],
            route[i][axis],route[(i+1)%route.size()][axis],route[(i+2)%route.size()][axis],t);};
        const int section=int(scene.points.size());
        scene.points.push_back({std::uint32_t(section*8),{coord(0),course==kasumi_loop.id?city_grade(section):wash_depth(section)-rim_height(section),coord(1)},605});
    }
    for(unsigned i=0;i<4;++i)
        scene.grid[i]=GridSlot{{route[0][0]+(i%2?1:-1)*(course==kasumi_loop.id?560:780),course==kasumi_loop.id?deck_y:surface,1400-int(i)*1000},0};
    return scene;
}
Scene mesa_scene() {return loop_scene(rim,ember_mesa.id);}
Scene kasumi_scene() {return loop_scene(city_route,kasumi_loop.id);}
std::span<const Vertex> kasumi_lamps() {
    static const auto bulbs=[] {
        const auto scene=kasumi_scene();
        std::vector<Vertex> result;
        for(unsigned i=0;i<scene.points.size();i+=3)result.push_back(city_bulb(scene,i));
        return result;
    }();
    return bulbs;
}
// Finite wall segments share their inside edge with the pavement mesh.
void resolve_kasumi_wall(VehicleState& body,RacePosition previous,const std::array<std::uint16_t,9>& orientation) {
    struct Wall { double x,z,tx,tz,nx,nz,length; };
    static const auto walls=[] {
        std::vector<Wall> result;
        const auto scene=kasumi_scene();
        for(unsigned i=0;i<scene.points.size();++i) {
            const auto a=city_section(scene,i),b=city_section(scene,(i+1)%scene.points.size());
            for(int side:{-1,1}) {
                const int k=side<0?2:7;
                const double dx=b[k].x-a[k].x,dz=b[k].z-a[k].z,len=std::hypot(dx,dz);
                result.push_back({double(a[k].x),double(a[k].z),dx/len,dz/len,side*dz/len,-side*dx/len,len});
            }
        }
        return result;
    }();
    double x=std::int16_t(body.word(0x1e)),z=std::int16_t(body.word(0x22));
    double vx=std::int16_t(body.word(0x2e)),vz=std::int16_t(body.word(0x32));
    // The capsule covers the outside tyres and nose. A normal impulse keeps
    // tangential velocity so a glancing hit lets the driver slide and steer away.
    const double fx=std::int16_t(orientation[6])/32768.,fz=std::int16_t(orientation[8])/32768.;
    bool touched=false;
    for(int pass=0;pass<2;++pass)for(const auto& wall:walls) {
        const double dx=x-wall.x,dz=z-wall.z;
        const double radius=121+68*std::abs(fx*wall.nx+fz*wall.nz);
        const double dist=dx*wall.nx+dz*wall.nz,depth=dist+radius;
        if(depth<=0)continue;
        const double ox=previous[0]-wall.x,oz=previous[2]-wall.z;
        const double old_dist=ox*wall.nx+oz*wall.nz;
        if(old_dist>1)continue; // Outside half-plane of an unrelated bend.
        const double t=dist>old_dist?std::clamp((-radius-old_dist)/(dist-old_dist),0.,1.):1.;
        const double along=std::lerp(ox,dx,t)*wall.tx+std::lerp(oz,dz,t)*wall.tz;
        if(along < -115 || along > wall.length+115)continue;
        x-=depth*wall.nx;z-=depth*wall.nz;
        const double outward=vx*wall.nx+vz*wall.nz;
        if(outward>0){vx-=outward*wall.nx;vz-=outward*wall.nz;}
        touched=true;
    }
    if(touched) {
        body.word(0x1e)=std::uint16_t(std::lround(x));body.word(0x22)=std::uint16_t(std::lround(z));
        body.word(0x2e)=std::uint16_t(std::lround(vx));body.word(0x32)=std::uint16_t(std::lround(vz));
    }
}
void add_custom_course_walls(std::vector<CourseWall>& walls,const Scene& scene) {
    if(scene.authored==kasumi_loop.id) {
        for(unsigned i=0;i<scene.points.size();++i) {
            const auto a=city_section(scene,i),b=city_section(scene,(i+1)%scene.points.size());
            for(auto [foot,top]:{std::pair{2,1},std::pair{7,8}})
                walls.push_back({a[foot],b[foot],a[top].y,b[top].y});
        }
    } else {
        constexpr int rail=-width-235;
        for(unsigned i=21;i<26;++i) {
            const auto a=side(scene,i,rail,130),b=side(scene,i+1,rail,130);
            walls.push_back({a,b,a.y-80,b.y-80});
        }
    }
}
void tune_cargo(VehicleTuning& tuning) {
    for(unsigned gear:{1U,2U,3U})for(unsigned n=1;n<=16;++n) {
        auto& torque=tuning.gear_curves[gear][n];
        torque=std::uint8_t(std::min(255u,unsigned(torque)*(gear==1?17:14)/16));
    }
}
std::vector<CollisionShape> custom_collision(const Scene& scene) {
    std::vector<CollisionShape> shapes;
    for(unsigned i=0;i<scene.points.size();++i) {
        const auto j=(i+1)%scene.points.size();
        const auto section=scene.authored==kasumi_loop.id?city_section:cross_section;
        const auto from=section(scene,i),to=section(scene,j);
        const bool slopes=raised(from) || raised(to);
        const unsigned first=slopes?0:1,last=unsigned(from.size()-1)-first;
        const Ring road{from[first],from[last],to[last],to[first]};
        auto a=scene.points[i].position,b=scene.points[j].position;
        const int x=(a.x+b.x)/2,z=(a.z+b.z)/2;
        CollisionShape shape{};shape.command_offset=i+1;shape.position={std::int16_t(x),surface,std::int16_t(z)};
        // Broadphase around the whole earthwork. Precise wheel support uses
        // the same triangles as the visible road, shoulder and fill slope.
        shape.descriptor_address=scene.authored==kasumi_loop.id?city_road:mesa_road;
        shape.extents[1]=3600;shape.object_flags=0x181|0x40;
        for(unsigned k=0;k<4;++k) {
            const auto p=road[k],q=road[(k+1)%4];
            const auto length=std::hypot(q.x-p.x,q.z-p.z);
            shape.extents[0]=std::max(shape.extents[0],std::uint16_t(1+std::abs(p.x-x)));
            shape.extents[2]=std::max(shape.extents[2],std::uint16_t(1+std::abs(p.z-z)));
            shape.planes.push_back({{std::int16_t(p.x-x),0,std::int16_t(p.z-z)},
                {std::int16_t(32767*(q.z-p.z)/length),0,std::int16_t(-32767*(q.x-p.x)/length)},0x81});
        }
        shapes.push_back(std::move(shape));
    }
    return shapes;
}
std::optional<MesaSurface> custom_surface(unsigned descriptor,unsigned section,RacePosition wheel) {
    auto cache=[](const Scene& scene,auto make_section) {
        std::vector<Section> result;
        for(unsigned i=0;i<scene.points.size();++i)result.push_back(make_section(scene,i));
        return result;
    };
    static const auto mesa_rows=cache(mesa_scene(),cross_section);
    static const auto city_rows=cache(kasumi_scene(),city_section);
    const auto& rows=descriptor==city_road?city_rows:mesa_rows;
    const auto& from=rows.at(section);
    const auto& to=rows[(section+1)%rows.size()];
    const unsigned first=descriptor==city_road?2:(raised(from) || raised(to)?0:1);
    for(unsigned k=first;k+1<from.size()-first;++k) {
        const Vertex a=from[k],c=to[k+1];
        for(const auto b:{from[k+1],to[k]}) {
            const int ux=b.x-a.x,uz=b.z-a.z,vx=c.x-a.x,vz=c.z-a.z;
            const int dx=wheel[0]-a.x,dz=wheel[2]-a.z;
            const int det=ux*vz-uz*vx;
            if(!det)continue;
            const int u=dx*vz-dz*vx,v=ux*dz-uz*dx;
            const int sign=det>0?1:-1;
            // Tolerate rounding to the nearest world unit at a shared edge.
            if(sign*u < -std::abs(vx)-std::abs(vz) || sign*v < -std::abs(ux)-std::abs(uz) ||
               sign*(det-u-v) < -std::abs(vx-ux)-std::abs(vz-uz))continue;
            const int uy=b.y-a.y,vy=c.y-a.y;
            const int y=a.y+int((std::int64_t(u)*uy+std::int64_t(v)*vy)/det);
            const double sx=double(uy*vz-vy*uz)/det,sz=double(ux*vy-vx*uy)/det;
            const double scale=32512/std::hypot(sx,1.,sz);
            return MesaSurface{y,{std::int16_t(std::lround(sx*scale)),std::int16_t(-std::lround(scale)),
                                   std::int16_t(std::lround(sz*scale))},descriptor==mesa_road && (k==0 || k+2==from.size())};
        }
    }
    return std::nullopt;
}
void add_custom_models(World& world,CourseId course) {
    world.models.push_back(kestrel());world.models.push_back(cargo_van());
    if(course==kasumi_loop.id){city_world(world);return;}
    if(course!=CourseId::ember_mesa)return;
    const auto scene=mesa_scene();
    world.route_points=scene.points;world.instances.clear();
    auto road=model(mesa_road),land=model(mesa_land);
    for(unsigned i=0;i<scene.points.size();++i) {
        const auto j=unsigned((i+1)%scene.points.size());
        const auto a=cross_section(scene,i),b=cross_section(scene,j);
        const unsigned first=raised(a) || raised(b)?0:1;
        for(unsigned k=first;k+1<a.size()-first;++k) {
            if(i==0 && k>=3 && k<=5) {
                if(k==3)finish_asphalt(road,a,b);
                continue;
            }
            auto color=band_colors[k];
            if((k==1 && i>=3 && i<10) || (k==7 && i>=3 && i<12))color=8;
            if(k==4 && i%2)color=0x2a;
            if((k==2 || k==6) && i%2)color=3;
            quad(road,a[k],a[k+1],b[k+1],b[k],color);
        }
        if(i%3==0)for(int s:{-1,1}) {
            const auto p=side(scene,i,s*(width+190));
            const auto foot=ring(p.x,p.y,p.z,32,32);
            const auto post=ring(p.x,p.y-185,p.z,27,27);
            const auto reflector=ring(p.x,p.y-225,p.z,27,27);
            band(land,foot,post,7);band(land,post,reflector,3);cap(land,reflector,3);
        }
    }
    eastern_landmarks(land,scene);
    // Sandstone towers have a dark talus skirt and a smaller, sunlit crown.
    for(const auto& site:std::array<Point,7>{{{-24200,7600},{-19500,22000},{500,26500},
        {24800,12500},{12200,8200},{-2000,-3000},{11000,-22000}}}) {
        // Unequal corners and an offset cap break up the silhouette on the skyline.
        constexpr std::array<Point,7> footprint{{{-9,-5},{-3,-9},{7,-8},{10,-1},{8,6},{1,10},{-10,5}}};
        auto shelf=[&](int radius,int height,int shift) {
            std::array<Vertex,7> points{};
            for(unsigned k=0;k<points.size();++k)points[k]={
                site[0]+shift+radius*footprint[k][0]/10,height,site[1]+radius*footprint[k][1]/13};
            return points;
        };
        const int size=2200+std::abs(site[0])%1250,peak=-3300-std::abs(site[1])%1800;
        auto foot=shelf(size,240,0),ledge=shelf(size*3/4,peak*6/10,80);
        auto seam=shelf(size*73/100,peak*6/10-150,105),top=shelf(size/2,peak,360);
        for(unsigned k=0;k<foot.size();++k) {
            const auto j=(k+1)%foot.size();
            quad(land,foot[k],foot[j],ledge[j],ledge[k],7);
            quad(land,ledge[k],ledge[j],seam[j],seam[k],8);
            quad(land,seam[k],seam[j],top[j],top[k],6);
            if(k>0 && k+1<foot.size())quad(land,top[0],top[k],top[j],top[j],8);
        }
    }
    desert_floor(land,scene);
    start_area(land);
    const auto hub=pumping_station(land,scene);
    world.models.push_back(pump_wheel());
    world.instances.push_back({3,mesa_wheel,0,2,hub});
    world.models.push_back(std::move(road));world.models.push_back(std::move(land));
    world.instances.push_back({1,mesa_road,0,2,{}});world.instances.push_back({2,mesa_land,0,2,{}});
}
}
