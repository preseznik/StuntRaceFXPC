#include "srf/demo_camera.hpp"

void apply_demo_camera(const Renderer& renderer,const srf::DemoCameraPose& camera) {
    const auto target=renderer.normalize_world({float(camera.eye[0]),float(camera.eye[1]),float(camera.eye[2])});
    constexpr float radians=6.283185307179586f/65536.f;
    controls.target_x=target[0];controls.target_y=target[1];controls.target_z=target[2];
    controls.distance=0;
    controls.yaw=-camera.yaw*radians;
    controls.pitch=camera.pitch*radians;
    controls.zoom=2.f;
}
