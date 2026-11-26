namespace {
struct FinishFrame {
    std::array<srf::Vehicle,4> vehicles;
    srf::EnvironmentPresentation environment;
    std::vector<srf::SceneryPose> scenery,rocks,props,bridges;
    std::vector<srf::ScenerySprite> sprites;
    std::uint64_t phase;
    RivalWheelTravel wheel_travel;

    FinishFrame(const srf::NativeRace& race,const srf::CourseScenery& scene,const srf::CourseEnvironment& sky,const RivalWheelTravel& wheels={})
        :vehicles(race.racers()),environment(sky.presentation()),scenery(scene.poses().begin(),scene.poses().end()),
         rocks(race.rock_poses().begin(),race.rock_poses().end()),bridges(race.bridge_poses().begin(),race.bridge_poses().end()),
         sprites(scene.sprites().begin(),scene.sprites().end()),phase(race.phase()),wheel_travel(wheels) {
        for(auto batch:{race.cone_poses(),race.prop_poses(),race.pickup_poses(),race.bonus_poses()})
            props.insert(props.end(),batch.begin(),batch.end());
        for(auto batch:{race.breakable_sprites(),race.spray_sprites()})
            sprites.insert(sprites.end(),batch.begin(),batch.end());
    }
};

class SpeedFinishArt : public srf::RaceResultsArt {
    std::vector<std::uint8_t> clock_tiles_;
    std::array<std::uint32_t,16> clock_palette_;
    static std::string time(unsigned cs) {
        return std::to_string(cs/6000)+"'"+two(cs/100%60)+"\""+two(cs%100);
    }
public:
    SpeedFinishArt(const srf::Rom& rom,srf::CourseId course):RaceResultsArt(rom),
        clock_tiles_(srf::extract_bonus_hud_tiles(rom)),
        clock_palette_(srf::bonus_hud_palette(rom,course==srf::CourseId::harbor_city?0xf8760:0xf8960)) {}
    void finish_hud(std::span<std::uint32_t> overlay,const srf::RoundResult& result,bool replay) const {
        srf::draw_race_stopwatch(overlay,148,result.elapsed_centiseconds,clock_tiles_,clock_palette_);
        if(replay) {
            const std::string label="POS "+std::to_string(result.place);
            int x=4;
            for(char c:label) {
                if(c!=' ')srf::draw_bonus_hud_tile(overlay,208,x,114,font_,c<='9'?c-'0':10+c-'A',colors_[2]);
                x+=8;
            }
        }
    }
    std::span<const std::uint32_t> draw(const srf::Championship* session,const srf::RoundResult& result,
        const std::array<std::uint32_t,3>& laps,std::optional<unsigned> best,unsigned tick,
        std::span<const std::uint32_t> dash,std::string_view record_notice) {
        course_header(srf::course_definition(result.course).name,session?session->initials():"---");
        unsigned sum=0;
        for(unsigned i=0;i<laps.size();++i) {
            const unsigned elapsed=tick>30*i?std::min(30u,tick-30*i):0;
            const auto counted=laps[i]*elapsed/30;
            sum+=counted;
            constexpr unsigned colors[]{0,2,3};
            text("LAP "+std::to_string(i+1),32,74+16*i,colors[i]);
            text(elapsed?time(counted):"-'--\"--",80,74+16*i,colors[i]);
        }
        text("TOTAL",163,74,0);
        text(time(tick>=90?result.elapsed_centiseconds:sum),158,86,0);
        text(session?"BEST":"BEST LAP",session?166:153,105,2);
        text(best?time(*best):"-'--\"--",158,117,2);
        if(tick>=90) {
            if(!record_notice.empty())text(std::string(record_notice),32,65,2);
            if(session && session->outcome()==srf::ChampionshipOutcome::bonus)text("START BONUS  SELECT SKIP",32,144,0);
            else text("PRESS START",84,144,0);
        }
        std::copy(dash.begin(),dash.end(),pixels_.begin()+168*256);
        return pixels_;
    }
};

constexpr unsigned goal_ticks=74,replay_ticks=224,board_tick=goal_ticks+replay_ticks;

void draw_finish_scene(Renderer& renderer,const srf::Rom& rom,const srf::World& world,PresentationRoad& road,
    const std::deque<FinishFrame>& frames,std::size_t player,RaceLook look,const srf::RoundResult& result,
    bool carry,double ticks,const SpeedFinishArt& results,const srf::BonusTallyArt& lettering,
    std::span<std::uint32_t> overlay,std::span<const std::uint32_t> dash) {
    const unsigned tick=unsigned(ticks);
    const unsigned bank=std::min<unsigned>(99,result.remaining_seconds);
    const bool replay=tick>=goal_ticks;
    const double pos=replay?std::min(1.,(ticks-goal_ticks)/(replay_ticks-1))*(frames.size()-1):frames.size()-1;
    const auto index=std::size_t(pos);
    const auto& frame=frames[index];
    const auto& next=frames[std::min(index+1,frames.size()-1)];
    const float blend=float(pos-index);
    const auto surface=road.sample(frame.bridges);
    const auto& camera_car=frames[replay?frames.size()/2:frames.size()-1].vehicles[player].body;
    controls.yaw=lerp_angle(camera_car.word(0x24),camera_car.word(0x24),1)+(replay?.8f:0.f);
    constexpr float angle_units=65536.f/6.28318530718f;
    const auto sprite_yaw=static_cast<std::uint8_t>(-(static_cast<std::uint16_t>(std::lround(-controls.yaw*angle_units))>>8));
    auto cars=vehicle_poses(rom,frame.vehicles,next.vehicles,player,blend,true,
        look==RaceLook::remastered?&next.wheel_travel:nullptr,surface,look==RaceLook::n64,sprite_yaw);
    if(look==RaceLook::remastered)seat_opponents(cars,surface);
    auto target=cars[player].position;
    target[1]-=50;
    const auto center=renderer.normalize_world(target);
    controls.target_x=center[0];controls.target_y=center[1];controls.target_z=center[2];
    controls.pitch=replay?-.36f:-.2f;controls.zoom=2;
    controls.distance=renderer.normalized_distance(replay?1350.f:1000.f+float(tick)*6);
    renderer.set_environment(frame.environment);
    renderer.set_race_phase(float(frame.phase)+blend);
    renderer.load_world(world,0,cars,&rom,frame.scenery,frame.rocks,frame.props,frame.bridges,frame.sprites);
    std::fill(overlay.begin(),overlay.end(),0);
    if(!replay)lettering.draw_goal(overlay,tick);
    else if(carry) {
        const unsigned age=tick-goal_ticks;
        const auto transferred=std::min(bank,age/2);
        lettering.draw_finish_time(overlay,bank,transferred,age/3);
    }
    if(race_hud_visible)results.finish_hud(overlay,result,replay);
    renderer.set_bonus_overlay(overlay,15,1,replay?40.f:0.f,race_hud_visible?dash:std::span<const std::uint32_t>{},0,224,replay);
    renderer.render();
}

enum class FinishChoice { back,advance,skip_bonus,race_again };

// Only the final four seconds of race phases are retained. Playback samples those
// poses without advancing physics, course triggers, the championship or its clock.
FinishChoice show_speed_finish(GameHost& host,const srf::Rom& rom,const srf::Championship* session,srf::PlayerCar car,
    const srf::RoundResult& result,const std::array<std::uint32_t,3>& laps,std::optional<unsigned> best,std::optional<unsigned> prior_best,
    const std::deque<FinishFrame>& frames,const srf::World& world,PresentationRoad& road,
    std::size_t player,RaceLook look,const std::filesystem::path& profile,std::unique_ptr<srf::AudioDevice> audio) {
    if(frames.empty())throw std::logic_error("A finished race has no replay frames");
    auto& renderer=*host.presenter;
    BootWindowInput keys;
    GameScreen screen(host,L"Stunt Race FX — Goal",boot_window_proc,&keys);
    std::unique_ptr<srf::Gamepad> pad;
    try {pad=std::make_unique<srf::Gamepad>(profile);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
    SpeedFinishArt results(rom,result.course);
    srf::BonusTallyArt lettering(rom,result.course==srf::CourseId::harbor_city?0xf8760:0xf8960);
    srf::BonusDashboard dashboard(rom,car);
    const auto& body=frames.back().vehicles[player].body;
    const auto dash=dashboard.draw(std::int16_t(body.word(0x4a)),body.word(0x82),session?session->credits():0,result.place);
    std::vector<std::uint32_t> overlay(208*128);
    const bool carry=session && result.place<=3 && session->round()<4;
    std::string notice;
    if(session && best==result.elapsed_centiseconds) {
        if(!prior_best) notice="FIRST RECORDED TIME";
        else if(*best<*prior_best) {
            const unsigned gain=*prior_best-*best;
            notice="NEW BEST  "+std::to_string(gain/100)+"\""+
                   (gain%100<10?"0":"")+std::to_string(gain%100)+" FASTER";
        }
    }
    auto last=std::chrono::steady_clock::now();
    double ticks=0;
    unsigned held_before=keys.held()|(pad?pad->poll_frontend(game_focused(screen.window)):0);
    bool board=false,audio_paused=false;
    renderer.grid_sweep=true;
    while(!game_closing && !keys.quit) {
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {
            if(msg.message==WM_QUIT)game_closing=true;
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        const bool focused=game_focused(screen.window);
        if(audio && audio_paused!=!focused) {
            audio_paused=!focused;
            try {audio->pause(audio_paused);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';audio.reset();}
        }
        const unsigned held=keys.held()|(pad?pad->poll_frontend(focused):0);
        const auto pressed=held&~held_before;held_before=held;
        if(focused && !session && keys.key('R'))return FinishChoice::race_again;
        if(focused && (keys.escape || (pressed&0x4000)))return FinishChoice::back;
        const auto now=std::chrono::steady_clock::now();
        if(focused)ticks+=60.0988*std::min(.16,std::chrono::duration<double>(now-last).count());
        last=now;
        if(focused && (pressed&0x2000) && ticks>=board_tick && session && session->outcome()==srf::ChampionshipOutcome::bonus)
            return FinishChoice::skip_bonus;
        if(focused && (pressed&0x9080)) {
            if(ticks<goal_ticks)ticks=goal_ticks;
            else if(ticks<board_tick)ticks=board_tick;
            else if(ticks<board_tick+90)ticks=board_tick+90;
            else return FinishChoice::advance;
        }
        const auto tick=unsigned(ticks);
        if(tick>=board_tick) {
            if(!board) {
                const auto outcome=session?session->outcome():srf::ChampionshipOutcome::racing;
                const wchar_t* action=!session?L"Quick Race":outcome==srf::ChampionshipOutcome::bonus?L"Bonus Trax":
                    outcome==srf::ChampionshipOutcome::class_complete?L"Class results":
                    outcome==srf::ChampionshipOutcome::retry?L"Use spare and retry":L"Next race";
                std::wstring caption=L"Stunt Race FX \u2014 RACE RESULTS";
                if(!notice.empty())caption+=L" \u2014 "+std::wstring(notice.begin(),notice.end());
                caption+=std::wstring(L" \u2014 ")+action;
                SetWindowTextW(screen.window,caption.c_str());
                audio.reset();renderer.clear_bonus_overlay();board=true;
            }
            renderer.render_course_map(results.draw(session,result,laps,best,tick-board_tick,dash,look==RaceLook::classic && !game_xr?std::string_view{}:notice));
            continue;
        }
        draw_finish_scene(renderer,rom,world,road,frames,player,look,result,carry,ticks,results,lettering,overlay,dash);
    }
    return FinishChoice::back;
}
}
