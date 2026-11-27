// Enter the real Bonus Trax screen after two qualifying rounds without replaying two races.
#define wmain preview_application_entry
#include "../src/preview.cpp"
#undef wmain
namespace {
ULONGLONG deadline;
bool sent=false;
std::wstring last_title;
void CALLBACK navigate(HWND window,UINT,UINT_PTR,DWORD){
    if(GetTickCount64()>=deadline){PostMessageW(window,WM_CLOSE,0,0);return;}
    wchar_t text[256]{};GetWindowTextW(window,text,256);
    if(last_title!=text){last_title=text;sent=false;return;}
    if(sent)return;
    if(last_title.ends_with(L"Bonus Trax") || last_title.ends_with(L"Resume driving") || last_title.ends_with(L"Course Map")){
        PostMessageW(window,WM_KEYDOWN,VK_ESCAPE,1);PostMessageW(window,WM_KEYUP,VK_ESCAPE,0xc0000001);
        sent=true;
    }
}
}

int wmain(int argc,wchar_t** argv) {
    const auto path=_wgetenv(L"SRF_ROM");
    if(!path)return 77;
    try {
        const auto rom=srf::Rom::load(path);
        srf::Championship progress(srf::SpeedClass::novice,srf::PlayerCar::four_wd,"AAA");
        srf::ChampionshipProgress records;
        const auto qualify=[&]{
            return progress.finish({progress.course(),srf::RaceFailure::none,1,32,9000,false,{1,2,3,4}},records);
        };
        qualify();progress.advance();
        if(qualify()!=srf::ChampionshipOutcome::bonus)throw std::runtime_error("Bonus fixture did not qualify");
        const std::wstring_view mode=argc==2?argv[1]:L"original";
        RaceGraphics graphics{};
        std::optional<srf::vr::Drive> xr;
        if(mode==L"vr"){xr.emplace(std::cout,90,60);game_xr=&*xr;}
        else if(mode==L"n64")graphics.look=RaceLook::n64;
        else if(mode==L"remastered")graphics.look=RaceLook::remastered;
        else if(mode!=L"original")throw std::invalid_argument("Unknown test style");
        controls.width=640;controls.height=480;
        GameHost host;host.open(false,graphics);
        const auto window=host.window;
        ShowWindow(window,SW_SHOW);
        deadline=GetTickCount64()+35000;
        if(!SetTimer(window,1,350,navigate))throw std::runtime_error("Cannot start input fixture");
        auto finish=srf::play_speed_bonus(host,rom,progress,{});
        if(!finish || finish->entry!=srf::CourseMapEntry::retire || game_closing)
            throw std::runtime_error("Could not retire from the hosted bonus");
        progress.finish_bonus(finish->reward.extra_seconds,finish->reward.extra_cars);
        if(progress.round()!=3)throw std::runtime_error("Round three was not reached");
        show_course_map(host,rom,progress,records,finish->entry,{},finish->audio);
        KillTimer(window,1);
        if(game_closing || !IsWindow(window) || window!=host.window || !last_title.ends_with(L"Course Map"))
            throw std::runtime_error("Round-three course map did not use the game host");
        std::cout<<"Bonus Trax to round-three course map passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
