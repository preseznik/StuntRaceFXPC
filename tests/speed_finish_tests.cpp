// Bounded, windowless captures of the Bonus vehicle and one Speed Trax finish.
#define wmain preview_application_entry
#include "../src/preview.cpp"
#undef wmain

namespace {
void check_frame(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}

void trailer(const srf::Rom& rom,RaceLook look,const std::filesystem::path& output) {
    srf::SpeedBonus bonus(rom,srf::SpeedClass::novice);
    const auto world=srf::extract_world(rom,bonus.scene());
    srf::TrailerLoading loading(rom,bonus.scene(),srf::PlayerCar::four_wd);
    TrailerLoadingView view(rom,srf::SpeedClass::novice,srf::PlayerCar::four_wd);
    Renderer renderer(nullptr,nullptr,0,{look});
    renderer.select_course(std::nullopt);
    renderer.set_course_colors(srf::generated::course_packages.front());
    renderer.load_textures(srf::extract_vehicle_sprites(rom,srf::CourseId::easy_ride,false,true));
    renderer.load_backdrop(srf::extract_speed_bonus_backdrop(rom));
    renderer.load_world(world,0);
    loading.advance(195);
    if(!output.empty()) {
        render_bonus_scene(renderer,rom,world,bonus,bonus.truck(),loading,view,195,2,false,nullptr);
        renderer.render();
        renderer.screenshot(output/(look==RaceLook::n64?"trailer-loading-n64.png":"trailer-loading-remastered.png"));
    }
    loading.advance(1005);
    check_frame(loading.door_closed(),"Trailer never closed");
    render_bonus_scene(renderer,rom,world,bonus,bonus.truck(),loading,view,1200,2,false,nullptr);
    renderer.render();
    const auto loaded=renderer.readback_bgra();
    if(!output.empty())renderer.screenshot(output/(look==RaceLook::n64?"trailer-n64.png":"trailer-remastered.png"));
    // Keep the cab and wheels, remove only the cargo polygons. A 4WD substituted
    // for the cargo descriptor would produce exactly the same image here.
    auto without=world;
    for(auto& model:without.models)if(model.descriptor_address==loading.cargo_descriptor())model.primitives.clear();
    render_bonus_scene(renderer,rom,without,bonus,bonus.truck(),loading,view,1200,2,false,nullptr);
    renderer.render();
    const auto no_cargo=renderer.readback_bgra();
    unsigned changed=0;
    for(std::size_t i=0;i<loaded.size();i+=4)if(!std::equal(loaded.begin()+i,loaded.begin()+i+3,no_cargo.begin()+i))++changed;
    check_frame(changed>100,"Cargo mesh is missing from the enhanced trailer");
    std::cout<<"Visible cargo pixels "<<changed<<'\n';
}

// Two exact 256 x 168 viewports keep each player's dashboard pixels independently verifiable.
void split_dashboards(const srf::Rom& rom) {
    Renderer renderer(nullptr,nullptr,0,{RaceLook::classic});
    renderer.resize(256,336);
    const auto course=srf::battle_courses.front();
    srf::LocalRace race(rom,course,srf::PlayerCar::four_wd,srf::PlayerCar::coupe,false);
    renderer.load_world(srf::extract_world(rom,race.scene()),0);
    std::array panels{srf::BonusDashboard(rom,srf::PlayerCar::four_wd),srf::BonusDashboard(rom,srf::PlayerCar::coupe)};
    const std::array images{panels[0].draw(256,0,0,1),panels[1].draw(32,160,0,2)};
    const std::vector<std::uint32_t> overlay(208*128);
    renderer.render({},[&](unsigned player){renderer.set_bonus_overlay(overlay,15,1,40,images[player]);});
    const auto shown=renderer.readback_bgra();
    for(unsigned player=0;player<2;++player){
        const auto first=256*(128+168*player);
        for(unsigned i=0;i<images[player].size();++i){
            const auto rgb=images[player][i];
            for(unsigned c=0;c<3;++c)
                check_frame(shown[4*(first+i)+c]==((rgb>>(16-8*c))&255),"Split dashboard differs from player's cartridge panel");
        }
    }
    renderer.clear_bonus_overlay();
    renderer.render({},[](unsigned){});
    check_frame(renderer.readback_bgra()!=shown,"Hidden split dashboards still visible");
}

// At this size each of the 256 columns occupies three output pixels. The
// 224 scanlines fill 576 rows, with a 96-pixel pillarbox on either side.
void check_classic_raster(const std::vector<BYTE>& pixels) {
    constexpr int width=960,height=576,left=96;
    bool has_scene=false;
    for(int y=0;y<height;++y) {
        const auto row=pixels.data()+4*width*y;
        for(int x=0;x<width;++x) {
            const auto p=row+4*x;
            if(x<left || x>=width-left) {
                check_frame(p[0]==0 && p[1]==0 && p[2]==0,"Classic 4:3 border is stretched");
            } else {
                has_scene|=(p[0]|p[1]|p[2])!=0;
                if((x-left)%3)check_frame(std::equal(p,p+3,p-4),"Classic horizontal pixel was filtered");
            }
        }
        if(y && ((2*y+1)*224)/(2*height)==((2*y-1)*224)/(2*height))
            check_frame(std::equal(row,row+4*width,row-4*width),"Classic vertical pixel was filtered");
    }
    check_frame(has_scene,"Classic raster was blank");
}

void race_finish(const srf::Rom& rom,const std::filesystem::path& output) {
    constexpr auto course=srf::CourseId::easy_ride;
    constexpr auto car=srf::PlayerCar::four_wd;
    srf::NativeRace race(rom,car,course);
    srf::CourseScenery scene(rom,course);
    srf::CourseEnvironment sky(rom,course);
    srf::RaceAudioState audio{};
    const auto world=srf::extract_world(rom);
    auto road=presentation_surfaces(world,course,false);
    Renderer renderer(nullptr,nullptr,0,{RaceLook::classic,true});
    renderer.use_classic_frame(rom);
    renderer.set_course_colors(srf::generated::course_packages.front());
    renderer.load_textures(srf::extract_vehicle_sprites(rom,course));
    renderer.load_backdrop(srf::extract_course_backdrop(rom,course));
    renderer.load_world(world,0);
    std::deque<FinishFrame> replay;
    std::array<std::uint32_t,3> laps{};
    unsigned checkpoint_count=0,lap_count=0,last_split=0;
    std::optional<std::uint64_t> last_award;
    for(const auto input:read_inputs(std::filesystem::path(SRF_TEST_FIXTURES)/"easy_ride_clean_inputs.csv")) {
        const auto player=race.racers()[race.player_index()].body;
        const unsigned lap=race.completion().acknowledged_laps;
        srf::RaceAudioOutput events;
        race.step(srf::prepare_player_input(audio,player,input.buttons,race.countdown_active(),events),input.ticks);
        scene.step(player,lap);sky.advance_video(input.ticks);sky.step(player,lap,race.elapsed_centiseconds());
        if(race.completion().acknowledged_laps!=lap) {
            laps.at(lap)=race.elapsed_centiseconds()-last_split;
            last_split=race.elapsed_centiseconds();
        }
        if(race.completion().acknowledged_laps>=2) {
            if(replay.size()==56)replay.pop_front();
            replay.emplace_back(race,scene,sky);
        }
        const auto award=race.time_award();
        if(award && last_award!=award->phase) {
            last_award=award->phase;
            if(award->checkpoint)++checkpoint_count;else ++lap_count;
            check_frame(award->seconds>0,"Time extension has no value");
            if(checkpoint_count==1 && award->checkpoint && !output.empty()) {
                const auto cars=vehicle_poses(rom,race.racers(),race.racers(),race.player_index(),1);
                chase_camera(renderer,cars[race.player_index()],lerp_angle(player.word(0x24),player.word(0x24),1),{},course);
                renderer.load_race_world(world,cars,rom,race,scene);
                srf::GridCountdownArt art(rom,course,car);std::vector<std::uint32_t> pixels(208*128);
                draw_driving_hud(renderer,art,pixels,race,0,true);
                renderer.render();renderer.screenshot(output/"checkpoint.png");
            }
        }
    }
    std::cout<<"Checkpoint awards "<<checkpoint_count<<", lap awards "<<lap_count<<'\n';
    check_frame(checkpoint_count==3 && lap_count==2 && race.completion().finished,"Missing timed race events");
    const auto result=srf::championship_result(race);
    renderer.resize(960,576);
    check_frame(last_split==result.elapsed_centiseconds && result.remaining_seconds==58,"Recorded finish changed");
    SpeedFinishArt art(rom,course);
    srf::BonusTallyArt lettering(rom);
    srf::BonusDashboard dashboard(rom,car);
    const auto& body=race.racers()[race.player_index()].body;
    const auto dash=dashboard.draw(std::int16_t(body.word(0x4a)),body.word(0x82),2,result.place);
    std::vector<std::uint32_t> overlay(208*128);
    std::vector<BYTE> previous;
    renderer.grid_sweep=true;
    for(auto [tick,name]:{std::pair{45u,"goal.png"},{goal_ticks+40,"extra-time.png"},{board_tick-1,"replay-end.png"}}) {
        draw_finish_scene(renderer,rom,world,road,replay,race.player_index(),RaceLook::classic,result,true,tick,art,lettering,overlay,dash);
        const auto frame=renderer.readback_bgra();
        if(tick==45)check_classic_raster(frame);
        check_frame(frame!=previous,"Finish presentation failed to advance");previous=frame;
        if(!output.empty())renderer.screenshot(output/name);
    }
    srf::Championship championship(srf::SpeedClass::novice,car,"AAA");
    auto first=art.draw(championship,result,laps,{},0,dash);
    const std::vector<std::uint32_t> initial(first.begin(),first.end());
    auto tallied=art.draw(championship,result,laps,{},90,dash);
    check_frame(!std::equal(initial.begin(),initial.end(),tallied.begin()),"Lap tally never changed");
    renderer.render_course_map(tallied);
    if(!output.empty())renderer.screenshot(output/"lap-total.png");
    check_frame(race.remaining_seconds()==58,"Presentation changed carry time");
    race.reset();check_frame(!race.time_award(),"Restart retained a checkpoint banner");
}
}

int wmain(int argc,wchar_t** argv) {
    if(!_wgetenv(L"SRF_ROM"))return 77;
    try {
        check(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"Capture COM");
        const auto rom=srf::Rom::load(_wgetenv(L"SRF_ROM"));
        const auto output=argc==2?std::filesystem::path(argv[1]):std::filesystem::path{};
        if(!output.empty())std::filesystem::create_directories(output);
        controls.width=640;controls.height=480;controls.world=controls.race=true;
        trailer(rom,RaceLook::n64,output);trailer(rom,RaceLook::remastered,output);
        split_dashboards(rom);
        race_finish(rom,output);
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
