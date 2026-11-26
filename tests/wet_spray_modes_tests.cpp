// Short offscreen drives for the game modes outside the Speed Trax replay CLI.
#define wmain preview_application_entry
#include "../src/preview.cpp"
#undef wmain

namespace {
using namespace srf;

RenderVehicle pose(const Rom& rom,const Vehicle& car) {
    return vehicle_pose(rom,car,car,true,1,std::uint8_t(car.body.word(0x24)>>8));
}

void aim(Renderer& renderer,const RenderVehicle& vehicle,const Vehicle& car) {
    const auto angle=car.body.word(0x24);
    chase_camera(renderer,vehicle,lerp_angle(angle,angle,1),{},CourseId::easy_ride);
}

void compare(Renderer& renderer,const std::filesystem::path& output,const std::string& name,
             bool split,const auto& draw) {
    const bool wet=renderer.rain_spray_enabled();
    draw(false);
    const auto off=renderer.readback_bgra();
    draw(wet);
    const auto on=renderer.readback_bgra();
    const unsigned views=split?2:1;
    for(unsigned view=0;view<views;++view) {
        unsigned changed=0;
        for(auto p=view*(on.size()/views);p<(view+1)*(on.size()/views);p+=4)
            if(std::abs(int(on[p])-off[p])>4)++changed;
        if(wet && changed<100)throw std::runtime_error(name+" has no visible spray in view "+std::to_string(view));
        // Hardware MSAA can resolve a few coplanar edge samples differently
        // on repeated dry draws. A visible trail covers hundreds of pixels.
        if(!wet && changed>8)throw std::runtime_error(name+" changed "+std::to_string(changed)+" pixels in clear weather");
    }
    if(!output.empty())renderer.screenshot(output/(name+".png"));
    std::cout<<name<<(wet?" rain":" clear")<<" passed\n";
}

void portals(const Rom& rom) {
    // Positions of the 4WD and a full-physics rival at Aqua replay phase 322.
    CourseEnvironment aqua(rom,CourseId::aqua_tunnel);
    VehicleState body{};
    body.word(0x58)=31;body.word(0x1e)=7244;
    if(!spray_exposed(CourseId::aqua_tunnel,aqua,body))throw std::runtime_error("Rival outside Aqua portal has no spray");
    body.word(0x58)=32;body.word(0x1e)=3686;
    if(spray_exposed(CourseId::aqua_tunnel,aqua,body))throw std::runtime_error("Inside Aqua portal emitted spray");
    // Harbor's clean replay drives over the higher deck. Its native water
    // fixture drives below it at the same X/Z (phase 650).
    CourseEnvironment harbor(rom,CourseId::harbor_city);
    body.word(0x1e)=22221;body.word(0x22)=std::uint16_t(-643);body.word(0x20)=std::uint16_t(-1000);
    if(spray_exposed(CourseId::harbor_city,harbor,body))throw std::runtime_error("Submerged Harbor car emitted spray");
    body.word(0x20)=std::uint16_t(-2685);
    if(!spray_exposed(CourseId::harbor_city,harbor,body))throw std::runtime_error("Harbor upper deck suppressed spray");
}

void stunt(Renderer& renderer,const Rom& rom,StuntCourse course,const std::filesystem::path& output) {
    RockFieldSession session(rom,PlayerCar::four_wd,course);
    WetTireSpray spray;
    unsigned moving=0;
    for(unsigned field=0;field<1500 && moving<18;++field) {
        const auto before=session.drive.player();
        if(!session.advance(0x10) || !session.drive.drive_started())continue;
        const auto phase=session.drive.phase();
        spray.begin_phase(phase);
        spray.emit(phase,0,before,session.drive.player());
        ++moving;
    }
    const auto& car=session.drive.player();
    const auto world=extract_world(rom,session.drive.scene());
    renderer.select_course({},course);
    generated::CoursePackage colors{};
    colors.day_palette=colors.dusk_palette=extract_rock_field_palette(rom,course);
    renderer.set_course_colors(colors);
    renderer.load_textures(extract_rock_field_sprites(rom,course));
    renderer.load_backdrop(extract_rock_field_backdrop(rom,course));
    renderer.load_world(world,0);
    const auto vehicle=pose(rom,car);
    aim(renderer,vehicle,car);
    compare(renderer,output,std::string(stunt_course_id(course)),false,[&](bool on){
        renderer.load_stunt_world(world,vehicle,rom,session);
        if(on)renderer.load_spray(spray,float(session.drive.phase()));
        renderer.render();
    });
}

void local(Renderer& renderer,const Rom& rom,LocalCourse course,const std::filesystem::path& output) {
    LocalRace race(rom,course,PlayerCar::four_wd,PlayerCar::coupe,false);
    WetTireSpray spray;
    for(unsigned t=0;t<18;++t) {
        std::array<Vehicle,2> before{};
        for(unsigned i=0;i<race.count();++i)before[i]=race.car(i);
        race.step({0x10,0x10});
        spray.begin_phase(race.phase());
        for(unsigned i=0;i<race.count();++i)spray.emit(race.phase(),i,before[i],race.car(i));
    }
    renderer.select_course({});
    generated::CoursePackage colors{};
    colors.day_palette=colors.dusk_palette=extract_local_palette(rom,course.index);
    renderer.set_course_colors(colors);
    renderer.load_textures(extract_local_sprites(rom,course.index));
    renderer.load_backdrop(extract_local_backdrop(rom,course.index));
    const auto world=extract_world(rom,race.scene());
    std::vector<RenderVehicle> vehicles;
    for(unsigned i=0;i<race.count();++i)vehicles.push_back(pose(rom,race.car(i)));
    renderer.load_world(world,0,vehicles,&rom);
    compare(renderer,output,"local-"+std::to_string(course.index),race.count()==2,[&](bool on){
        const auto camera=[&](unsigned i){
            aim(renderer,vehicles[i],race.car(i));
            if(on)renderer.load_spray(spray,float(race.phase()));
        };
        if(race.count()==2)renderer.render({},camera);
        else {camera(0);renderer.render();}
    });
}

void bonus(Renderer& renderer,const Rom& rom,SpeedClass league,const std::filesystem::path& output) {
    SpeedBonus event(rom,league);
    WetTireSpray spray;
    constexpr unsigned phase=24;
    for(unsigned i=1;i<=phase;++i) {
        const auto before=event.truck();
        event.step(0x10,4);
        spray.begin_phase(i);spray.emit(i,0,before,event.truck());
    }
    const auto world=extract_world(rom,event.scene());
    TrailerLoading loading(rom,event.scene(),PlayerCar::four_wd);
    TrailerLoadingView view(rom,league,PlayerCar::four_wd);
    loading.advance(view.end_tick());
    renderer.select_course({});
    renderer.set_course_colors(generated::course_packages.front());
    renderer.load_textures(extract_vehicle_sprites(rom,CourseId::easy_ride,false,true));
    renderer.load_backdrop(extract_speed_bonus_backdrop(rom));
    renderer.load_world(world,0);
    compare(renderer,output,"bonus-"+std::to_string(unsigned(league)),false,[&](bool on){
        render_bonus_scene(renderer,rom,world,event,event.truck(),loading,view,view.end_tick()+100,2,false,nullptr);
        if(on)renderer.load_spray(spray,phase);
        renderer.render();
    });
}
}

