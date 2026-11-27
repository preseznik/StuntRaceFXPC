#include "srf/local_race.hpp"

namespace {
std::uint16_t local_keyboard(const BootWindowInput& keys,bool second) {
    const auto held=[&](unsigned p1,unsigned p2,unsigned alias){
        return second?keys.key(p2):keys.key(p1) || keys.key(alias);
    };
    std::uint16_t buttons=0;
    if(held('W','I',VK_UP))buttons|=0x10;
    if(held('S','K',VK_DOWN))buttons|=0x40;
    if(held('A','J',VK_LEFT))buttons|=1;
    if(held('D','L',VK_RIGHT))buttons|=2;
    if(held(VK_SPACE,'U',VK_SPACE))buttons|=0x400;
    if(held(VK_SHIFT,'Y',VK_SHIFT))buttons|=srf::drift_button;
    if(held('X','O','X'))buttons|=0x20;
    return buttons;
}

void play_local_race(GameHost& host,const srf::Rom& rom,srf::LocalCourse course,srf::PlayerCar first,
                     srf::PlayerCar second,bool cpu,const std::filesystem::path& controller,
                     srf::DrivingPhysics physics=srf::DrivingPhysics::original,bool direct=false) {
    using namespace srf;
    LocalRace race(rom,course,first,second,cpu,{},physics);
    const bool battle=race.count()==2;
    const auto world=extract_world(rom,race.scene());
    auto& renderer=*host.presenter;
    renderer.select_course(std::nullopt);renderer.clear_bonus_overlay();renderer.grid_sweep=false;
    generated::CoursePackage colors{};
    colors.day_palette=colors.dusk_palette=extract_local_palette(rom,course.index);
    renderer.set_course_colors(colors);renderer.load_textures(extract_local_sprites(rom,course.index));
    renderer.load_backdrop(extract_local_backdrop(rom,course.index));renderer.set_environment({});
    controls.race=controls.world=true;renderer.load_world(world,0);
    BootWindowInput keys;
    const std::wstring title=L"Stunt Race FX — "+std::wstring(course.name)+L" — ";
    GameScreen screen(host,(title+L"Grid").c_str(),stunt_window_proc,&keys);
    std::array<std::unique_ptr<Gamepad>,2> pads;
    for(unsigned i=0;i<(battle && !cpu?2u:1u);++i){
        try{pads[i]=std::make_unique<Gamepad>(controller,battle && !cpu?std::optional(i):std::nullopt);}
        catch(const std::exception& e){std::cerr<<"Local race controller: "<<e.what()<<'\n';}
    }
    std::array dashboards{BonusDashboard(rom,first),BonusDashboard(rom,second)};
    BonusHudBoost boost(rom,0);
    RaceHudText notices(rom);
    CourseHudMap map(rom,course.index);
    const auto signal=extract_start_light_tiles(rom),clock_tiles=extract_bonus_hud_tiles(rom);
    const auto palette=bonus_hud_palette(rom,0xf8960);
    std::vector<std::uint32_t> overlay(208*128);
    std::unique_ptr<AudioDevice> audio;
    try{audio=std::make_unique<AudioDevice>(extract_local_audio(rom,course.index));audio->play_music();}
    catch(const std::exception& e){std::cerr<<"Local race audio: "<<e.what()<<'\n';}
    const auto tune=extract_race_audio_tuning(rom.bytes(),race.car(0).body.word(0x66));
    RaceAudioState motor{};
    using Clock=std::chrono::steady_clock;
    auto last=Clock::now();
    double remainder=0;
    unsigned countdown=42;
    bool paused=false,was_paused=false,last_r=false,last_p=false,last_h=false;
    std::wstring caption;
    std::optional<DriftTrails> drift_trails;
    if(physics==DrivingPhysics::enhanced)drift_trails.emplace(world,CourseId::easy_ride);
    WetTireSpray spray;
    const bool wet=renderer.rain_spray_enabled();
    std::array<Vehicle,2> previous{};
    std::array<DriftInput,2> drifts{};
    const auto reset=[&]{
        spray.begin_phase(0);
        if(drift_trails)drift_trails->reset();
        race.reset();countdown=42;remainder=0;paused=false;motor={};
        for(unsigned i=0;i<race.count();++i)previous[i]=race.car(i);
        keys.suppress_held();last=Clock::now();
        if(audio){audio->restart();audio->play_music();}
    };
    reset();
    while(!game_closing){
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT){game_closing=true;return;}
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(game_closing || keys.quit || keys.escape)return;
        const bool focused=game_focused(host.window);
        std::array<GamepadInput,2> input{};
        for(unsigned i=0;i<race.count();++i)
            input[i]=combine_player_input(local_keyboard(keys,i!=0),pads[i]?pads[i]->poll(focused,!i && race_camera.look):GamepadInput{});
        if(paused && (input[0].restart || input[1].restart))return;
        if((keys.key('R') && !last_r) || input[0].restart || input[1].restart)reset();
        if((keys.key('P') && !last_p) || input[0].pause || input[1].pause)paused=!paused;
        if(input[0].disconnected || input[1].disconnected)paused=true;
        if(keys.key('H') && !last_h)race_hud_visible=!race_hud_visible;
        last_h=keys.key('H');last_r=keys.key('R');last_p=keys.key('P');
        const auto now=Clock::now();
        const auto elapsed=std::min(.20,std::chrono::duration<double>(now-last).count());last=now;
        const bool hold=!focused || paused;
        for(unsigned i=0;i<race.count();++i)drifts[i].sample(input[i].requested,!hold && !countdown);
        if(audio && was_paused!=hold)audio->pause(hold);
        was_paused=hold;
        if(!hold){
            remainder+=elapsed*14;
            if(race_camera.look)race_camera.rotate(float(input[0].look_x*elapsed*2.3),float(input[0].look_y*elapsed*1.7));
            if(input[0].recenter || controls.recenter){race_camera.center();if(!game_xr)controls.recenter=false;}
        }
        while(remainder>=1 && !race.result()){
            --remainder;
            if(countdown){
                for(unsigned i=0;i<race.count();++i)race.note_input(i,input[i].requested);
                --countdown;continue;
            }
            for(unsigned i=0;i<race.count();++i)previous[i]=race.car(i);
            race.step({drifts[0].consume(input[0].requested),drifts[1].consume(input[1].requested)},
                      {input[0].steering_q15,input[1].steering_q15});
            if(drift_trails) {
                drift_trails->begin_phase(race.phase());
                for(unsigned i=0;i<race.count();++i)drift_trails->emit(race.phase(),i,previous[i],race.car(i));
            }
            if(wet) {
                spray.begin_phase(race.phase());
                for(unsigned i=0;i<race.count();++i)spray.emit(race.phase(),i,previous[i],race.car(i));
            }
            if(audio){
                const bool throttle=(input[0].requested&0x10)!=0;
                const auto cues=update_race_audio(motor,tune,race.car(0),{false,throttle});
                audio->update(cues,throttle,(input[0].requested&0x400)!=0,true,true);
            }
        }
        if(race.result()){
            if(audio)audio->pause(true);
            const auto winner=*race.result();
            const auto text=!battle?(winner==0?L"Three laps complete":L"Practice ended"):winner==2?L"Draw":winner==0?L"Player 1 wins":cpu?L"CPU wins":L"Player 2 wins";
            if(!game_result(host,{L"Results",std::wstring(course.name),text,L"Retry",direct?L"Exit":L"Course selection"}))return;
            reset();continue;
        }
        if(!focused && !game_xr){Sleep(12);continue;}
        const auto state=paused?L"Paused":countdown?L"Grid":L"Driving";
        if(caption!=state){caption=state;SetWindowTextW(host.window,(title+caption).c_str());}
        std::array<RenderVehicle,2> poses;
        std::array<float,2> yaw{};
        RECT rect{};GetClientRect(host.window,&rect);
        if(!rect.right || !rect.bottom){Sleep(12);continue;}
        const float blend=paused?1.f:float(remainder);
        for(unsigned i=0;i<race.count();++i){
            const auto& body=race.car(i).body;
            yaw[i]=lerp_angle(previous[i].body.word(0x24),body.word(0x24),blend);
            const auto facing=std::uint8_t(race.car(i).body.word(0x24)>>8);
            poses[i]=vehicle_pose(rom,previous[i],race.car(i),!(i && cpu),blend,facing);
        }
        const auto ranks=race.standings();
        const auto camera=[&](unsigned i){
            chase_camera(renderer,poses[i],yaw[i],{},CourseId::easy_ride);
            if(!race_hud_visible && !countdown && !paused){renderer.clear_bonus_overlay();return;}
            std::fill(overlay.begin(),overlay.end(),0);
            std::span<const std::uint32_t> panel;
            if(race_hud_visible){
                const auto& body=race.car(i).body;
                notices.draw(overlay,"LAP "+std::to_string(1+body.word(0x7a))+"/3",48);
                map.draw_route(overlay,5);
                for(unsigned j=0;j<race.count();++j)map.draw_marker(overlay,race.car(j),j==i?1:6);
                boost.draw_at(overlay,0,body);
                draw_race_stopwatch(overlay,148,race.phase()*100/14,clock_tiles,palette);
                panel=dashboards[i].draw(static_cast<std::int16_t>(body.word(0x4a)),body.word(0x82),0,ranks[i]);
            }
            if(paused)notices.draw(overlay,"PAUSED",60);
            if(countdown)draw_start_signal(overlay,72,1+(42-countdown)/14,signal,palette);
            renderer.set_bonus_overlay(overlay,15,1,40,panel,0,224,countdown!=0);
        };
        camera(0);
        renderer.load_world(world,0,std::span(poses).first(race.count()),&rom);
        const float phase=float(race.phase())+blend-1;
        if(battle)renderer.render({},[&](unsigned i){
            camera(i);
            if(wet)renderer.load_spray(spray,phase);
            if(drift_trails)renderer.load_drift(*drift_trails,phase);
        });
        else {
            if(wet)renderer.load_spray(spray,phase);
            if(drift_trails)renderer.load_drift(*drift_trails,phase);
            renderer.render();
        }
    }
}

