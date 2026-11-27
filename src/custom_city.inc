// Kasumi's west viaduct descends into the station district. The high eastern
// bypass links it to the dock streets, followed by the climb back to the grid.
constexpr std::array<Point,28> city_route{{
    {-21000,0},{-21000,8200},{-18800,15100},{-13600,19100},{-11000,23200},{-4500,24700},
    {1000,21600},{1800,15100},{4800,10700},{11200,10300},{15200,14200},{20300,15400},
    {24200,11700},{23200,5700},{17400,1600},{14500,-4100},{16500,-9300},{22500,-13300},
    {22900,-19200},{17800,-23000},{11600,-21900},{7200,-16300},{2100,-13000},{-4600,-14700},
    {-9600,-21000},{-16200,-21600},{-20800,-16000},{-20600,-8200}}};
static_assert(city_route.size()*samples==kasumi_loop.route_points);
constexpr int deck_y=-1820, street_y=-70;
int city_grade(int i) {
    const double station=ease(i,12,24)-ease(i,40,50);
    const double docks=ease(i,56,66)-ease(i,82,93);
    return deck_y+int(std::lround((street_y-deck_y)*(station+docks)));
}
int city_width(int i) {
    const double ramp=ease(i,7,13)-ease(i,22,27)+ease(i,38,43)-ease(i,49,53)
                     +ease(i,55,60)-ease(i,65,69)+ease(i,79,83)-ease(i,92,97);
    const double boulevard=ease(i,24,28)-ease(i,35,40)+ease(i,66,70)-ease(i,77,82);
    return 1460+int(std::lround(680*boulevard-500*ramp));
}
constexpr std::array<unsigned,3> junctions{28,36,76};
bool at_junction(unsigned i) {
    return std::ranges::any_of(junctions,[i](unsigned node){return i==node || i==node+1;});
}
Vertex city_side(const Scene& scene,unsigned i,int across,int y) {
    const auto n=scene.points.size();
    const auto& a=scene.points[(i+n-1)%n].position;
    const auto& b=scene.points[(i+1)%n].position;
    const auto& p=scene.points[i].position;
    const double length=std::hypot(b.x-a.x,b.z-a.z);
    return {p.x+int(std::lround(across*(b.z-a.z)/length)),y,
            p.z-int(std::lround(across*(b.x-a.x)/length))};
}
Section city_section(const Scene& scene,unsigned i) {
    const int w=city_width(i),y=scene.points[i].position.y;
    // Rows 2..7 are the driving surface. The other rows describe vertical
    // parapets and their caps; wheels must never treat these as road ramps.
    const int curb=at_junction(i)?285:(y==street_y?170:235);
    return {city_side(scene,i,-w-100,y-curb),city_side(scene,i,-w,y-curb),
            city_side(scene,i,-w,y),city_side(scene,i,-w+35,y),
            city_side(scene,i,-14,y),city_side(scene,i,14,y),
            city_side(scene,i,w-35,y),city_side(scene,i,w,y),
            city_side(scene,i,w,y-curb),city_side(scene,i,w+100,y-curb)};
}
void block(WorldModel& m,int x,int z,int bottom,int top,int w,int l,std::uint8_t material) {
    const auto base=ring(x,bottom,z,w,l),roof=ring(x,top,z,w,l);
    band(m,base,roof,material);cap(m,roof,material);
}
WorldModel cargo_van() {
    auto m=model(cargo_shell);
    const auto sill=ring(0,23,0,68,109),bumper=ring(0,9,0,77,119);
    const auto stripe=ring(0,-27,0,76,117),belt=ring(0,-38,0,76,117);
    const Ring roof{{{-67,-130,-110},{67,-130,-110},{67,-130,72},{-67,-130,72}}};
    band(m,sill,bumper,2);band(m,bumper,stripe,5);band(m,stripe,belt,9);
    // Rear doors wrap around the high box. Only the forward side panels are glass.
    const std::array<int,6> door_edges{0,2,14,16,28,30};
    const std::array<int,4> door_heights{0,4,11,13};
    auto door=[&](int x,int h) {
        return between(between(belt[0],roof[0],h,13),between(belt[1],roof[1],h,13),x,30);
    };
    for(unsigned i=0;i+1<door_edges.size();++i)for(unsigned j=0;j+1<door_heights.size();++j) {
        const int l=door_edges[i],r=door_edges[i+1],b=door_heights[j],t=door_heights[j+1];
        quad(m,door(l,b),door(r,b),door(r,t),door(l,t),j==1 && (i==1 || i==3)?13:19);
    }
    quad(m,belt[2],belt[3],roof[3],roof[2],13);
    for(int s:{-1,1}) {
        auto side_panel=[&](int z0,int z1,int roof_z1,int roof_z0,std::uint8_t color){
            const Vertex a{s*76,-38,z0},b{s*76,-38,z1},c{s*67,-130,roof_z1},d{s*67,-130,roof_z0};
            if(s>0)quad(m,a,b,c,d,color);else quad(m,d,c,b,a,color);
        };
        side_panel(-117,34,28,-110,19);side_panel(34,40,34,28,2);side_panel(40,117,72,34,13);
        block(m,s*78,58,-55,-72,12,9,2);
        block(m,s*68,-119,5,-32,6,4,24);
        block(m,s*68,-119,-23,-32,6,5,9);
        block(m,s*58,118,-5,-23,13,4,3);
    }
    cap(m,roof,19);
    const Vertex split_a{-2,-38,-118},split_b{2,-38,-118},split_c{2,-130,-111},split_d{-2,-130,-111};
    quad(m,split_a,split_b,split_c,split_d,2);
    block(m,0,-119,18,6,76,5,2);
    block(m,0,-120,0,-16,19,3,3);
    block(m,8,-118,-46,-51,6,3,2);
    for(int z:{-66,35})block(m,0,z,-130,-137,65,5,16);
    // Fit the shell over the outside track of the Coupe-based running gear.
    for(auto& v:m.frames.front()){v.x=v.x*3/2;v.z=v.z*6/5;}
    return m;
}
void city_building(WorldModel& land,int x,int z,int w,int l,int tiers,int pitch,unsigned seed) {
    const std::array<std::uint8_t,4> walls{14,15,16,21};
    const auto wall=walls[seed%walls.size()];
    const int top=-tiers*pitch;
    for(int floor=0;floor<tiers;++floor) {
        const int y=-floor*pitch;
        auto base=ring(x,y,z,w,l),sill=ring(x,y-pitch/2,z,w,l),lintel=ring(x,y-pitch+60,z,w,l);
        band(land,base,sill,wall);
        for(unsigned i=0;i<4;++i) {
            const unsigned j=(i+1)%4;
            const auto glass=(seed+floor+i)%7==0?18:((seed+i)%3==0?17:23);
            if(seed%3!=0) {
                quad(land,sill[i],sill[j],lintel[j],lintel[i],std::uint8_t(glass));
            } else {
                // Apartment bays break up the continuous glass bands of the offices.
                for(int bay=0;bay<3;++bay) {
                    const int left=4*bay;
                    const auto shade=std::uint8_t((seed+floor+bay+i)%5==0?18:glass);
                    auto pane=[&](int l,int r,std::uint8_t color) {
                        quad(land,between(sill[i],sill[j],l,12),between(sill[i],sill[j],r,12),
                             between(lintel[i],lintel[j],r,12),between(lintel[i],lintel[j],l,12),color);
                    };
                    pane(left,left+1,wall);pane(left+1,left+4,shade);
                }
            }
        }
        band(land,lintel,ring(x,y-pitch,z,w,l),wall);
    }
    cap(land,ring(x,top,z,w,l),wall);
    if(seed%3!=0)block(land,x,z,top,top-320,w/2,l/2,16);
    if(seed%5==0) {
        block(land,x,z,top-300,top-1160,32,32,15);
        block(land,x,z,top-1160,top-1230,39,39,5);
    }
}
void city_sign(WorldModel& land,const Scene& scene,unsigned i,bool merge) {
    const int lane_edge=city_width(i), road_y=scene.points[i].position.y;
    const auto base=city_side(scene,i,lane_edge+160,road_y);
    block(land,base.x,base.z,road_y-110,road_y-1290,36,36,15);
    auto face=[&](int x,int height){return city_side(scene,i,x,road_y-height);};
    auto panel=[&](int l,int r,int bottom,int top,std::uint8_t color) {
        auto a=face(l,bottom),b=face(r,bottom),c=face(r,top),d=face(l,top);
        quad(land,a,b,c,d,color);quad(land,d,c,b,a,color);
    };
    panel(-780,lane_edge+160,1240,1310,15);
    panel(-780,520,1310,1770,20);
    // Arrow signs face approaching traffic. Inset lettering and arrows use
    // the same local road axes as their overhead board.
    const auto& p=scene.points[i].position;
    const auto& q=scene.points[i-1].position;
    auto mark=[&](int x,int h){
        auto v=face(x,h);double d=std::hypot(q.x-p.x,q.z-p.z);
        v.x+=int(std::lround(15*(q.x-p.x)/d));v.z+=int(std::lround(15*(q.z-p.z)/d));return v;
    };
    for(int x:{-470,130}) {
        const int shift=merge && x<0?100:0;
        const auto a=mark(x-35,1380),b=mark(x+35,1380),c=mark(x+35+shift,1575),d=mark(x-35+shift,1575);
        quad(land,a,b,c,d,3);
        auto l=mark(x-118+shift,1575),r=mark(x+118+shift,1575),t=mark(x+shift,1700);
        quad(land,l,r,t,t,3);
    }
}
// Across and along are measured in the local road frame, including on bends.
Vertex city_frame(const Scene& scene,unsigned i,int across,int along,int y) {
    const auto center=scene.points[i].position;
    const auto side=city_side(scene,i,across,y);
    const auto right=city_side(scene,i,along,y);
    return {side.x-(right.z-center.z),y,side.z+(right.x-center.x)};
}
constexpr std::array<unsigned,2> turnpikes{32,70};
void city_turnpike(WorldModel& land,const Scene& scene,unsigned i) {
    // Twin carriageways cross the racing street; the piers flank the entire
    // boulevard and the underside is visible through the windscreen.
    auto at=[&](int x,int z,int y){return city_frame(scene,i,x,z,y);};
    // The overhead road stays level across the district. Only the continuous
    // racing roadway changes grade; the cross streets have no launch ramps.
    constexpr std::array spans{-11800,-9300,-6500,-3200,3200,6500,9300,11800};
    constexpr int top=-2100,base=top+280;
    for(int center:{-680,680}) {
        const int l=center-600,r=center+600;
        for(unsigned j=1;j<spans.size();++j) {
            const int x=spans[j-1],xx=spans[j];
            quad(land,at(x,r,base),at(xx,r,base),at(xx,l,base),at(x,l,base),15);
            quad(land,at(x,l,top),at(xx,l,top),at(xx,r,top),at(x,r,top),16);
            for(int z:{l,r})
                quad(land,at(x,z,top-105),at(xx,z,top-105),at(xx,z,base),at(x,z,base),14);
            if(j+1<spans.size()) {
                auto p=at(xx,center,0);
                block(land,p.x,p.z,0,base,150,190,15);
            }
            for(int mark=x+100;mark+520<xx;mark+=1150) {
                constexpr int y=top-4;
                quad(land,at(mark,center-12,y),at(mark+520,center-12,y),
                          at(mark+520,center+12,y),at(mark,center+12,y),3);
            }
        }
    }
}
void city_intersection(WorldModel& land,const Scene& scene,unsigned i) {
    // Cross-street mouths join the outside of the closed kerbs. The racing
    // surface runs straight through, with no coplanar intersection overlay.
    for(int side:{-1,1}) {
        auto arm=[&](unsigned row,int dist){return city_side(scene,row,side*(city_width(row)+100+dist),street_y+12);};
        auto a=arm(i,0),b=arm(i+1,0),c=arm(i+1,4300),d=arm(i,4300);
        auto strip=[&](int l,int r,std::uint8_t col) {
            quad(land,between(a,b,l,100),between(a,b,r,100),between(d,c,r,100),between(d,c,l,100),col);
        };
        strip(0,7,15);strip(7,49,16);strip(49,51,9);strip(51,93,16);strip(93,100,15);
        const auto pole=city_side(scene,i,side*(city_width(i)+150),street_y);
        block(land,pole.x,pole.z,street_y,street_y-780,22,22,15);
        block(land,pole.x,pole.z,street_y-620,street_y-870,65,50,2);
        block(land,pole.x,pole.z,street_y-772,street_y-845,68,53,24);
    }
}
Vertex city_bulb(const Scene& scene,unsigned i) {
    const int side=(i/3)%2?-1:1;
    return city_side(scene,i,side*(city_width(i)-230),scene.points[i].position.y-995);
}
void city_world(World& world) {
    const auto scene=kasumi_scene();
    world.route_points=scene.points;world.instances.clear();
    auto road=model(city_road),land=model(city_land);
    for(unsigned i=0;i<scene.points.size();++i) {
        auto a=city_section(scene,i),b=city_section(scene,(i+1)%scene.points.size());
        const int road_y=scene.points[i].position.y, lane_edge=city_width(i);
        const std::array<int,9> materials{19,at_junction(i)?9:15,3,
            0x2a,i%2?0x2a:3,0x2a,9,at_junction(i)?9:15,19};
        const bool crossing=std::ranges::find(junctions,i)!=junctions.end();
        for(unsigned n=0;n<materials.size();++n) {
            auto& mesh=(n<2 || n>6)?land:road;
            const auto tile=[&](int x0,int x1,int z0,int z1,int divisions,std::uint8_t color) {
                auto v=[&](int x,int z) {
                    return between(between(a[n],a[n+1],x,divisions),between(b[n],b[n+1],x,divisions),z,divisions);
                };
                quad(mesh,v(x0,z0),v(x1,z0),v(x1,z1),v(x0,z1),color);
            };
            if(crossing && (n==0 || n==1 || n==7 || n==8)) {
                for(int j=0;j<10;++j)tile(0,10,j,j+1,10,j%2?2:9);
                continue;
            }
            if(crossing && (n==3 || n==5)) {
                // Inlaid pedestrian crossing at the near side of the junction.
                for(int x=0;x<12;++x)tile(x,x+1,0,2,12,x%2?0x2a:3);
                tile(0,12,2,12,12,0x2a);
                continue;
            }
            if((n==3 || n==5) && i%2==0 && lane_edge>=1770) {
                tile(0,49,0,100,100,0x2a);tile(49,51,0,100,100,3);tile(51,100,0,100,100,0x2a);
                continue;
            }
            if(i==0 && n>=3 && n<=5) {
                if(n==3)finish_asphalt(road,a,b);
            } else quad(mesh,a[n],a[n+1],b[n+1],b[n],std::uint8_t(materials[n]));
        }
        // The soffit follows both ends of each road section, including ramps.
        // Flat, wide pier caps would emerge through the uphill side of a span.
        auto l=a.front(),r=a.back(),ll=b.front(),rr=b.back();
        l.y=r.y=road_y+260;
        ll.y=rr.y=scene.points[(i+1)%scene.points.size()].position.y+260;
        quad(land,a.front(),b.front(),ll,l,14);
        quad(land,a.back(),r,rr,b.back(),14);
        quad(land,l,ll,rr,r,15);
        const auto p=scene.points[i].position;
        if(i%2==0 && road_y<-450)block(land,p.x,p.z,0,road_y+165,190,240,15);
        if(i%3==0) {
            const int side=(i/3)%2?-1:1;
            const auto post=city_side(scene,i,side*(lane_edge+65),road_y);
            const auto bulb=city_bulb(scene,i);
            block(land,post.x,post.z,road_y-125,bulb.y-60,20,20,16);
            auto arm=[&](int across,int along,int y){return city_frame(scene,i,side*across,along,y);};
            auto arm_ring=[&](int y)->Ring {
                return {arm(lane_edge+78,-17,y),arm(lane_edge+78,17,y),
                        arm(lane_edge-230,17,y),arm(lane_edge-230,-17,y)};
            };
            const auto lower=arm_ring(bulb.y-39),upper=arm_ring(bulb.y-70);
            band(land,lower,upper,15);cap(land,upper,15);
            quad(land,lower[3],lower[2],lower[1],lower[0],15);
            block(land,bulb.x,bulb.z,bulb.y-24,bulb.y-67,84,45,16);
            block(land,bulb.x,bulb.z,bulb.y,bulb.y-24,80,42,25);
            const auto lens=ring(bulb.x,bulb.y,bulb.z,80,42);
            quad(land,lens[3],lens[2],lens[1],lens[0],25);
        }
    }
    for(unsigned i:{8u,22u,39u,54u,65u,81u,99u})city_sign(land,scene,i,i==8 || i==54 || i==81);
    for(auto i:junctions)city_intersection(land,scene,i);
    for(auto i:turnpikes)city_turnpike(land,scene,i);
    // City blocks sit on a street grid below the flyover. Reserve clearance
    // around every segment, including the wide side ramps and building corners.
    for(int ix=-7;ix<=7;++ix)for(int iz=-7;iz<=7;++iz) {
        const int x=ix*3800,z=iz*3800;
        const unsigned seed=unsigned(ix+7)*73+unsigned(iz+7)*37;
        const int w=760+int(seed%5)*100,l=720+int(seed%7)*95;
        bool clear=true;
        for(unsigned i=0;i<scene.points.size();++i) {
            const auto a=scene.points[i].position,b=scene.points[(i+1)%scene.points.size()].position;
            const double dx=b.x-a.x,dz=b.z-a.z;
            const double t=std::clamp(((x-a.x)*dx+(z-a.z)*dz)/(dx*dx+dz*dz),0.,1.);
            if(std::hypot(x-a.x-t*dx,z-a.z-t*dz)<city_width(i)+700+std::hypot(w,l)){clear=false;break;}
        }
        // Keep the transverse road corridors open as well as the racing line.
        auto clear_arm=[&](unsigned i,int extent,int half_width) {
            const auto a=scene.points[i].position;
            const auto end=city_side(scene,i,extent,0);
            const double rx=end.x-a.x,rz=end.z-a.z,len=std::hypot(rx,rz);
            const double dx=x-a.x,dz=z-a.z,pad=std::hypot(w,l);
            return std::abs((dx*rx+dz*rz)/len)>extent+pad ||
                   std::abs((dz*rx-dx*rz)/len)>half_width+pad;
        };
        for(auto i:turnpikes)clear=clear && clear_arm(i,11800,1350);
        for(auto i:junctions)clear=clear && clear_arm(i,city_width(i)+4400,2000);
        if(clear)city_building(land,x,z,w,l,4+int(seed%9),380+int(seed%3)*65,seed);
    }
    // Tile streets and pavement edge-to-edge beneath the viaduct.
    for(int x=-7;x<=7;++x)for(int z=-7;z<=7;++z) {
        const int x0=x*3800-1900,z0=z*3800-1900;
        for(int dx:{0,420})for(int dz:{0,420}) {
            const int x1=x0+(dx?3800:420),z1=z0+(dz?3800:420);
            quad(land,{x0+dx,0,z0+dz},{x1,0,z0+dz},{x1,0,z1},{x0+dx,0,z1},dx && dz?14:16);
        }
    }
    world.models.push_back(std::move(road));world.models.push_back(std::move(land));
    world.instances.push_back({1,city_road,0,2,{}});world.instances.push_back({2,city_land,0,2,{}});
}