int wmain(int argc,wchar_t** argv) {
    if(!_wgetenv(L"SRF_ROM"))return 77;
    try {
        check(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"Offscreen COM");
        const auto rom=srf::Rom::load(_wgetenv(L"SRF_ROM"));
        const auto output=argc==2?std::filesystem::path(argv[1]):std::filesystem::path{};
        if(!output.empty())std::filesystem::create_directories(output);
        controls.width=output.empty()?960:2560;controls.height=output.empty()?540:1440;
        controls.race=controls.world=race_camera.enabled=true;
        RaceGraphics graphics{};graphics.look=RaceLook::remastered;graphics.wet_tire_spray=true;
        portals(rom);
        for(const bool rain:{false,true}) {
            graphics.msaa=4;
            graphics.weather=rain?RaceWeather{1,.007f,1}:RaceWeather{};
            Renderer renderer(nullptr,nullptr,0,graphics);
            if(renderer.rain_spray_enabled()!=rain)throw std::runtime_error("Spray availability disagrees with weather");
            const auto folder=output.empty()?output:output/(rain?"rain":"clear");
            if(!folder.empty())std::filesystem::create_directories(folder);
            for(auto c:srf::playable_stunt_courses)stunt(renderer,rom,c,folder);
            for(auto c:srf::battle_courses)local(renderer,rom,c,folder);
            local(renderer,rom,srf::practice_course,folder);
            for(auto c:{srf::SpeedClass::novice,srf::SpeedClass::expert,srf::SpeedClass::master})bonus(renderer,rom,c,folder);
        }
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