void local_course_board(GameHost& host,const srf::Rom& rom,bool battle,srf::PlayerCar first,const std::filesystem::path& controller){
    unsigned course=0,opponent=0;
    constexpr std::array names{L"Coupe",L"4WD",L"F-Type"};
    const auto selection=[&](unsigned edge,srf::GameResultPage& page){
        if(battle){
            if(edge&0x0100)course=(course+1)%srf::battle_courses.size();
            if(edge&0x0200)course=(course+srf::battle_courses.size()-1)%srf::battle_courses.size();
            if(edge&0x0400)opponent=(opponent+1)%6;
            if(edge&0x0800)opponent=(opponent+5)%6;
            page.subtitle=srf::battle_courses[course].name;
            page.details=L"Left / Right    Course\r\nUp / Down    Opponent\r\n\r\n";
            page.details+=opponent<3?L"CPU  /  ":L"Player 2  /  ";page.details+=names[opponent%3];
            page.details+=L"\r\n\r\nP2: second gamepad or I/J/K/L. U boosts, O reverses.";
        }else{
            page.subtitle=L"Practice course";
            page.details=L"Drive three laps of the practice course in the 4WD.\r\n\r\nR restarts the run. Esc returns here.";
        }
    };
    while(!game_closing && game_result(host,{battle?L"Battle Trax":L"Test Run",L"",L"",L"Drive",L"Back"},selection))
        play_local_race(host,rom,battle?srf::battle_courses[course]:srf::practice_course,first,
                        static_cast<srf::PlayerCar>(opponent%3),opponent<3,controller);
}
}
