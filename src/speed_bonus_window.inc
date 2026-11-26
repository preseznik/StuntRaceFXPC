#include "srf/speed_bonus.hpp"
#include "srf/course_map.hpp"
#include "srf/trailer_loading.hpp"
#include <chrono>
#include "trailer_loading_view.inc"

namespace {
struct BonusWindowState {
    bool quit{}, retire{}, paused{};
};
LRESULT CALLBACK bonus_window_proc(HWND window,UINT message,WPARAM key,LPARAM data) {
    auto* state=reinterpret_cast<BonusWindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        state=static_cast<BonusWindowState*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
    }
    if(state) {
        if(message==WM_CLOSE){state->quit=true;return 0;}
        if(message==WM_KEYDOWN && !(data&(1L<<30))) {
            if(key==VK_ESCAPE)state->retire=true;
            if(key=='P')state->paused=!state->paused;
            if(key=='H')race_hud_visible=!race_hud_visible;
        }
    }
    return DefWindowProcW(window,message,key,data);
}
RenderVehicle bonus_cab(const srf::Rom& rom,const srf::Vehicle& truck,const RenderVehicle& trailer,std::uint8_t camera_yaw) {
    const auto& b=truck.body;
    RenderVehicle cab;
    // CPU08:DF4C attaches the cab to the cargo body, then steers its yaw independently.
    const auto entry=srf::Rom::lorom_offset(0x098669)+0x6f;
    cab.descriptor=rom.u16(0x48000+rom.u16(entry));
    const auto offset=static_cast<std::int8_t>(rom.bytes()[entry+4])*8;
    for(unsigned axis=0;axis<3;++axis)cab.position[axis]=trailer.position[axis]+offset*trailer.orientation[6+axis];
    srf::SceneryPose angles{};
    angles.yaw=static_cast<std::uint8_t>((b.word(0x24)>>8)+(b.word(0x3a)>>8));
    angles.pitch=static_cast<std::uint8_t>(b.word(0x26)>>8);
    angles.roll=static_cast<std::uint8_t>(b.word(0x28)>>8);
    for(unsigned row=0;row<3;++row) {
        const auto axis=srf::scenery_position({int(row==0),int(row==1),int(row==2)},angles);
        for(unsigned col=0;col<3;++col)cab.orientation[row*3+col]=float(axis[col]);
    }
    // The four E073 cab wheels attach to the steered cab, independently of the trailer wheels.
    const auto relative=static_cast<std::uint8_t>(angles.yaw-camera_yaw);
    const auto wheel=rom.u16(srf::Rom::lorom_offset(0x08de43));
    const auto size=srf::original_sprite_diameter(rom.u16((wheel&0x7fff)+16),rom.bytes()[(wheel&0x7fff)+7],0x6e);
    for(const auto p:{0x08de70u,0x08deb1u,0x08def6u,0x08df39u}) {
        const auto field=srf::Rom::lorom_offset(p);
        const srf::Vertex local{8*static_cast<std::int16_t>(rom.u16(field)),8*static_cast<std::int16_t>(rom.u16(field+6)),
                                8*static_cast<std::int16_t>(rom.u16(field+12))};
        const auto position=srf::scenery_position(local,angles);
        RenderSprite sprite;
        sprite.table=rom.u16(srf::Rom::lorom_offset(0x08e06c))+8+srf::wheel_sprite_bank(angles.roll,relative);
        sprite.phase=rom.bytes()[0x44056+(relative>>2)];
        sprite.diameter=size;
        for(unsigned i=0;i<3;++i)sprite.position[i]=cab.position[i]+float(position[i]);
        cab.sprites.push_back(sprite);
    }
    return cab;
}

// The stationary truck has already settled onto the grid in the original shot.
// Carry its displayed height through the first ground contact of the playable
// truck. The driving body, wheel history and clock still start on their usual grid.
srf::Vehicle loading_truck_pose(const srf::Vehicle& truck,int grounded_y,bool settling) {
    auto displayed=truck;
    if(settling) {
        const auto lift=grounded_y-signed_word(truck.body.word(0x20));
        displayed.body.word(0x20)+=lift;
        for(auto& wheel:displayed.wheels)wheel.word(0x20)+=lift;
    }
    return displayed;
}

