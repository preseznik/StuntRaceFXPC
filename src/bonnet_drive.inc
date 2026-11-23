namespace {
// The camera follows the car's heading. Level roll and pitch keep the driver's
// seated horizon stable as the suspension moves under the bonnet.
void bonnet_camera(const Renderer& renderer,const RenderVehicle& vehicle,float yaw) {
    // The 4WD roof ends at Z=40. Mount the eye over the windscreen, clear
    // of the roof, looking along the bonnet towards the nose at Z=160.
    constexpr float eye_height=114,forward=51;
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
    return {offset,tracked.orientation,tracked.left,tracked.right,tracked.down,tracked.up};
}
void load_bonnet_pose(Renderer& renderer,const srf::World& world,const srf::Rom& rom,
                      const srf::NativeRace& race,const srf::CourseScenery& scenery,const std::array<srf::Vehicle,4>& cars) {
    // Keep the 4WD's exterior eye billboards clear of the bonnet mount.
    const auto poses=vehicle_poses(rom,cars,cars,race.player_index(),1,false);
    const auto heading=cars[race.player_index()].body.word(0x24);
    bonnet_camera(renderer,poses[race.player_index()],lerp_angle(heading,heading,1));
    renderer.load_race_world(world,poses,rom,race,scenery);
}
srf::vr::Display bonnet_instruments(const srf::NativeRace& race,bool paused) {
    const auto& body=race.racers()[race.player_index()].body;
    std::wostringstream top,bottom;
    top<<L"Lap "<<std::min(3u,unsigned(body.word(0x7a))+1)<<L"/3     Pos "<<(race.standings()[race.player_index()]?std::to_wstring(race.standings()[race.player_index()]):L"–")
       <<L"/4     "<<race.remaining_seconds()<<L" s     Boost "<<std::min(100u,unsigned(body.word(0x6a))*100/384)<<L"%";
    if(race.completion().finished)bottom<<L"Finished";
    else if(race.failure()==srf::RaceFailure::wrecked)bottom<<L"Wrecked";
    else if(race.time_over_pending() || race.failure()==srf::RaceFailure::time_out)bottom<<L"Time up";
    else if(race.failure()!=srf::RaceFailure::none)bottom<<L"Out of course";
    else if(paused)bottom<<L"Paused    •    P / Start to resume";
    else if(race.countdown_active())bottom<<L"Grid    •    F2 / right stick to recenter";
    else bottom<<std::abs(signed_word(body.word(0x4a)))*15/64<<L" mph     Damage "
               <<std::min(100u,unsigned(body.word(0x82))*100/512)<<L"%";
    if(race.completion().finished || race.failure()!=srf::RaceFailure::none)bottom<<L"    •    R / View to restart";
    return {top.str(),bottom.str()};
}
void run_bonnet_drive(HWND window,srf::vr::Drive& drive,Renderer& renderer,const srf::Rom& rom,
                      srf::NativeRace& race,srf::CourseScenery& scenery,srf::CourseEnvironment& environment,
                      const srf::World& world,const std::filesystem::path& profile,std::unique_ptr<srf::AudioDevice>& audio,
                      const auto& step,const auto& reset,std::ostream& log) {
    std::unique_ptr<srf::Gamepad> gamepad;
    try{gamepad=std::make_unique<srf::Gamepad>(profile);}catch(const std::exception& e){log<<"Controller: "<<e.what()<<std::endl;}
    std::map<ID3D11Texture2D*,SceneTarget> targets;
    srf::vr::BonnetMotionClock clock;
    auto midpoint=race.racers();
    bool show_midpoint=false;
    uint64_t uploaded=UINT64_MAX;
    uint64_t scene_updates=0;
    std::string_view last_backdrop;
    bool muted=false,finished=false;
    drive.run([&]{
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT)return false;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        return IsWindow(window)!=FALSE;
    },[&](int64_t predicted,bool focused){
        const auto pad=gamepad?gamepad->poll(focused):srf::GamepadInput{};
        if(pad.pause)controls.paused=!controls.paused;
        if(pad.disconnected)controls.paused=true;
        if(focused && (controls.restart || pad.restart)){
            log<<"Restart at phase "<<race.phase()<<std::endl;
            reset();clock={};midpoint=race.racers();uploaded=UINT64_MAX;controls.paused=false;finished=false;
        }
        controls.restart=false;
        const bool driving=focused && !controls.paused;
        if(audio && muted==driving){muted=!driving;audio->pause(muted);}
        const auto input=srf::combine_player_input(focused?keyboard_control(window):0,pad);
        show_midpoint=clock.advance(predicted,driving,[&]{
            step(input.requested,input.steering_q15,std::nullopt,&midpoint);
        }) && !race.ended();
        if(!finished && race.completion().finished){
            log<<"Finished three laps at phase "<<race.phase()<<'\n'
               <<"Result: "<<unsigned(race.completion().acknowledged_laps)<<" laps, place "<<race.standings()[race.player_index()]
               <<", "<<race.remaining_seconds()<<" seconds left"<<std::endl;
            finished=true;
        }
        auto hud=bonnet_instruments(race,controls.paused);
        hud.recenter=focused && (controls.recenter || pad.recenter);controls.recenter=false;
        return hud;
    },[&](ID3D11Texture2D* texture,DXGI_FORMAT format,const srf::vr::Eye& eye){
        const uint64_t pose_id=2*race.phase()-unsigned(show_midpoint);
        if(uploaded!=pose_id){
            const auto& cars=show_midpoint?midpoint:race.racers();
            const auto presentation=environment.presentation();
            renderer.set_environment(presentation);
            if(race.course_id()==srf::CourseId::aqua_tunnel){
                const std::string_view backdrop=presentation.blank_backdrop?"transition":presentation.alternate_backdrop?"underwater":"surface";
                if(backdrop!=last_backdrop){
                    log<<"Aqua backdrop "<<backdrop<<" at phase "<<race.phase()<<std::endl;
                    last_backdrop=backdrop;
                }
            }
            load_bonnet_pose(renderer,world,rom,race,scenery,cars);
            uploaded=pose_id;++scene_updates;
        }
        auto [it,inserted]=targets.try_emplace(texture);
        if(inserted){D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);it->second=renderer.make_target(d.Width,d.Height,texture,format);}
        renderer.render_eye(it->second,bonnet_eye(renderer,eye));
    });
    log<<"Native scene updates "<<scene_updates<<" (paired vehicle callbacks, up to 28 poses/s)"<<std::endl;
}
}
