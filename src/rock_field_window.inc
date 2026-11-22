#include "srf/rock_field_session.hpp"
#include "rock_field_hud.hpp"

namespace {
using srf::StuntExit;

StuntExit play_stunt_course(const srf::Rom& rom,srf::GameSessionChoice& selection,srf::StuntAttempt attempt,
    const std::filesystem::path& scores_path,std::vector<srf::StuntRecord>& scores,
    const std::filesystem::path& controller,int width,int height,bool borderless) {
    using namespace srf;
    const auto course=selection.stunt_course;
    RockFieldSession session(rom,selection.car,course,attempt);
    const auto& drive=session.drive;
    const auto& rules=session.rules;
    StuntHud hud(rom,selection.car,course,attempt);
    StuntResultsArt results(rom,course);
    const auto world=extract_world(rom,drive.scene());
    BootWindowInput keys;
    ChampionshipWindow window;
    controls=Controls{};controls.race=controls.world=true;
    const auto name=stunt_course_name(course);
    const std::wstring window_title=L"Stunt Race FX — "+std::wstring(name.begin(),name.end())+L" — ";
    window.open(L"StuntRaceFXRockField",(window_title+L"Trailer").c_str(),boot_window_proc,&keys,width,height,borderless);
    generated::CoursePackage colors{};
    colors.day_palette=colors.dusk_palette=extract_rock_field_palette(rom,course);
    Renderer renderer(window.window,nullptr,desktop_fps);
    renderer.set_course_colors(colors);
    renderer.load_textures(extract_rock_field_sprites(rom,course));
    renderer.load_backdrop(extract_rock_field_backdrop(rom,course));
    renderer.set_environment({});
    renderer.load_world(world,0);
    std::unique_ptr<Gamepad> pad;
    std::unique_ptr<AudioDevice> audio;
    try {pad=std::make_unique<Gamepad>(controller);}catch(const std::exception& e){std::cerr<<"Stunt controller: "<<e.what()<<'\n';}
    try {
        audio=std::make_unique<AudioDevice>(extract_stunt_audio(rom,course));
        audio->play_music(extract_trailer_loading_music(rom));
        audio->update({},false,false,false,true);
    }catch(const std::exception& e){std::cerr<<"Stunt audio: "<<e.what()<<'\n';audio.reset();}
    const auto tune=extract_race_audio_tuning(rom.bytes(),drive.player().body.word(0x66));
    RaceAudioState motor{};
    StuntResults flow;
    const auto& page=flow.page;
    const auto& tick=session.tick;
    const auto& released=session.released;
    double remainder=0;
    bool previous_pause=false,previous_h=false,show_hud=true,paused=false,silent=false,driving_music=false;
    std::optional<unsigned> rendered_phase;
    std::wstring caption;
    ShowWindow(window.window,SW_SHOW);UpdateWindow(window.window);
    using Clock=std::chrono::steady_clock;
    auto last=Clock::now();
    while(!keys.quit) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT)return StuntExit::quit;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(keys.quit)return StuntExit::quit;
        if(keys.escape)return StuntExit::menu;
        const bool focused=GetForegroundWindow()==window.window && !IsIconic(window.window);
        const bool racing=page==StuntPage::drive;
        const auto input=racing?combine_player_input(keyboard_control(window.window),pad?pad->poll(focused):GamepadInput{}):GamepadInput{};
        const auto held=keys.held()|(!racing && pad?pad->poll_frontend(focused):0);
        if(focused && racing) {
            if((keys.down['P'] && !previous_pause) || input.pause)paused=!paused;
            if(input.disconnected)paused=true;
            if(keys.down['H'] && !previous_h)show_hud=!show_hud;
            if(keys.down['R'] || input.restart)return StuntExit::restart;
        }
        previous_h=keys.down['H'];previous_pause=keys.down['P'];
        const bool hold=!focused || paused;
        if(audio && silent!=hold){audio->pause(hold);silent=hold;}
        const auto now=Clock::now();
        if(!hold)remainder+=std::min(.25,std::chrono::duration<double>(now-last).count())*RockFieldSession::field_rate;
        last=now;
        if(!focused){Sleep(12);continue;}
        while(remainder>=1) {
            --remainder;
            if(page!=StuntPage::drive) {
                if(auto exit=flow.advance(static_cast<std::uint16_t>(held),selection.car))return *exit;
                continue;
            }
            const auto section=rules.section(),stars=rules.stars();
            const bool stepped=session.advance(input.requested,input.steering_q15);
            const bool throttle=(input.requested&0x10)!=0,boost=(drive.player().body.word(0x42)&0x400)!=0;
            hud.advance(boost,drive.drive_started());
            RaceAudioOutput cues;
            if(stepped) {
                cues=update_race_audio(motor,tune,drive.player(),{!drive.drive_started(),throttle});
                if(rules.stars()!=stars)cues.effects.push_back(0x3c);
                if(rules.section()!=section)cues.effects.push_back(0x35);
            }
            const auto stage=hud.stage(tick+3),prior=hud.stage(tick+2);
            append_countdown_audio(static_cast<std::uint8_t>(prior+2),static_cast<std::uint8_t>(stage+2),cues);
            if(audio) {
                if(tick==hud.trailer_song_end())audio->update({},false,false,false,false);
                const bool start_song=!driving_music && hud.driving_song_started(tick);
                if(start_song){audio->play_music();driving_music=true;}
                if(start_song || stepped || !cues.effects.empty())
                    audio->update(cues,throttle,boost,drive.drive_started(),tick<hud.trailer_song_end() || driving_music);
            }
            if(rules.outcome()!=RockFieldOutcome::driving) {
                flow.end_run(rules,selection.initials,selection.car);
                if(rules.outcome()==RockFieldOutcome::finished) {
                    try {scores=save_stunt_score(scores_path,course,flow.score);}
                    catch(const std::exception& e){MessageBoxA(window.window,e.what(),"Stunt Trax could not save the result",MB_OK|MB_ICONWARNING);}
                    if(audio){audio->play_music(extract_bonus_tally_music(rom),false,extract_bonus_tally_outro(rom));audio->update({},false,false,false,true);}
                } else {
                    if(audio){audio->update({},false,false,false,false);audio->play_race_effect(0x31);}
                }
            }
        }
        RECT rect{};GetClientRect(window.window,&rect);
        if(rect.right<=0 || rect.bottom<=0)continue;
        if(controls.width!=unsigned(rect.right) || controls.height!=unsigned(rect.bottom)) {
            controls.width=rect.right;controls.height=rect.bottom;
            renderer.resize(controls.width,controls.height);rendered_phase.reset();
        }
        const wchar_t* state=page==StuntPage::drive?(paused?L"Paused":drive.drive_started()?L"Driving":L"Trailer"):
            page==StuntPage::time_up?L"Time Up":(page==StuntPage::finish || page==StuntPage::finish_line)?L"Finish":page==StuntPage::records?L"Records":
            page==StuntPage::retry?L"Try Again":L"Change Car";
        std::wstring title=window_title+state;
        if(title!=caption){SetWindowTextW(window.window,title.c_str());caption=std::move(title);}
        const auto& player=drive.player();
        if(page==StuntPage::drive) {
            if(rendered_phase!=drive.phase()) {
                std::uint8_t facing=static_cast<std::uint8_t>(player.body.word(0x24)>>8);
                if(!drive.drive_started()) {
                    const auto eye=drive.trailer_camera();
                    const auto center=renderer.normalize_world({float(eye[0]),float(eye[1]),float(eye[2])});
                    controls.target_x=center[0];controls.target_y=center[1];controls.target_z=center[2];
                    constexpr float radians=6.28318530718f/65536.f;
                    controls.yaw=-eye[4]*radians;controls.pitch=eye[3]*radians;controls.zoom=2;controls.distance=0;
                    facing=static_cast<std::uint8_t>(-eye[4]>>8);
                }
                auto pose=vehicle_pose(rom,player,player,true,1,facing);
                if(drive.drive_started())chase_camera(renderer,pose,lerp_angle(player.body.word(0x24),player.body.word(0x24),1),{},CourseId::easy_ride);
                renderer.load_stunt_world(world,pose,rom,drive,rules);
                rendered_phase=drive.phase();
            }
            hud.draw(renderer,player,rules,tick,released,show_hud);
            renderer.render();
        } else if(page==StuntPage::time_up || page==StuntPage::finish_line) {
            hud.ended(renderer,player,rules,page==StuntPage::time_up,flow.age);renderer.render();
        } else renderer.render_course_map(results.draw(page,flow.score,selection.car,scores,flow.choice,flow.age,hud.panel(player.body,rules.stars())));
    }
    return StuntExit::quit;
}
}
