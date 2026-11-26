#include "srf/rock_field_session.hpp"
#include "rock_field_hud.hpp"

namespace {
using srf::StuntExit;

struct StuntReplay {
    std::optional<srf::StuntCourse> course;
    srf::PlayerCar car=srf::PlayerCar::four_wd;
    std::vector<RecordedInput> inputs;
};

LRESULT CALLBACK stunt_window_proc(HWND window,UINT message,WPARAM key,LPARAM data) {
    if(race_camera.enabled && ((message==WM_KEYDOWN && (key=='C' || key==VK_F2)) ||
        (race_camera.look && (message==WM_RBUTTONDOWN || message==WM_RBUTTONUP || message==WM_CAPTURECHANGED ||
                              message==WM_MOUSEMOVE || message==WM_MOUSEWHEEL))))
        return window_proc(window,message,key,data);
    return boot_window_proc(window,message,key,data);
}

std::filesystem::path stunt_scores_path(const std::filesystem::path& directory,srf::StuntCourse course) {
    return directory/(std::string(srf::stunt_course_id(course))+".sav");
}

StuntExit play_stunt_attempt(const srf::Rom& rom,srf::GameSessionChoice& selection,srf::StuntAttempt attempt,
    const std::filesystem::path& scores_path,std::vector<srf::StuntRecord>& scores,HWND window,
    BootWindowInput& keys,Renderer& renderer,srf::Gamepad* pad,std::optional<srf::World>& world,
    std::span<const RecordedInput> replay) {
    using namespace srf;
    const auto course=selection.stunt_course;
    RockFieldSession session(rom,selection.car,course,attempt);
    const auto& drive=session.drive;
    const auto& rules=session.rules;
    WetTireSpray spray;
    const bool wet=renderer.rain_spray_enabled();
    StuntHud hud(rom,selection.car,course,attempt);
    StuntResultsArt results(rom,course);
    controls.race=controls.world=true;
    const auto name=stunt_course_name(course);
    const std::wstring window_title=L"Stunt Race FX — "+std::wstring(name.begin(),name.end())+L" — ";
    if(!world) {
        renderer.select_course(std::nullopt,course);renderer.use_classic_frame(rom);
        world=extract_world(rom,drive.scene());
        generated::CoursePackage colors{};
        colors.day_palette=colors.dusk_palette=extract_rock_field_palette(rom,course);
        renderer.set_course_colors(colors);
        renderer.load_textures(extract_rock_field_sprites(rom,course));
        renderer.load_backdrop(extract_rock_field_backdrop(rom,course));
        renderer.set_environment({});
        renderer.load_world(*world,0);
    }
    renderer.clear_bonus_overlay();
    std::unique_ptr<AudioDevice> audio;
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
    bool previous_pause=false,previous_h=false,paused=false,silent=false,driving_music=false;
    std::optional<unsigned> rendered_phase;
    RemasteredCamera last_camera;
    std::wstring caption;
    using Clock=std::chrono::steady_clock;
    auto last=Clock::now();
    while(!game_closing && !keys.quit) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT)return StuntExit::quit;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(keys.quit)return StuntExit::quit;
        if(keys.escape)return StuntExit::course;
        const bool focused=game_focused(window);
        const bool racing=page==StuntPage::drive;
        const auto input=racing?combine_player_input(keyboard_control(window),pad?pad->poll(focused,race_camera.look):GamepadInput{}):GamepadInput{};
        const auto held=keys.held()|(!racing && pad?pad->poll_frontend(focused):0);
        if(focused && racing) {
            if(paused && input.restart)return StuntExit::course;
            if((keys.key('P') && !previous_pause) || input.pause)paused=!paused;
            if(input.disconnected)paused=true;
            if(keys.key('H') && !previous_h)race_hud_visible=!race_hud_visible;
            if(keys.key('R') || input.restart)return StuntExit::restart;
        }
        previous_h=keys.key('H');previous_pause=keys.key('P');
        const bool hold=!focused || paused;
        if(audio && silent!=hold){audio->pause(hold);silent=hold;}
        const auto now=Clock::now();
        const auto elapsed=std::min(.25,std::chrono::duration<double>(now-last).count());
        if(!hold)remainder+=elapsed*RockFieldSession::field_rate;
        if(racing && focused && race_camera.look) {
            race_camera.rotate(float(input.look_x*elapsed*2.3),float(input.look_y*elapsed*1.7));
            if(controls.recenter || input.recenter)race_camera.center();
            controls.recenter=false;
        }
        last=now;
        if(!focused && !game_xr){Sleep(12);continue;}
        while(remainder>=1) {
            --remainder;
            if(page!=StuntPage::drive) {
                if(auto exit=flow.advance(static_cast<std::uint16_t>(held),selection.car))return *exit;
                continue;
            }
            const auto section=rules.section(),stars=rules.stars();
            const auto phase=drive.phase();
            const auto buttons=replay.empty()?input.requested:(phase<replay.size()?replay[phase].buttons:0);
            std::optional<Vehicle> before;
            if(wet && drive.drive_started())before=drive.player();
            const bool stepped=session.advance(buttons,replay.empty()?input.steering_q15:std::nullopt);
            const bool throttle=(buttons&0x10)!=0,boost=(drive.player().body.word(0x42)&0x400)!=0;
            hud.advance(boost,drive.drive_started());
            RaceAudioOutput cues;
            if(stepped) {
                if(before) {
                    spray.begin_phase(drive.phase());
                    spray.emit(drive.phase(),0,*before,drive.player());
                }
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
                    catch(const std::exception& e){MessageBoxA(window,e.what(),"Stunt Trax could not save the result",MB_OK|MB_ICONWARNING);}
                    if(audio){audio->play_music(extract_bonus_tally_music(rom),false,extract_bonus_tally_outro(rom));audio->update({},false,false,false,true);}
                } else {
                    if(audio){audio->update({},false,false,false,false);audio->play_race_effect(0x31);}
                }
            }
        }
        RECT rect{};GetClientRect(window,&rect);
        if(rect.right<=0 || rect.bottom<=0)continue;
        if(controls.width!=unsigned(rect.right) || controls.height!=unsigned(rect.bottom)) {
            controls.width=rect.right;controls.height=rect.bottom;
            renderer.resize(controls.width,controls.height);rendered_phase.reset();
        }
        const wchar_t* state=page==StuntPage::drive?(paused?L"Paused":drive.drive_started()?L"Driving":L"Trailer"):
            page==StuntPage::time_up?L"Time Up":(page==StuntPage::finish || page==StuntPage::finish_line)?L"Finish":page==StuntPage::records?L"Records":
            page==StuntPage::retry?L"Try Again":L"Change Car";
        std::wstring title=window_title+state;
        if(title!=caption){SetWindowTextW(window,title.c_str());caption=std::move(title);}
        const auto& player=drive.player();
        if(page==StuntPage::drive) {
            if(rendered_phase!=drive.phase() || race_camera!=last_camera) {
                renderer.grid_sweep=!drive.drive_started();
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
                renderer.load_stunt_world(*world,pose,rom,session);
                if(wet)renderer.load_spray(spray,float(drive.phase()));
                rendered_phase=drive.phase();
                last_camera=race_camera;
            }
            hud.draw(renderer,player,rules,tick,released,race_hud_visible);
            renderer.render();
        } else if(page==StuntPage::time_up || page==StuntPage::finish_line) {
            hud.ended(renderer,player,rules,page==StuntPage::time_up,flow.age);renderer.render();
        } else renderer.render_course_map(results.draw(page,flow.score,selection.car,scores,flow.choice,flow.age,hud.panel(player.body,rules.stars())));
    }
    return StuntExit::quit;
}
StuntExit play_stunt_course(const srf::Rom& rom,srf::GameSessionChoice& selection,const std::filesystem::path& directory,
    std::vector<srf::StuntRecord>& scores,HWND window,BootWindowInput& keys,Renderer& renderer,srf::Gamepad* pad,
    const StuntReplay& replay) {
    const auto path=stunt_scores_path(directory,selection.stunt_course);
    std::optional<srf::World> world;
    auto attempt=srf::StuntAttempt::first;
    for(;;) {
        keys.suppress_held();
        if(pad)pad->poll(false); // Require neutral controls before the new attempt.
        std::span<const RecordedInput> inputs;
        if(replay.course==selection.stunt_course && replay.car==selection.car)inputs=replay.inputs;
        const auto exit=play_stunt_attempt(rom,selection,attempt,path,scores,window,keys,renderer,pad,world,inputs);
        if(GetCapture()==window)ReleaseCapture();
        if(exit==StuntExit::retry_finish)attempt=srf::StuntAttempt::after_finish;
        else if(exit==StuntExit::retry_timeout)attempt=srf::StuntAttempt::after_timeout;
        else if(exit==StuntExit::restart)attempt=srf::StuntAttempt::first;
        else return exit;
    }
}

}
