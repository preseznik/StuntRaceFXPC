#pragma once
#include "srf/world.hpp"
#include "srf/race.hpp"

namespace srf {
inline constexpr std::uint16_t kestrel_shell=0x7000, mesa_land=0x7001, mesa_road=0x7002, mesa_wheel=0x7003, cargo_shell=0x7004, city_land=0x7005, city_road=0x7006;
constexpr bool authored_model(unsigned id) { return id>=kestrel_shell && id<=city_road; }
struct VehicleState;
void resolve_kasumi_wall(VehicleState&,RacePosition previous,const std::array<std::uint16_t,9>& orientation);
struct VehicleTuning;
void tune_kestrel(VehicleTuning&);
void tune_cargo(VehicleTuning&);
Scene kasumi_scene();
std::span<const Vertex> kasumi_lamps();
Scene mesa_scene();
std::vector<CollisionShape> custom_collision(const Scene&);
void add_custom_course_walls(std::vector<CourseWall>&,const Scene&);
struct MesaSurface { int y; RacePosition normal; bool dirt; };
std::optional<MesaSurface> custom_surface(unsigned descriptor,unsigned section,RacePosition wheel);
void add_custom_models(World&, CourseId);
}
