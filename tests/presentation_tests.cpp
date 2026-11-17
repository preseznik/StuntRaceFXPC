#include "srf/presentation.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}}
int main() {
    try {
        // Scenery yaw shares the recovered static-object convention; pitch and
        // roll use the same handedness as vehicle poses, without moving origin.
        const srf::Vertex local{17,-29,43},origin{100,200,-300};
        for(unsigned yaw=0;yaw<256;++yaw) {
            const auto fixed=srf::world_position(local,{0,0,static_cast<std::uint8_t>(yaw),0,origin});
            const auto animated=srf::scenery_position(local,{0,0,origin,static_cast<std::uint8_t>(yaw)});
            for(unsigned a=0;a<3;++a) require(std::abs(fixed[a]-animated[a])<1e-9,"Scenery yaw must preserve ROM placements");
        }
        const auto pitched=srf::scenery_position({0,0,32},{0,0,origin,0,64});
        const auto rolled=srf::scenery_position({32,0,0},{0,0,origin,0,0,64});
        require(std::abs(pitched[0]-100)<1e-9 && std::abs(pitched[1]-232)<1e-9 && std::abs(pitched[2]+300)<1e-9,"Quarter pitch orientation");
        require(std::abs(rolled[0]-100)<1e-9 && std::abs(rolled[1]-168)<1e-9 && std::abs(rolled[2]+300)<1e-9,"Quarter roll orientation");
        // Original CPU selector boundaries, including the negative-angle bug.
        require(srf::wheel_sprite_bank(0xff,0)==0,"Minus one degree must use the level wheel");
        require(srf::wheel_sprite_bank(0xfa,0)==0,"Negative half-roll truncates toward zero");
        require(srf::wheel_sprite_bank(0xf9,0)==-4,"Signed half-roll must keep the arithmetic-shift boundary");
        require(srf::wheel_sprite_bank(0xf7,0)==-4,"Negative bank threshold");
        require(srf::wheel_sprite_bank(8,63)==4 && srf::wheel_sprite_bank(8,64)==-4,"Near side-view boundary");
        require(srf::wheel_sprite_bank(8,192)==-4 && srf::wheel_sprite_bank(8,193)==4,"Far side-view boundary is inclusive");
        require(srf::wheel_sprite_bank(5,11,254)==0,"Original captured camera roll");
        require(srf::wheel_sprite_bank(127,0)==8 && srf::wheel_sprite_bank(128,0)==-8,"Bank clamp");
        require(srf::interpolated_angle_byte(0,65500,.5f)==255,"Negative angle wrap");
        require(srf::interpolated_angle_byte(65500,0,1)==0,"Positive angle wrap");
        require(srf::interpolated_angle_byte(256,256,0)==1,"Exact upper byte");
        require(srf::original_sprite_diameter(36,1,60)==192,"Original 4WD billboard uses descriptor radius, not X extent");
        require(srf::original_sprite_diameter(36,1,30)==132,"Original Sports tire size");
        require(srf::original_sprite_diameter(36,1,10)==92,"Original Formula tire size");
        require(srf::original_sprite_diameter(36,0,10)==82,"Only the CPU size byte receives descriptor scaling");
        require(srf::road_painter_depth(10420,320,0,true)==19060,"Original road painter priority");
        require(srf::road_painter_depth(10420,-320,3200,true)==22260,"Signed descriptor bias and absolute height");
        require(srf::road_painter_depth(800,100,4864,false)==5864,"F7DC road uses descriptor bias without opcode0C offset");
        const std::array<srf::Vertex,4> road{{{-100,0,100},{-100,0,-100},{100,0,-100},{100,0,100}}};
        const srf::Polygon road_face{{0,1,2,3},{}};
        require(srf::road_support_surface(0x0c,0,road,road_face),"Source-marked road top gets supporting-face priority");
        require(srf::road_support_surface(0x02,4864,road,road_face),"Original F7DC positive descriptor bias is retained");
        require(!srf::road_support_surface(0x02,0,road,road_face),"Ordinary objects retain geometric depth");
        require(!srf::road_support_surface(0x0c,0,road,{{3,2,1,0},{}}),"Road undersides retain geometric depth");
        const std::array<srf::Vertex,4> bank{{{-100,-60,100},{-100,-60,-100},{100,60,-100},{100,60,100}}};
        require(srf::road_support_surface(0x0c,0,bank,road_face),"Banked road remains a supporting face");
        const std::array<srf::Vertex,4> wall{{{0,0,100},{0,0,-100},{0,200,-100},{0,200,100}}};
        require(!srf::road_support_surface(0x0c,0,wall,road_face),"A wall in a priority road object still occludes cars");
        require(!srf::road_support_surface(0x02,4864,wall,road_face),"F7DC wall faces retain geometric depth");

        srf::WorldModel barrier;barrier.frames={{wall.begin(),wall.end()}};
        barrier.primitives={{{0,1,2,3},{0,0,123,0,0,0}}};
        require(srf::missing_barrier_backfaces(barrier,0)==std::vector<bool>{true},"Missing plain roadside back is filled");
        barrier.primitives.push_back({{3,2,1,0},{0,0,105,0,0,0}});
        require(srf::missing_barrier_backfaces(barrier,0)==std::vector<bool>({false,false}),"Original distinct reverse face takes precedence");
        barrier.primitives[0].attributes[2]=179;barrier.primitives.pop_back();
        require(!srf::missing_barrier_backfaces(barrier,0)[0],"Chevron and sign textures remain one-sided");
        barrier.frames={{road.begin(),road.end()}};barrier.primitives[0].attributes[2]=123;
        require(!srf::missing_barrier_backfaces(barrier,0)[0],"Horizontal surfaces are outside the barrier exception");

        srf::WorldModel shadow;shadow.descriptor_address=0xee20;shadow.frames={{road.begin(),road.end()}};
        shadow.primitives={{{0,1,2,3},{211,16,169,0,127,0}}};
        require(srf::coplanar_surface_offsets(shadow,0,{})[0]==srf::SurfaceOffset({0,-.25f,0}),"Original separate-object windmill shadow stays above its road");
        shadow.descriptor_address=0xb7a8;
        require(srf::coplanar_surface_offsets(shadow,0,{})[0]==srf::SurfaceOffset{},"Same material on unrelated props is not a ground shadow");

        srf::WorldModel model;model.frames={{{0,0,0},{0,0,100},{0,100,100},{0,100,0},
            {0,20,20},{0,20,80},{0,80,80},{0,80,20},
            {1,0,0},{1,0,100},{1,100,100},{1,100,0}}};
        // Texture occurs before backing in the source, as on the original truck.
        model.primitives={{{4,5,6,7},{0,0,2,0,0,0}},{{0,1,2,3},{0,0,1,0,0,0}},
                          {{3,2,1,0},{0,0,1,0,0,0}},{{8,9,10,11},{0,0,1,0,0,0}}};
        std::array<bool,256> textures{};textures[2]=true;
        const auto before=model.frames;
        const auto offsets=srf::coplanar_surface_offsets(model,0,textures);
        require(offsets[0][0]==-.25f && offsets[0][1]==0 && offsets[0][2]==0,"Vertical decal must move toward its front");
        require(offsets[1]==srf::SurfaceOffset{} && offsets[2]==srf::SurfaceOffset{},"Backing and opposite side must stay put");
        require(offsets[3]==srf::SurfaceOffset{},"A separate plane must not receive a bias");
        require(model.frames==before,"Presentation must not change source geometry");
        // In D8E8 plain road paint precedes the asphalt. Raising the road
        // produces a dotted line of sky along the adjoining banked section.
        srf::WorldModel marked_road;
        marked_road.frames={{road.begin(),road.end()}};
        marked_road.frames[0].insert(marked_road.frames[0].end(),{{-5,0,50},{-5,0,-50},{5,0,-50},{5,0,50}});
        marked_road.primitives={{{4,5,6,7},{0,0,0x1e,0,127,0}},{{0,1,2,3},{0,0,0x2c,0,127,0}}};
        const auto marking=srf::coplanar_surface_offsets(marked_road,0,{});
        require(marking[0][1]==-.25f && marking[1]==srf::SurfaceOffset{},"Lift paint off the connected road deck");
        // Adjacent coplanar surfaces share an edge, not overlapping area.
        model.frames[0].insert(model.frames[0].end(),{{0,100,0},{0,100,100},{0,200,100},{0,200,0}});
        model.primitives.push_back({{12,13,14,15},{0,0,1,0,0,0}});
        require(srf::coplanar_surface_offsets(model,0,textures).back()==srf::SurfaceOffset{},"Adjacent faces must not be shifted");
        // BFC0's canyon wall is split along a diagonal. Both halves occupy
        // the same bounding rectangle, but their interiors never overlap.
        srf::WorldModel canyon;
        canyon.frames={{{0,0,0},{0,0,100},{0,100,100},{0,100,0}}};
        canyon.primitives={{{0,1,2},{}},{{2,3,0},{}}};
        require(srf::coplanar_surface_offsets(canyon,0,{})==std::vector<srf::SurfaceOffset>(2),
                "Shared diagonal must stay watertight");
        canyon.primitives.push_back({{0,1,3},{}});
        require(srf::coplanar_surface_offsets(canyon,0,{}).back()[0]==-.25f,
                "Intersecting coplanar triangle interiors need separate depth");
        std::cout<<"Wheel selector, sprite size, road priority, angle wrap and coplanar decal checks passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
