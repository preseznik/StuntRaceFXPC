namespace {
// The camera follows the car's heading. Level roll and pitch keep the driver's
// seated horizon stable as the suspension moves under the bonnet.
void bonnet_camera(const Renderer& renderer,const RenderVehicle& vehicle,float yaw) {
    // The 4WD roof ends at Z=40. Mount the eye over the windscreen, clear
    // of the roof, looking along the bonnet towards the nose at Z=160.
    const bool coupe=vehicle.descriptor==n64::coupe_player_shell;
    const bool f_type=vehicle.descriptor==n64::f_type_player_shell;
    const float eye_height=coupe?78.f:f_type?66.f:114.f;
    const float forward=coupe?41.f:f_type?65.f:51.f;
    auto mount=vehicle.position;
    for(unsigned axis=0;axis<3;++axis)
        mount[axis]+=-eye_height*vehicle.orientation[3+axis]+forward*vehicle.orientation[6+axis];
    const auto seat=renderer.normalize_world(mount);
    controls.target_x=seat[0];controls.target_y=seat[1];controls.target_z=seat[2];
    controls.yaw=yaw;controls.pitch=0;controls.distance=0;
}
SceneEye bonnet_eye(const Renderer& renderer,const srf::vr::Eye& tracked) {
    // The original body is 310 units nose to tail and 136 units wide.
    // This gives the compact 4WD a 3.44 m body and keeps the runtime IPD in metres.
    constexpr float units_per_meter=90;
    auto offset=tracked.meters;
    for(auto& axis:offset)axis=renderer.normalized_distance(axis*units_per_meter);
    return {offset,tracked.orientation,tracked.left,tracked.right,tracked.down,tracked.up,tracked.second};
}
void load_bonnet_pose(Renderer& renderer,const srf::World& world,const srf::Rom& rom,
                      const srf::NativeRace& race,const srf::CourseScenery& scenery,const std::array<srf::Vehicle,4>& cars) {
    // Keep the 4WD's exterior eye billboards clear of the bonnet mount.
    const auto poses=vehicle_poses(rom,cars,cars,race.player_index(),1,false);
    const auto heading=cars[race.player_index()].body.word(0x24);
    bonnet_camera(renderer,poses[race.player_index()],lerp_angle(heading,heading,1));
    renderer.load_race_world(world,poses,rom,race,scenery);
}
void run_bonnet_drive(HWND window,srf::vr::Drive& drive,Renderer& renderer,const srf::Rom& rom,
                      srf::NativeRace& race,srf::CourseScenery& scenery,srf::CourseEnvironment& environment,
                      const srf::World& world,const std::array<srf::Vehicle,4>& previous,PresentationRoad& road,
                      const RivalWheelTravel& travel,RaceLook look,bool native_motion,const WetTireSpray* spray,const std::filesystem::path& profile,srf::GridCountdownArt& hud_art,std::span<std::uint32_t> hud_pixels,std::unique_ptr<srf::AudioDevice>& audio,
                      const auto& step,const auto& reset,std::ostream& log) {
    std::unique_ptr<srf::Gamepad> gamepad;
    try{gamepad=std::make_unique<srf::Gamepad>(profile);}catch(const std::exception& e){log<<"Controller: "<<e.what()<<std::endl;}
    std::map<ID3D11Texture2D*,SceneTarget> targets;
    srf::vr::BonnetMotionClock clock;
    auto midpoint=race.racers();
    bool show_midpoint=false;
    float blend=1;
    std::optional<std::pair<uint64_t,float>> uploaded;
    uint64_t scene_updates=0;
    std::string_view last_backdrop;
    bool muted=false,finished=false;
    std::optional<std::tuple<uint64_t,bool,bool>> displayed;
    drive.run([&]{
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT)return false;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        return !finished && !game_closing && !controls.back && IsWindow(window)!=FALSE;
    },[&](int64_t predicted,bool focused){
        const auto pad=gamepad?gamepad->poll(focused):srf::GamepadInput{};
        if(controls.paused && pad.restart){controls.back=true;return srf::vr::Display{};}
        if(pad.pause)controls.paused=!controls.paused;
        if(pad.disconnected)controls.paused=true;
        if(focused && (controls.restart || pad.restart)){
            log<<"Restart at phase "<<race.phase()<<std::endl;
            reset();displayed.reset();clock={};midpoint=race.racers();uploaded.reset();controls.paused=false;finished=false;
        }
        controls.restart=false;
        const bool driving=focused && !controls.paused;
        if(audio && muted==driving){muted=!driving;audio->pause(muted);}
        const auto input=srf::combine_player_input(focused?keyboard_control(window):0,pad);
        show_midpoint=clock.advance(predicted,driving,[&]{
            step(input.requested,input.steering_q15,std::nullopt,&midpoint);
        }) && !race.ended();
        if(driving)blend=race.ended()?1.f:(float(!show_midpoint)+(native_motion?1.f:clock.blend()))*.5f;
        if(!finished && race.completion().finished){
            log<<"Finished three laps at phase "<<race.phase()<<'\n'
               <<"Result: "<<unsigned(race.completion().acknowledged_laps)<<" laps, place "<<race.standings()[race.player_index()]
               <<", "<<race.remaining_seconds()<<" seconds left"<<std::endl;
            finished=true;
        }
        const auto state=std::tuple{race.phase(),race_hud_visible,controls.paused};
        if(displayed!=state) {
            draw_driving_hud(renderer,hud_art,hud_pixels,race,0,race_hud_visible);
            displayed=state;
        }
        srf::vr::Display hud{renderer.headset_hud};
        hud.recenter=focused && (controls.recenter || pad.recenter);controls.recenter=false;
        return hud;
    },[&](ID3D11Texture2D* texture,DXGI_FORMAT format,const srf::vr::Eye& eye){
        const auto pose_id=std::pair{uint64_t(race.phase()),race.phase()?blend:0.f};
        if(uploaded!=pose_id){
            const auto& cars=race.racers();
            const auto presentation=environment.presentation();
            renderer.set_environment(presentation);
            if(race.course_id()==srf::CourseId::aqua_tunnel){
                const std::string_view backdrop=presentation.blank_backdrop?"transition":presentation.alternate_backdrop?"underwater":"surface";
                if(backdrop!=last_backdrop){
                    log<<"Aqua backdrop "<<backdrop<<" at phase "<<race.phase()<<std::endl;
                    last_backdrop=backdrop;
                }
            }
            const auto surface=road.sample(race.bridge_poses());
            auto poses=vehicle_poses(rom,previous,cars,race.player_index(),blend,false,
                look==RaceLook::remastered?&travel:nullptr,surface,look==RaceLook::n64,std::nullopt,&midpoint,native_motion);
            if(look==RaceLook::remastered)seat_opponents(poses,surface);
            const auto& player=poses[race.player_index()];
            bonnet_camera(renderer,player,player.heading);
            renderer.load_race_world(world,poses,rom,race,scenery,blend-1.f,spray);
            uploaded=pose_id;++scene_updates;
        }
        auto [it,inserted]=targets.try_emplace(texture);
        if(inserted){D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);it->second=renderer.make_target(d.Width,d.Height,texture,format);}
        renderer.render_eye(it->second,bonnet_eye(renderer,eye));
    });
    log<<"Native scene updates "<<scene_updates<<(native_motion?" (native callback poses)":" (vehicle poses interpolated at predicted display time)")<<std::endl;
}
}
