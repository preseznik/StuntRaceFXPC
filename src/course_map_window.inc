#include "course_map_art.hpp"
#include "srf/class_board.hpp"

namespace {
struct CourseMapWindowState {
    bool quit{}, enter{}, w{}, up{}, block_start{}, block_accelerator{};
    bool left{}, right{}, block_left{}, block_right{};
};
LRESULT CALLBACK course_map_window_proc(HWND window,UINT message,WPARAM key,LPARAM data) {
    auto* state=reinterpret_cast<CourseMapWindowState*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        state=static_cast<CourseMapWindowState*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
    }
    if(state) {
        if(message==WM_CLOSE) {state->quit=true;return 0;}
        if(message==WM_SETFOCUS) {
            const auto down=[](int k){return (GetAsyncKeyState(k)&0x8000)!=0;};
            state->enter=state->block_start=down(VK_RETURN);
            state->w=down('W');state->up=down(VK_UP);
            state->block_accelerator=state->w || state->up;
            state->left=state->block_left=down(VK_LEFT);
            state->right=state->block_right=down(VK_RIGHT);
        }
        if(message==WM_KEYDOWN || message==WM_KEYUP) {
            const bool down=message==WM_KEYDOWN;
            if(down && key==VK_ESCAPE) {state->quit=true;return 0;}
            if(key==VK_RETURN)state->enter=down;
            if(key=='W')state->w=down;
            if(key==VK_UP)state->up=down;
            if(key==VK_LEFT)state->left=down;
            if(key==VK_RIGHT)state->right=down;
            if(!state->left)state->block_left=false;
            if(!state->right)state->block_right=false;
            if(!state->enter)state->block_start=false;
            if(!state->w && !state->up)state->block_accelerator=false;
        }
    }
    return DefWindowProcW(window,message,key,data);
}

void course_map_sound(const srf::Rom& rom,srf::CourseId course,std::unique_ptr<srf::AudioDevice>& audio,
                      const srf::CourseMapFrame& frame) {
        try {
            if(frame.stop_music)audio.reset();
            if(frame.start_music) {
                audio=std::make_unique<srf::AudioDevice>(srf::extract_course_map_audio(rom));
                audio->play_music();
            }
            if(frame.start_grid_music) {
                audio=std::make_unique<srf::AudioDevice>(srf::extract_course_audio(rom,course));
                audio->play_music(srf::extract_trailer_loading_music(rom),true);
            }
            if(audio && (frame.start_grid_music || frame.start_music || frame.effect>=0 || frame.music_command>=0)) {
                srf::RaceAudioOutput cues;
                if(frame.page_sound && frame.effect!=0x45)cues.effects.push_back(0x45);
                if(frame.effect>=6)cues.effects.push_back(static_cast<std::uint8_t>(frame.effect));
                if(frame.music_command>=0)cues.commands.push_back(static_cast<std::uint8_t>(frame.music_command));
                audio->update(cues,false,false,frame.start_grid_music,true);
            }
        }catch(const std::exception& e){std::cerr<<"Course map audio: "<<e.what()<<'\n';audio.reset();}
}
bool show_course_map(GameHost& host,const srf::Rom& rom,const srf::Championship& championship,const srf::ChampionshipProgress& progress,
                     srf::CourseMapEntry entry,const std::filesystem::path& controller,
                     std::unique_ptr<srf::AudioDevice>& audio) {
    CourseMapWindowState state;
    GameScreen scope(host,L"Stunt Race FX — Course Map",course_map_window_proc,&state);
    RECT rect{};
    auto& renderer=*host.presenter;
    const srf::CourseMapArt art(rom,championship,progress);
    srf::CourseMap map(static_cast<unsigned>(championship.league()),progress.available(srf::SpeedClass::master),
                       entry,false,false,static_cast<unsigned>(championship.car()),championship.round());
    std::vector<std::uint32_t> pixels(256*224);
    std::unique_ptr<srf::Gamepad> pad;
    try {pad=std::make_unique<srf::Gamepad>(controller);}
    catch(const std::exception& e){std::cerr<<"Course map controller: "<<e.what()<<'\n';}
    const auto play_audio=[&](const srf::CourseMapFrame& frame){course_map_sound(rom,championship.course(),audio,frame);};
    ShowWindow(scope.window,SW_SHOW);UpdateWindow(scope.window);
    play_audio(map.frame());
    using Clock=std::chrono::steady_clock;
    auto last=Clock::now();
    double remainder=0;
    constexpr double period=1./60.0988;
    bool audio_paused=false;
    while(!game_closing && !state.quit) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT){state.quit=true;break;}
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(state.quit)break;
        const bool focused=game_focused(scope.window);
        const auto buttons=pad?pad->poll_course_map(focused):srf::CourseMapButtons{};
        const auto now=Clock::now();
        if(focused)remainder+=std::min(.16,std::chrono::duration<double>(now-last).count());
        last=now;
        const bool paused=!focused;
        if(audio && audio_paused!=paused) {
            audio_paused=paused;
            try {audio->pause(audio_paused);}catch(const std::exception& e){std::cerr<<"Course map audio: "<<e.what()<<'\n';audio.reset();}
        }
        while(focused && remainder>=period) {
            remainder-=period;
            map.advance(buttons.start || (state.enter && !state.block_start),
                        buttons.accept || ((state.w || state.up) && !state.block_accelerator),
                        buttons.left || (state.left && !state.block_left),buttons.right || (state.right && !state.block_right));
            play_audio(map.frame());
            if(map.finished())return true;
        }
        if(!game_xr && IsIconic(scope.window)){Sleep(30);continue;}
        GetClientRect(scope.window,&rect);
        if(rect.right>0 && rect.bottom>0 && (controls.width!=unsigned(rect.right) || controls.height!=unsigned(rect.bottom))) {
            controls.width=rect.right;controls.height=rect.bottom;renderer.resize(controls.width,controls.height);
        }
        art.draw(pixels,map.frame());
        renderer.render_course_map(pixels);
    }
    return false;
}

