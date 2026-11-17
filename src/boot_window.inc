#include "boot_menu_art.hpp"
#include "boot_title_art.hpp"
#include "stunt_board_art.hpp"
#include "srf/demo_flow.hpp"
#include "demo_scene.inc"

namespace {
// The build smoke test takes the same ROM-backed title through the D3D presenter.
void capture_boot_title(const srf::Rom& rom,const std::filesystem::path& path) {
    struct ComScope {
        HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        ~ComScope(){if(SUCCEEDED(hr))CoUninitialize();}
    } com;
    check(com.hr,"Boot screenshot COM initialization");
    srf::BootTitle title;
    for(unsigned tick=0;tick<520;++tick)title.advance(false,false);
    std::vector<std::uint32_t> pixels(256*224);
    srf::BootTitleArt(rom).draw(pixels,title.frame());
    Renderer renderer(nullptr);
    renderer.render_course_map(pixels);
    renderer.screenshot(path);
}

bool run_boot_menus(const srf::Rom& rom,srf::GameSessionChoice& selection,bool cold,
                    const std::filesystem::path& controller,bool borderless,
                    std::unique_ptr<srf::AudioDevice>& audio,
                    srf::StuntMenuReturn resume=srf::StuntMenuReturn::game_selection,
                    const std::array<std::vector<srf::StuntRecord>,srf::playable_stunt_courses.size()>& records={}) {
    BootWindowInput keys;
    ChampionshipWindow scope;
    scope.open(L"StuntRaceFXBoot",L"Stunt Race FX",boot_window_proc,&keys,controls.width,controls.height,borderless);
    Renderer renderer(scope.window);
    srf::BootTitleArt title_art(rom);
    srf::BootMenuArt menu_art(rom);
    std::optional<srf::BootTitle> title;
    if(cold)title.emplace();
    std::optional<srf::DemoFlow> demo;
    std::optional<IdleDemoScene> scene;
    srf::BootMenu menu(selection.car,selection.initials);
    std::optional<srf::StuntBoard> stunt;
    std::optional<srf::StuntBoardArt> stunt_art;
    const auto open_stunt=[&](){
        stunt.emplace(selection.car);
        if(!stunt_art)stunt_art.emplace(rom,records);
    };
    if(resume==srf::StuntMenuReturn::course)open_stunt();
    else if(resume==srf::StuntMenuReturn::car)menu.resume_car(1);
    std::vector<std::uint32_t> pixels(256*224);
    std::unique_ptr<srf::Gamepad> pad;
    try{pad=std::make_unique<srf::Gamepad>(controller);}
    catch(const std::exception& e){std::cerr<<"Boot controller: "<<e.what()<<'\n';}
    try {
        audio=std::make_unique<srf::AudioDevice>(srf::extract_title_audio(rom));
        audio->update({},false,false,false,false);
        if(!cold && !stunt)audio->play_music();
    }catch(const std::exception& e){std::cerr<<"Boot audio: "<<e.what()<<'\n';audio.reset();}
    ShowWindow(scope.window,SW_SHOW);UpdateWindow(scope.window);
    using Clock=std::chrono::steady_clock;
    constexpr double period=1./60.0988;
    auto last=Clock::now();
    double remainder=0;
    bool audio_paused=false;
    std::uint16_t held_on_entry=0;
    const wchar_t* last_caption=nullptr;
    while(!keys.quit) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT){keys.quit=true;break;}
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(keys.quit || (keys.escape && !demo && !stunt && (title || menu.frame().screen==srf::BootMenuScreen::game)))return false;
        if(keys.escape && stunt && stunt->page==srf::StuntBoard::Page::courses) {
            // Desktop Escape returns to mode selection even on cartridge pages with no back button.
            stunt.reset();menu=srf::BootMenu(selection.car,selection.initials);
            try{audio=std::make_unique<srf::AudioDevice>(srf::extract_title_audio(rom));audio->play_music();}
            catch(const std::exception& e){std::cerr<<"Menu audio: "<<e.what()<<'\n';audio.reset();}
        }
        keys.escape=false;
        const bool focused=GetForegroundWindow()==scope.window && !IsIconic(scope.window);
        const auto held=static_cast<std::uint16_t>(keys.held()|(pad?pad->poll_frontend(focused):0));
        const auto now=Clock::now();
        if(focused)remainder+=std::min(.16,std::chrono::duration<double>(now-last).count());
        last=now;
        const bool paused=!focused;
        if(audio && audio_paused!=paused) {
            audio_paused=paused;
            try {audio->pause(paused);}
            catch(const std::exception& e){std::cerr<<"Boot audio: "<<e.what()<<'\n';audio.reset();}
        }
        while(focused && remainder>=period) {
            remainder-=period;
            int effect=-1,command=-1;
            if(title) {
                title->advance((held&0x1000)!=0,(held&0x8000)!=0);
                effect=title->frame().effect;
                command=title->frame().music_command;
                if(title->frame().stop_music && audio)audio->update({},false,false,false,false);
                if(title->frame().start_music && audio)audio->play_music();
                if(title->finished()) {
                    if(title->to_demo())demo.emplace();
                    held_on_entry=held;
                    title.reset();
                }
            }else if(demo) {
                demo->advance(held);
                const auto frame=demo->frame();
                // The ROM writes 0C/30 into CPU race queues during loading.
                // Neither reaches the sound ports. Song 1 runs through the drive.
                if(demo->finished()) {
                    if(demo->to_menu()) {
                        menu=srf::BootMenu(selection.car,selection.initials,srf::BootMenuEntry::demo);
                        held_on_entry=held;
                    }else title.emplace(srf::BootTitleEntry::demo_return);
                    scene.reset();
                    demo.reset();
                }else if(scene)scene->advance();
                else if(frame.tick==57)scene.emplace(rom,renderer);
            }else if(stunt) {
                stunt->advance(held);
                effect=stunt->effect;
                if(stunt->tick==82) {
                    try{audio=std::make_unique<srf::AudioDevice>(srf::extract_course_map_audio(rom));audio->play_music();}
                    catch(const std::exception& e){std::cerr<<"Stunt board audio: "<<e.what()<<'\n';audio.reset();}
                }
                if(stunt->page==srf::StuntBoard::Page::loading && stunt->age==104)audio.reset();
                if(stunt->ready_to_drive()){
                    selection.select_stunt_course(static_cast<srf::StuntCourse>(stunt->course));
                    return true;
                }
                if(stunt->unavailable) {
                    srf::show_game_result({L"STUNT TRAX",L"Course unavailable",L"Blue Lake is still in development.",L"Return",L""});
                    remainder=0;last=Clock::now();
                }
            }else {
                held_on_entry&=held;
                menu.advance(static_cast<std::uint16_t>(held&~held_on_entry));
                effect=menu.frame().effect;
                command=menu.frame().music_command;
            }
            if(audio) {
                if(effect>=6)audio->play_race_effect(static_cast<std::uint8_t>(effect));
                if(command>=0) {
                    srf::RaceAudioOutput cues;
                    cues.commands.push_back(static_cast<std::uint8_t>(command));
                    audio->update(cues,false,false,false,true);
                }
            }
            if(!title && !demo && !stunt) {
                if(menu.event()==srf::BootMenuEvent::board) {
                    selection.car=menu.car();
                    selection.initials=menu.initials();
                    selection.stunt_trax=menu.frame().mode==1;
                    selection.speed_trax=!selection.stunt_trax;
                    if(selection.stunt_trax)open_stunt();
                    else return true;
                }
                if(menu.event()==srf::BootMenuEvent::unavailable) {
                    const auto detail=menu.frame().screen==srf::BootMenuScreen::game?
                        L"Speed Trax and Stunt Trax are playable in this build.":L"Manual opponent selection is unavailable.";
                    srf::show_game_result({L"STUNT RACE FX",L"Selection unavailable",detail,L"Return",L""});
                    remainder=0;last=Clock::now();
                    break;
                }
            }
        }
        if(!focused){Sleep(12);continue;}
        RECT rect{};GetClientRect(scope.window,&rect);
        if(rect.right>0 && rect.bottom>0 && (controls.width!=unsigned(rect.right) || controls.height!=unsigned(rect.bottom))) {
            controls.width=rect.right;controls.height=rect.bottom;renderer.resize(controls.width,controls.height);
        }
        const wchar_t* caption=stunt?(stunt->page==srf::StuntBoard::Page::records?L"Stunt Race FX — Stunt Trax Records":
            stunt->page==srf::StuntBoard::Page::data?L"Stunt Race FX — Stunt Trax Data Box":L"Stunt Race FX — Stunt Trax"):
            demo?L"Stunt Race FX — Easy Ride Demo":title?L"Stunt Race FX":
            menu.frame().screen==srf::BootMenuScreen::game?L"Stunt Race FX — Game Selection":
            menu.frame().screen==srf::BootMenuScreen::car?L"Stunt Race FX — Car Selection":L"Stunt Race FX — Input Your Name";
        if(caption!=last_caption){SetWindowTextW(scope.window,caption);last_caption=caption;}
        if(scene)scene->draw(renderer,demo->frame().tick-57,demo->frame().brightness);
        else {
            if(stunt)stunt_art->draw(pixels,*stunt);
            else if(title)title_art.draw(pixels,title->frame());
            else if(demo)std::fill(pixels.begin(),pixels.end(),0xff000000u);
            else menu_art.draw(pixels,menu.frame());
            renderer.render_course_map(pixels);
        }
    }
    return false;
}
}