void render_bonus_scene(Renderer& renderer,const srf::Rom& rom,const srf::World& world,
                        const srf::SpeedBonus& event,const srf::Vehicle& truck,
                        const srf::TrailerLoading& loading,TrailerLoadingView& view,unsigned tick,unsigned spares,bool show_hud,
                        const srf::BonusTally* tally,bool paused=false) {
    const auto camera_yaw=view.camera(renderer,truck,tick,loading.phase());
    auto trailer=vehicle_pose(rom,truck,truck,true,1,camera_yaw);
    trailer.descriptor=loading.cargo_descriptor();
    std::array<RenderVehicle,3> parts;
    parts[0]=std::move(trailer);
    parts[1]=bonus_cab(rom,truck,parts[0],camera_yaw);
    parts[0].source_model=parts[1].source_model=true;
    unsigned count=2;
    if(loading.car_visible()) {
        if(const auto distant=loading.distant_car(view.eye_x,view.eye_z,camera_yaw)) {
            parts[0].sprites.push_back({distant->table,distant->frame,{float(distant->position.x),float(distant->position.y),
                float(distant->position.z)},float(distant->diameter)});
        } else parts[count++]=vehicle_pose(rom,loading.car(),loading.car(),true,1,camera_yaw);
    }
    const auto& ramp=loading.ramp();
    renderer.load_world(world,0,std::span(parts).first(count),&rom,event.gates(),std::span(&ramp,1));
    view.overlay(renderer,tick,event,spares,show_hud,tally,paused);
}
}

