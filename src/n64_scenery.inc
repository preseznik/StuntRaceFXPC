namespace n64 {
constexpr unsigned left_stand=0xab68, right_stand=0xab84, fuel_station=0xad98;
constexpr unsigned red_cottage=0xac2c, blue_cottage=0xac48;

// Local coordinates keep the siding vertical and the pumps and windows aligned on rotated placements.
void building_surface(GpuVertex& out,unsigned model,unsigned material,const srf::Vertex& v,bool along_z) {
    const float x=v.x/90.f, y=-v.y/90.f, z=v.z/90.f;
    auto paint=[&](int texture,float u,float t,Vec color) {
        out.texture=float(texture);out.u=u;out.v=t;out.r=color[0];out.g=color[1];out.b=color[2];
    };
    if(model==left_stand || model==right_stand) {
        if(material==0x26 || material==0x20)
            paint(-11,z,x,material==0x26?Vec{.31f,.45f,.47f}:Vec{.24f,.32f,.33f});
        else if(material==0xd5)
            paint(-5,z,x,{.34f,.32f,.29f});
        else
            paint(-7,along_z?z:x,y,{1,1,1});
    } else if(model==fuel_station) {
        if(material==0x01) paint(-16,z,x,{1,1,1});
        else if(material>=0x30 && material<=0x37) {
            // Each broad cabinet face runs from |z|=640 to 1280 and from y=0 to -480.
            const bool broad=material==0x34 || material==0x37;
            paint(broad?-15:-17,(std::abs(v.z)-640)/640.f,-v.y/480.f,{.75f,.19f,.105f});
        } else if(material==0x28 || material==0x2a) paint(-8,z,y,{1,1,1});
        else paint(-19,z,x,{.70f,.72f,.65f});
    } else if(model==red_cottage || model==blue_cottage) {
        const bool red=model==red_cottage;
        if(material<0x40) paint(-12,z,x,red?Vec{.61f,.275f,.18f}:Vec{.23f,.37f,.46f});
        else if(material>=0x5f && material<=0x62) {
            const float center=red?480.f:-480.f;
            const float u=along_z?(z+(red?160.f/90:0)):(v.x-center)/90;
            const bool entrance=!along_z && v.z<0 && material==(red?0x60u:0x61u);
            const bool garage=!red && material==0x5f && v.x==1120;
            paint(garage?-18:entrance?-13:-20,u,y,red?Vec{.83f,.69f,.50f}:Vec{.81f,.78f,.65f});
        } else paint(-14,along_z?z:x,y,{.55f,.32f,.24f});
    }
}

// Mesh additions share the immutable scenery buffer. Local Y points down.
template<class Place>
void trackside_details(std::vector<GpuVertex>& mesh,unsigned model,const Place& place) {
    const bool mirrored=model==right_stand;
    const int side=mirrored?-1:1;
    auto quad=[&](Vec a,Vec b,Vec c,Vec d,Vec color) {
        for(auto v:{a,b,c,a,c,d}) {
            auto p=place(srf::Vertex{int(side*v[0]),int(v[1]),int(v[2])});
            mesh.push_back({p[0],p[1],p[2],color[0],color[1],color[2],v[2]/90,v[0]/90,-5});
        }
    };
    auto fascia=[&](Vec a,Vec b,float depth,Vec color) {
        auto c=b,d=a;c[1]+=depth;d[1]+=depth;
        quad(a,b,c,d,color);
    };
    if(model==fuel_station) {
        // Follow the original butterfly canopy, including its low central valley.
        for(float x:{-1920.f,1920.f}) {
            fascia({x,-1600,-1920},{x,-1600,1920},72,{.71f,.23f,.13f});
            for(float z:{-1920.f,1920.f})
                fascia({x,-1600,z},{0,-960,z},72,{.71f,.23f,.13f});
        }
        for(float x:{-880.f,880.f})for(float z:{-960.f,960.f}) {
            const Vec a{x-148,-24,z-385},b{x+148,-24,z-385},c{x+148,-24,z+385},d{x-148,-24,z+385};
            quad(a,b,c,d,{.69f,.65f,.54f});
            for(auto edge:{std::pair{a,b},std::pair{b,c},std::pair{c,d},std::pair{d,a}})
                fascia(edge.first,edge.second,24,{.58f,.52f,.39f});
        }
        return;
    }
    // Both mirrored D5 seating faces have the same rise and run.
    for(float x:{-960.f,960.f})fascia({x,-960,-3840},{x,-960,3840},38,{.24f,.38f,.39f});
    for(float z:{-3840.f,3840.f})fascia({-960,-960,z},{960,-960,z},38,{.24f,.38f,.39f});
    unsigned seed=mirrored?417u:381u;
    auto random=[&](unsigned range){seed=1664525u*seed+1013904223u;return (seed>>8)%range;};
    constexpr Vec shirts[]={{.78f,.37f,.20f},{.32f,.53f,.60f},{.91f,.72f,.39f},{.84f,.81f,.69f},
                            {.58f,.29f,.32f},{.32f,.45f,.37f},{.26f,.32f,.45f}};
    constexpr Vec skin[]={{.84f,.62f,.46f},{.58f,.38f,.27f},{.73f,.49f,.34f},{.91f,.72f,.56f}};
    for(int row=0;row<6;++row) {
        const float front=768.f-row*288, back=front-288, y=-192.f-(row+1)*(448.f/6)-3;
        quad({front,y,-3840},{front,y,3840},{back,y,3840},{back,y,-3840},{.51f,.45f,.34f});
        quad({front,y,-3840},{front,y+76,-3840},{front,y+76,3840},{front,y,3840},{.30f,.32f,.31f});
        for(int seat=-19;seat<=19;++seat) {
            if(std::abs(seat)==10 || random(7)==0)continue;
            const float x=front-122+float(random(29)), z=seat*194.f+float(random(27))-13;
            const float shoulder=y-69-float(random(20));
            // Four-sided tapered sections give heads and shoulders a silhouette from either approach.
            auto section=[&](float bottom,float top,float base,float crown,Vec color) {
                const Vec lower[]={{x+base*.55f,bottom,z-base},{x+base*.55f,bottom,z+base},
                                   {x-base*.55f,bottom,z+base},{x-base*.55f,bottom,z-base}};
                const Vec upper[]={{x+crown*.55f,top,z-crown},{x+crown*.55f,top,z+crown},
                                   {x-crown*.55f,top,z+crown},{x-crown*.55f,top,z-crown}};
                for(int i=0;i<4;++i) {
                    auto light=color;for(auto& channel:light)channel*=i==0?.98f:i==2?.69f:.82f;
                    quad(lower[i],lower[(i+1)%4],upper[(i+1)%4],upper[i],light);
                }
                quad(upper[0],upper[1],upper[2],upper[3],color);
            };
            section(y,shoulder,32,47,shirts[random(std::size(shirts))]);
            section(shoulder,shoulder-29,18,22,skin[random(std::size(skin))]);
            section(shoulder-29,shoulder-41,23,17,{.23f,.18f,.15f});
        }
    }
}
}
