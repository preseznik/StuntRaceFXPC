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
    srf::vr::RaceClock clock;
    uint64_t uploaded=UINT64_MAX;
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
            reset();clock={};uploaded=UINT64_MAX;controls.paused=false;finished=false;
        }
        controls.restart=false;
        const bool driving=focused && !controls.paused;
        if(audio && muted==driving){muted=!driving;audio->pause(muted);}
        const auto input=srf::combine_player_input(focused?keyboard_control(window):0,pad);
        const auto phases=clock.advance(predicted,driving);
        for(unsigned i=0;i<phases;++i)step(input.requested,input.steering_q15);
        if(!finished && race.completion().finished){log<<"Finished three laps at phase "<<race.phase()<<std::endl;finished=true;}
        auto hud=bonnet_instruments(race,controls.paused);
        hud.recenter=focused && (controls.recenter || pad.recenter);controls.recenter=false;
        return hud;
    },[&](ID3D11Texture2D* texture,DXGI_FORMAT format,const srf::vr::Eye& eye){
        if(uploaded!=race.phase()){
            const auto& cars=race.racers();
            // The 4WD's exterior cartoon eyes are flat billboards beside this mount.
            const auto poses=vehicle_poses(rom,cars,cars,race.player_index(),1,false);
            const auto heading=cars[race.player_index()].body.word(0x24);
            bonnet_camera(renderer,poses[race.player_index()],lerp_angle(heading,heading,1));
            renderer.set_environment(environment.presentation());renderer.load_race_world(world,poses,rom,race,scenery);
            uploaded=race.phase();
        }
        auto [it,inserted]=targets.try_emplace(texture);
        if(inserted){D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);it->second=renderer.make_target(d.Width,d.Height,texture,format);}
        renderer.render_eye(it->second,bonnet_eye(renderer,eye));
    });
}
}