namespace srf {
struct BonusFinish {
    BonusReward reward;
    CourseMapEntry entry;
    std::unique_ptr<AudioDevice> audio;
};
static std::optional<BonusFinish> play_speed_bonus(GameHost& host,const Rom& rom,const Championship& championship,
    const std::filesystem::path& controller_config) {
    if(championship.outcome()!=ChampionshipOutcome::bonus)throw std::logic_error("No pending Bonus Trax event");
    const auto league=championship.league();
    const auto car=championship.car();
    const unsigned spares=championship.credits();
    const unsigned incoming=championship.results().back().remaining_seconds;
    SpeedBonus event(rom,league);
    TrailerLoading loading(rom,event.scene(),car);
    TrailerLoadingView view(rom,league,car);
    const auto world=extract_world(rom,event.scene());
    const int grounded_y=signed_word(event.truck().body.word(0x20))+98;
    bool settling=true;
    WetTireSpray spray;
    unsigned phase=0;
    BonusWindowState state;

    controls.world=true;controls.race=true;
    GameScreen scope(host,L"Stunt Race FX — Bonus Trax",bonus_window_proc,&state);
    RECT rect{};
    auto& renderer=*host.presenter;
    renderer.select_course(std::nullopt);renderer.use_classic_frame(rom);
    renderer.set_course_colors(generated::course_packages.front());
    renderer.load_textures(extract_vehicle_sprites(rom,CourseId::easy_ride,false,true));
    renderer.load_backdrop(extract_speed_bonus_backdrop(rom));
    renderer.set_environment({});
    renderer.load_world(world,0);
    std::unique_ptr<Gamepad> pad;
    std::unique_ptr<AudioDevice> audio;
    try {pad=std::make_unique<Gamepad>(controller_config);}catch(const std::exception& e){std::cerr<<"Bonus controller: "<<e.what()<<'\n';}
    try {audio=std::make_unique<AudioDevice>(extract_speed_bonus_audio(rom));}catch(const std::exception& e){std::cerr<<"Bonus audio: "<<e.what()<<'\n';}
    const auto sound_tuning=extract_race_audio_tuning(rom.bytes(),0x8669);
    RaceAudioState sound_state{};
    std::uint8_t engine_parameter=0;
    auto loading_music=extract_trailer_loading_music(rom);
    auto tally_music=extract_bonus_tally_music(rom);
    auto tally_outro=extract_bonus_tally_outro(rom);
    ShowWindow(scope.window,SW_SHOW);UpdateWindow(scope.window);
    using Steady=std::chrono::steady_clock;
    auto last=Steady::now();
    constexpr double video_period=1./60.0988;
    double accumulator=0,clock_ticks=0,video_remainder=0;
    unsigned video_tick=0;
    std::optional<BonusTally> tally;
    bool audio_paused=false;
    const auto pause_audio=[&](bool paused) {
        if(audio)try {audio->pause(paused);}catch(const std::exception& e){std::cerr<<"Bonus audio: "<<e.what()<<'\n';audio.reset();}
    };
    const auto play_tally_audio=[&](RaceAudioOutput cues) {
        if(audio)try {
            if(tally->frame().start_music)audio->play_music(std::move(tally_music),false,std::move(tally_outro));
            if(!cues.effects.empty() || !cues.commands.empty()) {
                cues.engine_parameter=engine_parameter;
                audio->update(cues,false,false,true,true);
            }
        }catch(const std::exception& e){std::cerr<<"Bonus tally audio: "<<e.what()<<'\n';audio.reset();}
    };
    while(!game_closing && !state.quit) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT){state.quit=true;break;}
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(state.quit)break;
        const bool focused=game_focused(scope.window);
        auto input=combine_player_input(keyboard_control(scope.window),pad?pad->poll(focused):GamepadInput{});
        if(input.pause) {
            if(state.paused)state.paused=false;
            else if(!tally)state.retire=true;
        }
        if(input.disconnected)state.paused=true;
        if(state.retire) {
            if(tally)return std::nullopt;
            pause_audio(true);

            const bool resume=game_result(host,{L"RETIRE?",L"Bonus Trax",
                L"Retiring forfeits flag time. Spare cars earned for completed laps carry forward.",L"Resume driving",L"Retire"});
            if(!resume)return BonusFinish{event.collected_reward(),CourseMapEntry::retire,{}};
            ShowWindow(scope.window,SW_SHOW);

            pause_audio(audio_paused);
            state.retire=false;last=Steady::now();
        }
        const auto now=Steady::now();
        const double elapsed=focused && !state.paused?std::min(.16,std::chrono::duration<double>(now-last).count()):0;
        last=now;
        if(audio && audio_paused!=(!focused || state.paused)) {
            audio_paused=!focused || state.paused;
            pause_audio(audio_paused);
        }
        const auto previous_tick=video_tick;
        video_remainder+=elapsed;
        while(video_remainder>=video_period) {
            video_remainder-=video_period;
            ++video_tick;
            if(video_tick<=view.end_tick())loading.advance();
            if(!tally)view.advance(video_tick,(input.requested&0x400)!=0);
            else play_tally_audio(tally->advance());
        }
        if(previous_tick<view.end_tick()) {
            RaceAudioOutput cues;
            view.append_audio(previous_tick,video_tick,cues);
            if(audio)try {
                if(previous_tick<5 && video_tick>=5) {
                    audio->play_music(std::move(loading_music),true);
                    audio->update({},false,false,true,true);
                }
                if(previous_tick<view.music_tick() && video_tick>=view.music_tick())audio->play_music();
                if(!cues.effects.empty())
                    audio->update(cues,false,false,true,true);
                if(!audio->error().empty())throw std::runtime_error(audio->error());
            }catch(const std::exception& e){std::cerr<<"Bonus audio: "<<e.what()<<'\n';audio.reset();}
        }
        if(previous_tick>=view.drive_tick())accumulator+=elapsed;
        else if(video_tick>=view.drive_tick())
            accumulator+=1./14+(video_tick-view.drive_tick())*video_period+video_remainder;
        while(accumulator>=1./14) {
            const auto before=event.reward();
            std::optional<Vehicle> previous_truck;
            if(renderer.rain_spray_enabled())previous_truck=event.truck();
            if(tally) {
                view.latch_speed(event);
                event.coast_after_timeout();
            } else {
                clock_ticks+=60.0988/14.;
                const auto ticks=static_cast<std::uint8_t>(clock_ticks);clock_ticks-=ticks;
                view.latch_instruments(event);
                event.step(input.requested,ticks,input.steering_q15);
                if(event.ended()) {
                    const auto earned=event.collected_reward();
                    const auto start=event.stayed_parked()?BonusTallyStart::parked:BonusTallyStart::moving;
                    tally.emplace(league,earned.extra_seconds,earned.extra_cars,incoming,video_tick,start,car);
                    play_tally_audio(tally->opening_audio());
                }
            }
            ++phase;
            if(previous_truck) {
                spray.begin_phase(phase);
                spray.emit(phase,0,*previous_truck,event.truck());
            }
            if(signed_word(event.truck().body.word(0x20))>=grounded_y)settling=false;
            if(audio)try {
                const bool throttle=!tally && (input.requested&0x10)!=0;
                auto output=update_race_audio(sound_state,sound_tuning,event.truck(),{false,throttle});
                engine_parameter=output.engine_parameter;
                if(event.reward().extra_seconds!=before.extra_seconds)output.effects.push_back(0x5c);
                if(event.reward().extra_cars!=before.extra_cars)output.effects.push_back(0x35);
                audio->update(output,throttle,!tally && (event.truck().body.word(0x42)&0x400)!=0,true,true);
                if(!audio->error().empty())audio.reset();
            }catch(const std::exception&){audio.reset();}
            accumulator-=1./14;
        }
        if(!game_xr && IsIconic(scope.window)){Sleep(30);continue;}
        GetClientRect(scope.window,&rect);
        if(rect.right>0 && rect.bottom>0 && (controls.width!=unsigned(rect.right) || controls.height!=unsigned(rect.bottom))) {
            controls.width=rect.right;controls.height=rect.bottom;renderer.resize(controls.width,controls.height);
        }
        const auto displayed=loading_truck_pose(event.truck(),grounded_y,settling);
        render_bonus_scene(renderer,rom,world,event,displayed,loading,view,video_tick,spares,race_hud_visible,tally?&*tally:nullptr,state.paused);
        if(renderer.rain_spray_enabled())renderer.load_spray(spray,float(phase));
        renderer.render();
        if(tally && tally->finished())return BonusFinish{event.collected_reward(),
            event.stayed_parked()?CourseMapEntry::bonus_parked:CourseMapEntry::bonus_moving,std::move(audio)};
    }
    return std::nullopt;
}
}
