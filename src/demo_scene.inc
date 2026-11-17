#include "srf/demo_drive.hpp"
#include "srf/demo_camera.hpp"
#include "demo_dashboard.hpp"
#include "demo_camera_view.inc"

namespace {
class IdleDemoScene {
    const srf::Rom& rom_;
    srf::DemoDrive drive_;
    srf::DemoCamera camera_;
    srf::DemoDashboard dashboard_;
    srf::CourseScenery scenery_;
    srf::World world_;
    std::optional<unsigned> uploaded_phase_;

    void advance_camera() {
        while(camera_.phase()<drive_.display_phase()) {
            const auto phase=camera_.phase()+1;
            scenery_.step(drive_.car_at(phase-1).body);
            camera_.view(phase,drive_.car_at(phase));
        }
    }
public:
    IdleDemoScene(const srf::Rom& rom,Renderer& renderer)
        :rom_(rom),drive_(rom),camera_(rom),dashboard_(rom),
         scenery_(rom,srf::CourseId::easy_ride,true),world_(srf::extract_world(rom)) {
        controls.race=true;
        controls.world=true;
        controls.wire=false;
        renderer.set_course_colors(srf::generated::course_packages.front());
        renderer.load_textures(srf::extract_easy_ride_demo_sprites(rom));
        renderer.load_backdrop(srf::extract_easy_ride_demo_backdrop(rom));
        renderer.set_environment({});
        renderer.load_world(world_,0);
        advance_camera();
    }
    void advance() {
        drive_.advance();
        advance_camera();
    }
    void draw(Renderer& renderer,unsigned tick,unsigned brightness) {
        const auto phase=drive_.display_phase();
        if(uploaded_phase_!=phase) {
            const auto& view=camera_.pose();
            apply_demo_camera(renderer,view);
            const auto yaw=srf::demo_sprite_facing(view);
            const auto& car=drive_.car();
            const auto pose=vehicle_pose(rom_,car,car,true,1,yaw);
            renderer.load_world(world_,0,std::span(&pose,1),&rom_,scenery_.poses(),{},{},{},scenery_.sprites());
            uploaded_phase_=phase;
        }
        renderer.render(dashboard_.draw(tick,brightness,drive_.dashboard_speed()));
    }
};
}
