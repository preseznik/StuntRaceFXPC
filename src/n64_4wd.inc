// Original rally bodywork, in vehicle-local units (Y points down, +Z is forward).
// The chassis follows the body pose. Each tire follows its own suspension attachment.
namespace n64 {
using Vec = std::array<float,3>;
struct Face { std::array<Vec,4> corners; Vec color; bool glass=false; };
using Mesh = std::vector<Face>;
constexpr unsigned f_type_player_shell=0xa200, f_type_rival_shell=0xa7e8;
constexpr unsigned coupe_player_shell=0xa2a8, coupe_rival_shell=0xa804;
bool original_player_shell(unsigned descriptor) {
    return descriptor==coupe_player_shell || descriptor==f_type_player_shell;
}
float tire_radius(const RenderSprite& wheel) {return wheel.diameter*.43f;}

Mesh rally_body() {
    Mesh mesh;
    const Vec ochre{.92f,.46f,.10f}, gold{1.f,.65f,.19f}, cream{.92f,.90f,.75f};
    const Vec trim{.13f,.16f,.17f}, glass{.19f,.35f,.41f};
    // Chamfer the corners in plan, with separate sill, belt, glazing and roof profiles.
    auto ring=[](float w,float y,float tail,float nose,float bevel) -> std::array<Vec,8> {
        return {{{-w+bevel,y,tail},{w-bevel,y,tail},{w,y,tail+bevel},{w,y,nose-bevel},
                 {w-bevel,y,nose},{-w+bevel,y,nose},{-w,y,nose-bevel},{-w,y,tail+bevel}}};
    };
    const auto sill=ring(75,37,-143,146,17), bumper=ring(84,18,-151,154,19);
    const auto belt=ring(85,-37,-146,148,17), deck=ring(79,-49,-135,140,14);
    const auto window=ring(75,-50,-112,70,10), roof=ring(62,-104,-90,37,10);
    const auto cap=ring(59,-110,-86,35,10);
    auto band=[&](const auto& lower,const auto& upper,Vec color,bool glazing=false) {
        for(unsigned i=0;i<8;++i) {
            const auto j=(i+1)%8;
            mesh.push_back({{lower[i],lower[j],upper[j],upper[i]},color,glazing});
        }
    };
    band(sill,bumper,trim); band(bumper,belt,ochre); band(belt,deck,gold);
    band(deck,window,ochre); band(window,roof,glass,true); band(roof,cap,cream);
    const Vec roof_center{0,-110,-23};
    for(unsigned i=0;i<8;++i)
        mesh.push_back({{cap[i],cap[(i+1)%8],roof_center,roof_center},cream});
    // Rear trim, split tail lamps and two inset side-window pillars.
    auto panel=[&](float left,float right,float top,float bottom,float z,Vec color) {
        mesh.push_back({{Vec{left,top,z},Vec{right,top,z},Vec{right,bottom,z},Vec{left,bottom,z}},color});
    };
    for(float side:{-1.f,1.f}) {
        panel(side*34,side*67,-28,-9,-151,trim);
        panel(side*37,side*64,-26,-18,-152,{1.f,.22f,.09f});
        panel(side*37,side*49,-16,-12,-152,cream);
        panel(side*51,side*64,-16,-12,-152,{.89f,.48f,.09f});
        panel(side*35,side*64,-26,-9,152,{1.f,.92f,.71f});
        mesh.push_back({{Vec{side*75.6f,-50,-36},Vec{side*75.6f,-50,-27},
                         Vec{side*62.6f,-104,-27},Vec{side*62.6f,-104,-36}},trim});
    }
    panel(-20,20,-26,-9,-151,trim);
    panel(-14,14,-23,-12,-152,{.77f,.73f,.59f});
    panel(-62,62,8,18,-152,{.28f,.30f,.29f});
    // Asymmetric paint stripes wrap over the hatch and across the roof.
    for(float x:{-32.f,-23.f}) {
        mesh.push_back({{Vec{x,-38,-147},Vec{x+5,-38,-147},Vec{x+5,6,-151},Vec{x,6,-151}},cream});
        mesh.push_back({{Vec{x,-110.5f,-80},Vec{x+5,-110.5f,-80},Vec{x+5,-110.5f,30},Vec{x,-110.5f,30}},ochre});
    }
    return mesh;
}

Mesh rally_wheel() {
    // Unit radius along Y/Z. Rounded shoulders, staggered tread and inset alloy faces.
    Mesh mesh;
    constexpr unsigned sectors=24;
    auto point=[](float x,float radius,float step) -> Vec {
        const auto angle=step*(6.2831853f/sectors);
        return {x,radius*std::cos(angle),radius*std::sin(angle)};
    };
    auto strip=[&](float x0,float r0,float x1,float r1,Vec color) {
        for(unsigned i=0;i<sectors;++i)
            mesh.push_back({{point(x0,r0,i),point(x0,r0,i+1),point(x1,r1,i+1),point(x1,r1,i)},color});
    };
    const Vec rubber{.14f,.15f,.15f}, tread{.20f,.21f,.21f}, metal{.72f,.73f,.68f};
    strip(-.36f,1,.36f,1,rubber);
    for(float side:{-1.f,1.f}) {
        strip(side*.36f,1,side*.47f,.88f,rubber);
        strip(side*.47f,.88f,side*.47f,.54f,rubber);
        strip(side*.47f,.54f,side*.49f,.46f,metal);
        strip(side*.46f,.47f,side*.46f,0,{.15f,.18f,.19f});
        strip(side*.51f,.17f,side*.51f,0,metal);
        for(unsigned i=0;i<sectors;++i) {
            mesh.push_back({{point(0,1.012f,i),point(0,1.012f,i+.35f),
                             point(side*.33f,1.012f,i+.68f),point(side*.33f,1.012f,i+.33f)},tread});
            if(i%4==0)
                mesh.push_back({{point(side*.50f,.13f,i),point(side*.50f,.13f,i+2),
                                 point(side*.50f,.49f,i+1),point(side*.50f,.49f,i)},metal});
        }
    }
    return mesh;
}

Mesh rally_fender(bool arch_enabled=true) {
    Mesh mesh;
    auto arch=[](float x,float radius,int segment) -> Vec {
        const float angle=(segment-5)*.23f;
        return {x,-radius*std::cos(angle),radius*std::sin(angle)};
    };
    for(int i=0;arch_enabled && i<10;++i) {
        // Graphite extensions at the ends protect the painted crown.
        const Vec finish=(i<2 || i>7)?Vec{.23f,.24f,.23f}:Vec{.86f,.49f,.13f};
        mesh.push_back({{arch(-.59f,1.15f,i),arch(.59f,1.15f,i),
                         arch(.59f,1.15f,i+1),arch(-.59f,1.15f,i+1)},finish});
        for(float x:{-.59f,.59f})
            mesh.push_back({{arch(x,1.15f,i),arch(x,1.05f,i),
                             arch(x,1.05f,i+1),arch(x,1.15f,i+1)},{.20f,.22f,.22f}});
    }
    // +X points inward here. A half axle reaches from each hub to the chassis.
    for(unsigned i=0;i<8;++i) {
        auto axle=[](float x,unsigned j) -> Vec {
            const float t=j*.7853982f;
            return {x,.14f*std::cos(t),.14f*std::sin(t)};
        };
        mesh.push_back({{axle(0,i),axle(1.85f,i),axle(1.85f,i+1),axle(0,i+1)},{.30f,.32f,.32f}});
        auto strut=[&](float x,unsigned j) {
            auto p=axle(x,j);
            p[1]=p[1]*.65f-.65f*x;
            return p;
        };
        mesh.push_back({{strut(0,i),strut(1.15f,i),strut(1.15f,i+1),strut(0,i+1)},{.60f,.58f,.46f}});
    }
    return mesh;
}

Mesh rally_hood() {
    Mesh mesh;
    auto panel=[&](float left,float right,float y,Vec color) {
        mesh.push_back({{Vec{left,y,67},Vec{right,y,67},Vec{right,y+1,138},Vec{left,y+1,138}},color});
    };
    panel(-67,67,-51,{.91f,.48f,.12f});
    for(float x:{-32.f,-23.f})panel(x,x+5,-51.3f,{.94f,.88f,.69f});
    return mesh;
}

Mesh opponent_trim(unsigned descriptor) {
    Mesh mesh;
    const Vec rubber{.15f,.17f,.18f},metal{.47f,.49f,.49f},lamp{.98f,.88f,.65f};
    // Place trim against each extracted shell in its own local coordinates.
    auto block=[&](Vec lo,Vec hi,Vec color) {
        const auto [x,y,z]=lo;const auto [X,Y,Z]=hi;
        std::array<Vec,8> p{{{x,y,z},{X,y,z},{X,y,Z},{x,y,Z},{x,Y,z},{X,Y,z},{X,Y,Z},{x,Y,Z}}};
        for(std::array<int,4> f:std::array<std::array<int,4>,6>{{{0,1,2,3},{7,6,5,4},{0,4,5,1},{1,5,6,2},{2,6,7,3},{3,7,4,0}}})
            mesh.push_back({{p[f[0]],p[f[1]],p[f[2]],p[f[3]]},color});
    };
    // A longitudinal frame ties both axle lines to the underside of the shell.
    const float rear=descriptor==0xa820?-150.f:-110.f;
    block({-13,13,rear-2},{13,23,132},rubber);
    for(float z:{rear,130.f})block({-84,13,z-3},{84,21,z+3},metal);
    if(descriptor==f_type_rival_shell) { // Single-seater rear wing, endplates and twin exhaust.
        for(float side:{-1.f,1.f}) {
            float x=side*16;
            block({x-2,-62,-128},{x+2,-24,-124},metal);
            block({x-3,-29,-127},{x+3,-23,-84},rubber);
            x=side*51;
            block({x-1,-71,-150},{x+1,-59,-101},rubber);
            x=side*11;
            block({x-4,-11,-99},{x+4,-4,-85},metal);
        }
    } else if(descriptor==coupe_rival_shell) { // Coupe sill, front lamps and spoiler lip.
        block({-65,-53,-139},{65,-48,-135},rubber);
        for(float side:{-1.f,1.f}) {
            float x=side*70;
            block({x-2,10,-98},{x+2,17,24},rubber);
            x=side*34;
            block({x-8,-27,99},{x+8,-23,104},lamp);
        }
    } else if(descriptor==0xa820) { // Pickup bed, bumper and running boards.
        for(float x:{-48.f,-24.f,0.f,24.f,48.f})
            block({x-2,-70,-115},{x+2,-68,-7},rubber);
        block({-61,-4,-145},{61,4,-137},metal);
        for(float x:{-72.f,72.f})
            block({x-5,0,-63},{x+5,7,58},rubber);
    }
    return mesh;
}

void append_instruments(Mesh& mesh) {
    const Vec charcoal{.13f,.16f,.18f},alloy{.58f,.60f,.56f};
    auto quad=[&](Vec a,Vec b,Vec c,Vec d,Vec color){mesh.push_back({{a,b,c,d},color});};
    auto circle=[](Vec center,float radius,int step) {
        float a=step*6.2831853f/24;
        return Vec{center[0]+radius*std::cos(a),center[1]+radius*std::sin(a),center[2]};
    };
    auto ring=[&](Vec center,float inner,float outer,Vec color) {
        for(int i=0;i<24;++i)quad(circle(center,inner,i),circle(center,outer,i),
            circle(center,outer,i+1),circle(center,inner,i+1),color);
    };
    for(float x:{-33.f,-12.f}) {
        const Vec dial{x,-53,41};
        ring(dial,6.7f,7.5f,alloy);
        ring(dial,0,6.7f,charcoal);
        const Vec marks{x,-53,40.8f};
        for(int tick=10;tick<=26;tick+=2)quad(circle(marks,4.9f,tick),circle(marks,6.3f,tick),
            circle(marks,6.3f,tick+1),circle(marks,4.9f,tick+1),{.91f,.87f,.71f});
        quad({x-1,-53,40.5f},{x,-52,40.5f},{x+4,-56,40.5f},{x+3,-57,40.5f},{1.f,.52f,.14f});
    }
    const Vec hub{-18,-52,24};
    ring(hub,14,17,{.20f,.23f,.22f});
    for(int i:{0,8,16})quad(circle(hub,3,i-1),circle(hub,14,i-1),circle(hub,14,i),circle(hub,3,i),alloy);
    ring({hub[0],hub[1],hub[2]-.3f},0,4.5f,charcoal);
}

Mesh single_seat_dash() {
    Mesh mesh;
    append_instruments(mesh);
    // A central driving position behind the scuttle at Z=50, Y=-44.
    for(auto& face:mesh)for(auto& p:face.corners) {
        p[0]=(p[0]+18.f)*.77f;
        p[1]=(p[1]+52.f)*.82f-51.f;
        p[2]+=10.f;
    }
    mesh.push_back({{Vec{-38,-59,55},Vec{38,-59,55},Vec{40,-27,58},Vec{-40,-27,58}},{.12f,.15f,.16f}});
    return mesh;
}

Mesh rally_cockpit(bool coupe=false) {
    Mesh mesh=coupe?Mesh{}:rally_hood();
    const Vec charcoal{.13f,.16f,.18f},vinyl{.28f,.32f,.32f};
    auto quad=[&](Vec a,Vec b,Vec c,Vec d,Vec color){mesh.push_back({{a,b,c,d},color});};
    quad({-73,-48,70},{73,-48,70},{70,-60,44},{-70,-60,44},vinyl);
    quad({-70,-60,44},{70,-60,44},{73,-10,36},{-73,-10,36},vinyl);
    quad({-73,-10,36},{73,-10,36},{73,19,-85},{-73,19,-85},charcoal);
    for(float side:{-1.f,1.f}) {
        quad({side*75,-48,70},{side*67,-48,70},{side*52,-105,36},{side*60,-105,36},vinyl);
        quad({side*75,-48,70},{side*73,-39,70},{side*73,-39,-84},{side*75,-48,-84},charcoal);
        quad({side*73,-39,70},{side*73,20,49},{side*73,20,-84},{side*73,-39,-84},vinyl);
        quad({side*75,-48,-30},{side*75,-48,-36},{side*60,-105,-32},{side*60,-105,-26},vinyl);
    }
    quad({-60,-105,36},{60,-105,36},{60,-101,37},{-60,-101,37},charcoal);
    quad({-60,-105,36},{60,-105,36},{60,-105,-88},{-60,-105,-88},{.38f,.38f,.34f});
    append_instruments(mesh);
    if(coupe) {
        // Fit the shared instruments to the Coupé's narrower cabin. The
        // windscreen runs from Z=0 at the roof to Z=40 at the scuttle.
        for(auto& face:mesh)for(auto& p:face.corners) {
            p[0]*=.68f;
            p[1]=.88f*(p[1]+48.f)-60.f;
            p[2]=(p[2]-36.f)*(40.f/34.f);
        }
        quad({-52,-60,40},{52,-60,40},{32,-20,120},{-32,-20,120},{1.f,.66f,.04f});
    }
    return mesh;
}

template<class Normalize>
void draw_car(std::vector<GpuVertex>& triangles,const RenderVehicle& car,bool n64_lighting,const Normalize& normalize,bool interior=false) {
    static const Mesh body=rally_body(),wheel=rally_wheel(),fender=rally_fender(),axle=rally_fender(false);
    auto draw=[&](const Mesh& mesh,const Vec& center,float scale,const std::array<float,9>& axes) {
        for(const auto& face:mesh) {
            std::array<Vec,4> corners;
            for(unsigned i=0;i<4;++i) {
                corners[i]=center;
                for(unsigned a=0;a<3;++a) for(unsigned b=0;b<3;++b)
                    corners[i][a]+=face.corners[i][b]*scale*axes[b*3+a];
            }
            Vec u{},v{};
            for(unsigned a=0;a<3;++a) {u[a]=corners[1][a]-corners[0][a];v[a]=corners[2][a]-corners[0][a];}
            const Vec n{u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
            const float shade=.68f+.32f*std::abs(.36f*n[0]-.85f*n[1]+.38f*n[2])/std::max(.001f,std::hypot(n[0],n[1],n[2]));
            std::array<GpuVertex,4> quad;
            for(unsigned i=0;i<4;++i) {
                const auto p=normalize(corners[i]);
                float vertex_light=n64_lighting?shade:1.f;
                if(n64_lighting && &mesh==&body) {
                    // Round the broad chassis lighting across its chamfered corners.
                    const float nx=face.corners[i][0]/86.f,nz=face.corners[i][2]/154.f;
                    const float sun=(.48f*axes[0]+.32f*axes[2])*nx+(.48f*axes[6]+.32f*axes[8])*nz;
                    vertex_light=std::clamp(shade+.19f*sun,.53f,1.08f);
                }
                quad[i]={p[0],p[1],p[2],face.color[0]*vertex_light,face.color[1]*vertex_light,
                         face.color[2]*(n64_lighting?.10f+.90f*vertex_light:1.f)};
                if(face.glass) {quad[i].u=(i==1 || i==2);quad[i].v=(i>=2);quad[i].texture=-4;}
            }
            for(int i:{0,1,2,0,2,3}) triangles.push_back(quad[i]);
        }
    };
    if(interior && car.descriptor==f_type_player_shell) {
        static const Mesh dash=single_seat_dash();
        draw(dash,car.position,1,car.orientation);
    } else if(interior) {
        static const Mesh cockpit=rally_cockpit(),coupe_cockpit=rally_cockpit(true);
        draw(car.descriptor==coupe_player_shell?coupe_cockpit:cockpit,car.position,1,car.orientation);
        return;
    }
    const bool rally_player=car.player && !original_player_shell(car.descriptor);
    if(rally_player) {
        draw(body,car.position,1,car.orientation);
        if(!n64_lighting) {
            static const Mesh hood=rally_hood();
            draw(hood,car.position,1,car.orientation);
        }
    } else {
        static const Mesh single=opponent_trim(f_type_rival_shell),coupe=opponent_trim(coupe_rival_shell),pickup=opponent_trim(0xa820);
        if(car.descriptor==f_type_rival_shell || car.descriptor==f_type_player_shell)draw(single,car.position,1,car.orientation);
        if(car.descriptor==coupe_rival_shell || car.descriptor==coupe_player_shell)draw(coupe,car.position,1,car.orientation);
        if(car.descriptor==0xa820)draw(pickup,car.position,1,car.orientation);
    }
    for(const auto& sprite:car.sprites) if(sprite.wheel) {
        auto axes=car.orientation;
        const float c=std::cos(sprite.steer),s=std::sin(sprite.steer);
        const float roll_c=std::cos(sprite.spin),roll_s=std::sin(sprite.spin);
        for(unsigned a=0;a<3;++a) {
            axes[a]=c*car.orientation[a]-s*car.orientation[6+a];
            const float forward=s*car.orientation[a]+c*car.orientation[6+a];
            axes[3+a]=roll_c*car.orientation[3+a]+roll_s*forward;
            axes[6+a]=-roll_s*car.orientation[3+a]+roll_c*forward;
        }
        const float radius=tire_radius(sprite);
        draw(wheel,sprite.position,radius,axes);
        auto mounting=car.orientation;
        float lateral=0;
        for(unsigned a=0;a<3;++a)lateral+=(sprite.position[a]-car.position[a])*mounting[a];
        if(lateral>0)for(unsigned a=0;a<3;++a)mounting[a]=-mounting[a];
        draw(rally_player?fender:axle,sprite.position,radius,mounting);
    }
}
} // namespace n64