std::optional<srf::SpeedClass> show_initial_course_map(GameHost& host,const srf::Rom& rom,const srf::ChampionshipProgress& progress,
                      srf::PlayerCar car,const std::filesystem::path& controller,
                      std::unique_ptr<srf::AudioDevice>& audio,srf::SpeedClass initial=srf::SpeedClass::novice) {
    BootWindowInput input;
    GameScreen scope(host,L"Stunt Race FX — Speed Trax",boot_window_proc,&input);
    auto& renderer=*host.presenter;
    srf::ClassBoard board(progress.available(srf::SpeedClass::master),static_cast<unsigned>(initial));
    std::vector<std::uint32_t> pixels(256*224);
    std::array<std::optional<srf::CourseMapArt>,3> art;
    std::unique_ptr<srf::Gamepad> pad;
    try {pad=std::make_unique<srf::Gamepad>(controller);}
    catch(const std::exception& e){std::cerr<<"Class selection controller: "<<e.what()<<'\n';}
    ShowWindow(scope.window,SW_SHOW);UpdateWindow(scope.window);
    using Clock=std::chrono::steady_clock;
    auto last=Clock::now();
    double remainder=0;
    bool paused=false,play_caption=false;unsigned previous_buttons=0xffff;
    while(!game_closing && !input.quit) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT){input.quit=true;break;}
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(input.quit || (input.escape && !board.choosing_play()))break;
        input.escape=false;
        const bool focused=game_focused(scope.window);
        const auto buttons=input.held()|(pad?pad->poll_frontend(focused):0);
        if((buttons&~previous_buttons&0x4000) && !board.choosing_play())break;
        previous_buttons=buttons;
        const auto now=Clock::now();
        if(focused)remainder+=std::min(.16,std::chrono::duration<double>(now-last).count());
        last=now;
        if(audio && paused!=!focused) {
            paused=!focused;
            try {audio->pause(paused);}catch(const std::exception& e){std::cerr<<"Class selection audio: "<<e.what()<<'\n';}
        }
        constexpr double period=1./60.0988;
        while(focused && remainder>=period) {
            remainder-=period;board.advance(static_cast<std::uint16_t>(buttons));
            const auto& f=board.frame();
            course_map_sound(rom,static_cast<srf::CourseId>(4*f.speed_class),audio,f);
            if(board.finished())return static_cast<srf::SpeedClass>(f.speed_class);
        }
        if(play_caption!=board.choosing_play()) {
            play_caption=board.choosing_play();
            SetWindowTextW(scope.window,play_caption?L"Stunt Race FX — Play":L"Stunt Race FX — Speed Trax");
        }
        if(!game_xr && IsIconic(scope.window)){Sleep(30);continue;}
        RECT rect{};GetClientRect(scope.window,&rect);
        if(rect.right>0 && rect.bottom>0 && (controls.width!=unsigned(rect.right) || controls.height!=unsigned(rect.bottom))) {
            controls.width=rect.right;controls.height=rect.bottom;renderer.resize(controls.width,controls.height);
        }
        const auto& f=board.frame();
        auto& surface=art.at(f.speed_class);
        if(!surface)surface.emplace(rom,srf::Championship(static_cast<srf::SpeedClass>(f.speed_class),car),progress);
        surface->draw(pixels,f);
        renderer.render_course_map(pixels);
    }
    return std::nullopt;
}
}
