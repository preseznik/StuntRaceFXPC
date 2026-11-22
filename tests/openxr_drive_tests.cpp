#define wmain preview_application_entry
#include "../src/preview.cpp"
#undef wmain
#include <tuple>

void verify_test_runtime();
ID3D11Texture2D* test_hud_texture();
void reset_test_runtime(bool);
float test_refresh_request();
namespace {
void verify(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void desktop_limits(){
    for(unsigned rate:{60,120}){
        DesktopFrameLimit limit(rate);
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<24;++i)limit.wait();
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        verify(seconds>=23.9/rate,"Desktop frame cap presented early");
        std::cout<<"Desktop "<<rate<<" FPS pacer, measured "<<24/seconds<<" Hz\n";
    }
}
void refresh_settings(){
    for(auto [extension,preferred,expected]:{std::tuple{true,120u,120},std::tuple{true,72u,0},std::tuple{false,120u,0}}){
        reset_test_runtime(extension);
        std::ostringstream log;srf::vr::Drive drive(log,preferred,75);
        int draws=0;
        drive.run([]{return true;},[](int64_t,bool){return srf::vr::Display{};},
                  [&](ID3D11Texture2D* texture,DXGI_FORMAT,const auto&){
                      D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);
                      verify(d.Width==576 && d.Height==384,"75% eye size");++draws;
                  });
        verify(draws==8 && test_refresh_request()==expected,"Unsupported or available headset refresh handling");
        D3D11_TEXTURE2D_DESC d{};test_hud_texture()->GetDesc(&d);verify(d.Width==960 && d.Height==148,"Scaled HUD");
    }
    reset_test_runtime(false);
}
void full_races(const srf::Rom& rom){
    auto tape=read_inputs(std::filesystem::path(SRF_TEST_FIXTURES)/"easy_ride_clean_inputs.csv");
    srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::easy_ride);
    const auto grid=race.racers();
    std::array<srf::Vehicle,4> reference{};unsigned finish_clock{};
    for(int rate:{60,90,120,0}){
        race.reset();srf::vr::BonnetMotionClock clock;srf::RaceAudioState sound{};
        srf::NativeRace baseline(rom);
        auto midpoint=grid;
        verify(grid[0].body.words==race.racers()[0].body.words && race.phase()==0,"Restart grid");
        int64_t now=1000000000;size_t i=0;int frames=0;
        verify(!clock.advance(now,true,[]{throw std::runtime_error("Initial phase ran early");}),"Initial clock");
        unsigned extra_poses=0;
        bool half=false;
        while(i<tape.size() || half){
            constexpr int jitter[]{7000000,16000000,11000000,8000000};
            now+=rate?1000000000/rate:jitter[frames%4];++frames;
            half=clock.advance(now,true,[&]{
                verify(i<tape.size(),"Extra native phase");
                const auto before=race.racers()[race.player_index()].body;
                srf::RaceAudioOutput events;
                auto input=srf::prepare_player_input(sound,race.racers()[race.player_index()].body,tape[i].buttons,race.countdown_active(),events);
                race.step(input,tape[i].ticks,std::nullopt,&midpoint);baseline.step(input,tape[i].ticks);++i;
                for(unsigned j=0;j<4;++j){
                    verify(race.racers()[j].body.words==baseline.racers()[j].body.words,"Midpoint changed car");
                    for(unsigned k=0;k<4;++k)verify(race.racers()[j].wheels[k].words==baseline.racers()[j].wheels[k].words,"Midpoint changed wheel");
                }
                const auto position=[](const srf::VehicleState& b){return std::array{b.word(0x1e),b.word(0x20),b.word(0x22)};};
                const auto mid=position(midpoint[race.player_index()].body);
                extra_poses+=mid!=position(before) && mid!=position(race.racers()[race.player_index()].body);
            });
        }
        verify(race.completion().finished && race.completion().acknowledged_laps==3,"Complete Easy Ride");
        if(rate==60){reference=race.racers();finish_clock=race.remaining_seconds();}
        for(size_t j=0;j<4;++j){
            verify(reference[j].body.words==race.racers()[j].body.words,"Render rate changed racer");
            for(int k=0;k<4;++k)verify(reference[j].wheels[k].words==race.racers()[j].wheels[k].words,"Render rate changed wheel");
        }
        verify(finish_clock==race.remaining_seconds(),"Finish clock differs");
        verify(now-1000000000>=int64_t(tape.size())*1000000000/14 &&
               now-1000000000<int64_t(tape.size())*1000000000/14+17000000,"Unexpected race duration");
        std::cout<<"Replay "<<rate<<" Hz ("<<frames<<" presents), "<<race.phase()<<" phases, "<<finish_clock<<" seconds left, "
                 <<extra_poses<<" distinct native midpoint positions\n";
        verify(extra_poses>1000,"Driving rarely produced a distinct native midpoint");
    }
    srf::vr::RaceClock clock;clock.advance(1000000000,true);clock.advance(1060000000,true);
    verify(!clock.advance(1070000000,false) && !clock.advance(2070000000,true) &&
           !clock.advance(2110000000,true) && clock.advance(2150000000,true)==1,"Clock advanced during focus loss");
}
}
int wmain(int argc,wchar_t** argv){
    try{
        const auto path=argc>1?argv[1]:_wgetenv(L"SRF_ROM");if(!path)return 77;
        auto rom=srf::Rom::load(path);full_races(rom);desktop_limits();
        const auto out=argc>2?std::filesystem::path(argv[2]):std::filesystem::path();
        check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
        controls=Controls{};controls.world=controls.race=true;
        refresh_settings();
        srf::vr::Drive drive(std::cout);Renderer renderer(nullptr,drive.device());
        const auto world=srf::extract_world(rom);renderer.load_textures(srf::extract_vehicle_sprites(rom));renderer.load_world(world,0);
        srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::easy_ride);
        srf::CourseScenery scenery(rom,race.course_id());
        const auto route=read_inputs(std::filesystem::path(SRF_TEST_FIXTURES)/"easy_ride_clean_inputs.csv");
        srf::RaceAudioState sound{};
        std::array<srf::Vehicle,4> midpoint,start;
        for(unsigned i=0;i<165;++i){
            srf::RaceAudioOutput events;
            auto previous=race.racers()[race.player_index()].body;
            start=race.racers();
            race.step(srf::prepare_player_input(sound,previous,route[i].buttons,race.countdown_active(),events),route[i].ticks,std::nullopt,&midpoint);
            scenery.step(previous,0);
        }
        auto surface=renderer.make_target(768,512);
        const auto capture=[&](const auto& cars){
            load_bonnet_pose(renderer,world,rom,race,scenery,cars);
            renderer.render_eye(surface,SceneEye{});return renderer.readback_bgra(surface);
        };
        const auto first=capture(start),middle=capture(midpoint),complete=capture(race.racers());
        verify(middle!=first && middle!=complete,"The native midpoint did not reach the rendered bonnet camera");
        int frame=0,draws=0;
        std::vector<BYTE> centered[2];
        drive.run([]{return true;},[&](int64_t,bool focused){
            ++frame;verify(focused==(frame!=4 && frame!=5),"Focus or tracking handling");
            auto hud=bonnet_instruments(race,false);hud.recenter=frame==2;return hud;
        },[&](ID3D11Texture2D* t,DXGI_FORMAT format,const srf::vr::Eye& eye){
            const int side=draws++%2;
            if(frame==1 || frame==3)verify(std::abs(eye.meters[0]-(side?.032f:-.032f))<.001f && std::abs(eye.orientation[1])<.001f,"Seat origin or IPD");
            if(frame==2)verify(eye.meters[0]>.08f && eye.orientation[1]<-.1f,"Tracked translation and handedness");
            auto target=renderer.make_target(768,512,t,format);
            renderer.render_eye(target,bonnet_eye(renderer,eye));auto pixels=renderer.readback_bgra(target);
            if(frame==1){
                centered[side]=pixels;
                if(!out.empty())Renderer::write_png(out/(side?"right.png":"left.png"),pixels,768,512);
            }else if(frame==2)verify(pixels!=centered[side],"Head tracking did not move eye view");
            else if(frame==3)verify(pixels==centered[side],"Recenter did not restore seat");
        });
        verify_test_runtime();verify(centered[0]!=centered[1],"Stereo eyes identical");
        SceneTarget panel{960,148,test_hud_texture()};
        auto hud=renderer.readback_bgra(panel);
        for(int row:{0,74}){
            unsigned count=0;for(int y=row;y<row+74;++y)for(int x=0;x<960;++x)count+=hud[(y*960+x)*4]>160;
            verify(count>1000,"Missing HUD text");
        }
        if(!out.empty())Renderer::write_png(out/"hud.png",hud,960,148);
        std::cout<<"Stereo, head motion, seated recenter, compositor HUD, tracking loss and shutdown passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
