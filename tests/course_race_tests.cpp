#include "srf/native_race.hpp"
#include "srf/championship.hpp"
#include "srf/speed_bonus.hpp"
#include "srf/bonus_tally.hpp"
#include "srf/grid_countdown.hpp"
#include "srf/race_audio.hpp"
#include "srf/scenery.hpp"
#include "srf/environment.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace {
void require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
bool same(const std::array<srf::Vehicle,4>& a,const std::array<srf::Vehicle,4>& b) {
    for(unsigned i=0;i<4;++i) {
        if(a[i].body.words!=b[i].body.words)return false;
        for(unsigned j=0;j<4;++j)if(a[i].wheels[j].words!=b[i].wheels[j].words)return false;
    }
    return true;
}
struct Input { std::uint16_t buttons; std::uint8_t ticks; };
std::vector<Input> load(const std::filesystem::path& path) {
    std::ifstream file(path);std::string line;
    require(bool(std::getline(file,line)),"Missing race fixture");
    if(!line.empty() && line.back()=='\r')line.pop_back();
    require(line=="phase,requested,clock_ticks","Fixture must contain requested buttons only");
    std::vector<Input> inputs;
    while(std::getline(file,line)) {
        unsigned phase,buttons,ticks;char a,b;std::istringstream row(line);
        require(bool(row>>phase>>a>>buttons>>b>>ticks),"Malformed input fixture");
        require(phase==inputs.size() && a==',' && b==',' && !(buttons&~0x473u),"Invalid button input");
        // The replay uses the desktop clock policy (60.0988 video ticks / 14 phases).
        const auto tick_total=[](std::uint64_t p){return p*600988/140000;};
        require(ticks==tick_total(inputs.size()+1)-tick_total(inputs.size()),"Fixture changed the race clock");
        row>>std::ws;require(row.eof(),"Trailing input fields");
        inputs.push_back({static_cast<std::uint16_t>(buttons),static_cast<std::uint8_t>(ticks)});
    }
    require(!inputs.empty(),"Empty input fixture");return inputs;
}
void championship_grid_and_retry(const srf::Rom& rom) {
    using namespace srf;
    ChampionshipProgress progress;
    Championship session(SpeedClass::novice, PlayerCar::four_wd);
    NativeRace first(rom, session.car(), session.course(), session.carry_seconds());
    RaceAudioState audio{};
    for (const auto input : load(std::filesystem::path(SRF_TEST_FIXTURES) / "easy_ride_clean_inputs.csv")) {
        RaceAudioOutput events;
        first.step(prepare_player_input(audio, first.racers()[NativeRace::player_index()].body,
            input.buttons, first.countdown_active(), events), input.ticks);
    }
    const auto result = championship_result(first);
    require(first.completion().finished && result.place == 3 && result.remaining_seconds > 0 &&
        session.finish(result, progress) == ChampionshipOutcome::next_race, "Easy Ride failed to qualify");
    session.advance();
    require(session.course() == CourseId::aqua_tunnel && session.carry_seconds() == result.remaining_seconds,
        "Qualified time did not carry into Aqua Tunnel");
    NativeRace second(rom, session.car(), session.course(), session.carry_seconds());
    const auto grid = second.remaining_seconds();
    require(grid == 50 + result.remaining_seconds, "4WD Aqua Tunnel grid lost ROM grant or carry");
    for (const auto car : {PlayerCar::f_type, PlayerCar::coupe}) {
        NativeRace alternate(rom, car, session.course(), session.carry_seconds());
        require(alternate.remaining_seconds() == 40 + result.remaining_seconds, "Aqua Tunnel ignored car-specific grant");
    }
    for (unsigned phase = 0; phase < 3000 && !second.ended(); ++phase) second.step(0, 4);
    require(second.failure() == RaceFailure::time_out && !second.remaining_seconds() &&
        session.finish(championship_result(second), progress) == ChampionshipOutcome::retry,
        "Aqua Tunnel time out did not offer retry");
    session.advance();
    NativeRace retry(rom, session.car(), session.course(), session.carry_seconds());
    require(session.round() == 2 && session.credits() == 1 && retry.remaining_seconds() == grid &&
        session.total_centiseconds() == result.elapsed_centiseconds, "Retry lost entry time or changed saved race total");

    audio = {};
    for (const auto input : load(std::filesystem::path(SRF_TEST_FIXTURES) / "aqua_tunnel_championship_inputs.csv")) {
        RaceAudioOutput events;
        retry.step(prepare_player_input(audio, retry.racers()[NativeRace::player_index()].body,
            input.buttons, retry.countdown_active(), events), input.ticks);
    }
    const auto second_result = championship_result(retry);
    require(session.finish(second_result, progress) == ChampionshipOutcome::bonus &&
        second_result.remaining_seconds > 0, "Qualified Aqua Tunnel did not offer the trailer bonus");
    SpeedBonus bonus(rom, session.league());
    for (unsigned phase = 0; phase < 1000 && !bonus.ended(); ++phase) bonus.step(0x10, 4);
    const auto earned = bonus.reward();
    require(bonus.ended() && earned.extra_seconds && earned.extra_cars, "Trailer replay did not earn its awards");
    session.finish_bonus(earned.extra_seconds, earned.extra_cars);
    NativeRace third(rom, session.car(), session.course(), session.carry_seconds(), session.starting_grid());
    require(session.starting_grid() == retry.standings() && third.standings() == retry.standings(),
        "Race two's four finishers did not take their places on the race-three grid");
    require(session.course() == CourseId::sunset_valley && session.credits() == 1 + earned.extra_cars &&
        third.remaining_seconds() == 50 + std::min(99u, second_result.remaining_seconds + earned.extra_seconds),
        "Race three lost incoming time or trailer awards");
}

void bonus_tally_grid_and_retry(const srf::Rom& rom) {
    using namespace srf;
    struct Run {
        SpeedClass league;
        PlayerCar car;
        CourseId third;
        std::uint16_t incoming,flags,laps,carry,grid;
        BonusTallyStart start=BonusTallyStart::moving;
    };
    constexpr Run cases[]{
        {SpeedClass::novice, PlayerCar::four_wd, CourseId::sunset_valley, 83,2,1,85,135},
        {SpeedClass::expert, PlayerCar::coupe, CourseId::white_land, 87,3,2,90,130},
        {SpeedClass::master, PlayerCar::f_type, CourseId::sky_ramp, 93,3,1,96,146},
        {SpeedClass::master, PlayerCar::f_type, CourseId::sky_ramp, 136,3,1,99,149},
        {SpeedClass::novice, PlayerCar::four_wd, CourseId::sunset_valley, 83,0,0,83,133,BonusTallyStart::parked},
        {SpeedClass::expert, PlayerCar::coupe, CourseId::white_land, 87,0,0,87,127,BonusTallyStart::parked},
        {SpeedClass::master, PlayerCar::f_type, CourseId::sky_ramp, 136,0,0,99,149,BonusTallyStart::parked}
    };
    for(const auto& run:cases) {
        ChampionshipProgress progress;
        Championship session(run.league,run.car);
        session.finish({session.course(),RaceFailure::none,2,20,10000},progress);
        session.advance();
        constexpr GridOrder finishes[]{{1,4,3,2},{2,4,1,3},{4,2,3,1}};
        const auto& places=finishes[static_cast<unsigned>(run.league)];
        session.finish({session.course(),RaceFailure::none,places[NativeRace::player_index()],
                        run.incoming,11000,false,places},progress);
        BonusTally tally(run.league,run.flags,run.laps,session.results().back().remaining_seconds,0,run.start,run.car);
        while(!tally.finished())tally.advance();
        session.finish_bonus(run.flags,run.laps);
        require(tally.frame().bank_seconds==run.carry && session.carry_seconds()==run.carry &&
                session.course()==run.third && session.credits()==2u+run.laps,"Results and race-three awards disagree");
        NativeRace third(rom,run.car,session.course(),session.carry_seconds(),session.starting_grid());
        require(third.remaining_seconds()==run.grid,"Incorrect clock at the race-three grid");
        const auto on_grid=third.racers();
        for(unsigned phase=0;phase<160;++phase)third.step(0,4);
        require(third.standings()!=places,"Retirement fixture did not change the live standings");
        session.finish(championship_result(third,true),progress);
        session.advance();
        NativeRace retry(rom,run.car,session.course(),session.carry_seconds(),session.starting_grid());
        require(retry.standings()==places && same(retry.racers(),on_grid),
                "A failed race replaced the four-car entry lineup");
        require(retry.remaining_seconds()==run.grid && session.credits()==1u+run.laps,
                "Race-three retry changed the clock or spent the wrong number of spares");
    }
}

void race_three_time_over(const srf::Rom& rom) {
    using namespace srf;
    constexpr GridOrder grid{2,4,3,1};
    for(const bool moving : {false,true}) {
        NativeRace race(rom,PlayerCar::four_wd,CourseId::sunset_valley,0,grid);
        while(!race.time_over_pending() && !race.ended() && race.phase()<1000) {
            race.step(moving && race.remaining_seconds()<=3 ? 0x10 : 0);
        }
        require(race.time_over_pending() && !race.remaining_seconds(),"Time Over skipped its coasting update");
        const auto entry=race.phase();
        const auto clock=race.elapsed_centiseconds();
        const auto& player=race.racers()[NativeRace::player_index()].body;
        require(!moving || std::abs(int(static_cast<std::int16_t>(player.word(0x4a))))>=69,
                "Moving timeout fixture reached zero below the coasting threshold");
        while(!race.ended() && race.phase()-entry<=32) {
            const auto phase=race.phase();
            const auto places=race.standings();
            const bool slow=std::abs(int(static_cast<std::int16_t>(player.word(0x4a))))<69;
            race.step(0x410);
            require((player.word(0x42)&0x430)==0,"Time Over accepted throttle, reverse or boost");
            if(slow || phase-entry==31)
                require(race.ended() && race.phase()==phase && race.standings()==places,
                        "Time Over failed to latch standings before the next vehicle update");
            else require(!race.ended() && race.phase()==phase+1,"Time Over ended before its cutoff");
        }
        require(!race.time_over_pending() && race.failure()==RaceFailure::time_out &&
                race.elapsed_centiseconds()>clock && (race.phase()>entry)==moving,
                "Moving and parked Time Over resolved at the wrong point");
        race.reset();
        require(race.standings()==grid && !race.ended() && !race.time_over_pending(),
                "A reset retained a pending or completed timeout");
    }
}

void grid_signal_poses(const srf::Rom& rom) {
    using namespace srf;
    struct Grid {
        CourseId course;
        PlayerCar car;
        GridOrder places;
        unsigned first_y,release_phase;
        std::array<std::array<unsigned,4>,4> at_release; // X, Y, Z and yaw at 07:E506
    };
    constexpr Grid cases[]{
        {CourseId::sunset_valley,PlayerCar::four_wd,{1,4,3,2},65280,119,
            {{{57828,65502,38816,15196},{60467,65409,37846,15224},{59680,4,39133,14980},{58848,65376,38208,16384}}}},
        {CourseId::white_land,PlayerCar::coupe,{1,4,3,2},65078,117,
            {{{2982,65502,58897,64096},{2018,65409,56140,64376},{3299,4,57149,64068},{2432,65521,58528,0}}}},
        {CourseId::sky_ramp,PlayerCar::f_type,{2,3,4,1},63654,115,
            {{{57321,63966,257,64128},{58277,63874,64796,64380},{56999,64003,63884,64096},{58048,63954,1472,0}}}}
    };
    for(unsigned league=0;league<3;++league) {
        const auto& c=cases[league];
        NativeRace race(rom,c.car,c.course,85,c.places);
        const auto full_clock=race.remaining_seconds();
        GridCountdown grid(league,unsigned(c.car),full_clock-85,85,2,c.places);
        for(;;grid.advance()) {
            const auto& frame=grid.frame();
            while(race.phase()+1<frame.scene_phase)race.step(0x10,0);
            if(!frame.tick)require(race.racers()[0].body.word(0x20)==c.first_y,"Incorrect first visible grid height");
            if(frame.driving)break;
        }
        require(race.phase()+1==c.release_phase && race.standings()==c.places &&
                !race.elapsed_centiseconds() && race.remaining_seconds()==full_clock,"Grid released at the wrong phase");
        for(unsigned i=0;i<4;++i)for(unsigned axis=0;axis<4;++axis)
            require(race.racers()[i].body.word(0x1e + axis*2)==c.at_release[i][axis],"Incorrect car pose at green");
        race.release_start_signal();
        race.step(0x10,4);
        require(!race.countdown_active() && race.elapsed_centiseconds() && (race.racers()[3].body.word(0x42)&0x10),
                "Grid release did not start driving and the race clock");
    }
}

void harbor_prop_impacts(const srf::Rom& rom) {
    const auto inputs=load(std::filesystem::path(SRF_TEST_FIXTURES)/"harbor_city_clean_inputs.csv");
    srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::harbor_city);
    for(const bool cone:{true,false}) {
        race.reset();srf::RaceAudioState audio{};
        const unsigned start=cone?175:455, duration=cone?10:1, impact=cone?226:483;
        const std::uint32_t id=cone?0x6b10e:0x6b45e;
        std::vector<unsigned> sounds;
        for(unsigned phase=0;phase<impact+6;++phase) {
            const auto input=inputs.at(phase);
            const auto buttons=phase>=start && phase<start+duration ? (cone?0x412:0x11) : input.buttons;
            srf::RaceAudioOutput events;
            race.step(srf::prepare_player_input(audio,race.racers()[3].body,buttons,race.countdown_active(),events),input.ticks);
            for(const auto sound:race.prop_effects()) {
                require(sound==(cone?0x54:0x59),"Unexpected Harbor City impact sound");
                sounds.push_back(race.phase());
            }
            if(race.phase()<impact)continue;
            const auto pose=std::ranges::find(race.prop_poses(),id,&srf::SceneryPose::command_offset);
            require(pose!=race.prop_poses().end(),"Missing struck Harbor City prop");
            const auto spin=race.phase()-impact;
            require(pose->yaw==(cone?128:192)+6*spin && pose->pitch==(cone?10*spin:0) &&
                    pose->roll==pose->pitch,"Harbor City prop spin axes or timing changed");
        }
        require(sounds==(cone?std::vector<unsigned>{222,226}:std::vector<unsigned>{483}),
                "Harbor City prop did not respond to ordinary steering input");
    }
}
void lake_side_route_sentinel(const srf::Rom& rom) {
    const auto route=srf::extract_route(srf::extract_course_scene(rom,srf::CourseId::lake_side));
    const auto count=static_cast<std::uint16_t>(route.size());
    // These two positions occurred under ordinary controls when the car left the course.
    for(const srf::RacePosition position: {srf::RacePosition{-13743,-153,-31474},
                                           srf::RacePosition{-27273,-159,-21388}}) {
        require(!srf::nearest_route_point(route,position),"Expected original off-course route sentinel");
    }
    srf::RouteProgress p{0,57,1,0};
    require(!srf::update_route_progress(p,count) && p.accepted_index==57 && p.laps==1,
            "Leaving the course must preserve accepted progress");
    p.accepted_index=count;
    require(srf::update_route_progress(p,count) && p.accepted_index==0 && p.laps==1,
            "Original final checkpoint did not accept sentinel on wrap");
    require(srf::route_next_distance(route,0,0,route.front().position)==0,
            "Forward route from the zero sentinel did not target the first point");
    p.current_index=1;
    require(srf::update_route_progress(p,count) && p.laps==2,"Return to the finish line did not count");
}
void night_cruise_flyover(const srf::Rom& rom) {
    const auto inputs=load(std::filesystem::path(SRF_TEST_FIXTURES)/"night_cruise_flyover_inputs.csv");
    srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::night_cruise);
    std::vector<std::vector<srf::SceneryPose>> original_run;
    for(unsigned run=0;run<2;++run) {
        race.reset();srf::RaceAudioState audio;
        std::vector<unsigned> sounds;
        for(auto input:inputs) {
            srf::RaceAudioOutput events;
            const auto before=race.racers()[3].body.word(0x6a);
            const auto controls=srf::prepare_player_input(audio,race.racers()[3].body,input.buttons,race.countdown_active(),events);
            race.step(controls,input.ticks);
            const auto phase=race.phase();
            const auto poses=race.bonus_poses();
            if(!run)original_run.emplace_back(poses.begin(),poses.end());
            else require(std::ranges::equal(original_run.at(phase-1),poses),"Night Cruise flyover changed after restart");
            const auto visible=[&](unsigned model){return std::ranges::any_of(poses,[&](const auto& p){
                return p.visible && p.visual_descriptor==model;
            });};
            require(visible(0xaa34)==(phase>=226 && phase<=266),"Wrong aircraft visibility");
            require(visible(0xb7a8)==(phase>=236 && phase<=247),"Wrong dropped item visibility");
            for(auto sound:race.bonus_effects()) {
                require((phase==226 && sound==0x0e) || (phase==239 && sound==0x25),"Unexpected flyover sound");
                sounds.push_back(sound);
            }
            if(phase==239)require(before==224 && race.racers()[3].body.word(0x6a)==382,"Flyover missed boost award");
        }
        require(inputs.size()==270 && race.phase()==270 && sounds==std::vector<unsigned>{0x0e,0x25},
                "Incomplete Night Cruise flyover replay");
    }
}

void starting_slot_permutations(const srf::Rom& rom) {
    using namespace srf;
    // Original Sunset Valley scene commands and 08:B8B4 allocation. Vehicle
    // identity, color and linked-list index stay fixed when an entrant moves.
    constexpr std::array<std::array<int,2>,4> slots{{{-7488,-26688},{-6688,-27328},
                                                     {-5728,-26368},{-4768,-27648}}};
    constexpr unsigned definitions[]{0x876e,0x8990,0x8aa8,0x8411},colors[]{0,2,3,1};
    GridOrder order{1,2,3,4};
    do {
        NativeRace race(rom,PlayerCar::four_wd,CourseId::sunset_valley,85,order);
        const auto initial=race.racers();
        for(unsigned i=0;i<initial.size();++i) {
            const auto& body=initial[i].body;
            const auto& slot=slots[order[i]-1];
            require(body.word(0x1e)==std::uint16_t(slot[0]) && body.word(0x22)==std::uint16_t(slot[1]) &&
                    body.word(0x24)==0x4000 && body.word(0x66)==definitions[i] && body.word(0x96)==colors[i],
                    "Starting slot changed actor identity or used another pose");
        }
        for(unsigned phase=0;phase<4;++phase) {
            require(race.standings()==order,"Grid rank flickered during initialization");
            race.step(0);
        }
        race.reset();
        require(race.standings()==order && same(initial,race.racers()),"Restart lost its grid order");
    } while(std::next_permutation(order.begin(),order.end()));
    for(const auto bad:{GridOrder{0,1,2,3},GridOrder{2,2,3,4},GridOrder{1,2,3,5}}) {
        bool rejected=false;
        try{NativeRace race(rom,PlayerCar::four_wd,CourseId::sunset_valley,85,bad);}
        catch(const std::invalid_argument&){rejected=true;}
        require(rejected,"Invalid grid permutation accepted");
    }
}

void fourth_course_slots(const srf::Rom& rom) {
    using namespace srf;
    struct Grid {
        CourseId course;
        PlayerCar car;
        unsigned grant, player_definition;
        std::array<unsigned,4> colors;
        std::array<std::array<unsigned,2>,4> slots; // Cartridge X/Z by saved place.
    };
    constexpr Grid grids[]{
        {CourseId::night_owl,PlayerCar::four_wd,60,33809,{0,2,3,1},
            {{{22272,16832},{21632,16032},{22592,15072},{21312,14112}}}},
        {CourseId::night_cruise,PlayerCar::coupe,40,34109,{0,1,2,3},
            {{{56128,3392},{55488,2592},{56448,1632},{55168,672}}}},
        {CourseId::harbor_city,PlayerCar::f_type,50,33209,{2,1,3,0},
            {{{61888,5312},{61248,4512},{62208,3552},{60928,2592}}}}
    };
    for(const auto& grid:grids) {
        const std::array<unsigned,4> definitions{0x876e,0x8990,0x8aa8,grid.player_definition};
        GridOrder order{1,2,3,4};
        do {
            NativeRace race(rom,grid.car,grid.course,85,order);
            require(race.remaining_seconds()==grid.grant+85,"Race four lost the car-specific grant or bank");
            for(unsigned i=0;i<4;++i) {
                const auto& body=race.racers()[i].body;
                const auto& slot=grid.slots[order[i]-1];
                require(body.word(0x1e)==slot[0] && body.word(0x22)==slot[1] && body.word(0x24)==0 &&
                        body.word(0x66)==definitions[i] && body.word(0x96)==grid.colors[i],
                        "Fourth grid changed an entrant or assigned the wrong physical slot");
            }
        } while(std::next_permutation(order.begin(),order.end()));
    }
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if(!path) {std::cout<<"Set SRF_ROM to the supplied ROM\n";return 77;}
        const auto rom=srf::Rom::load(path);
        starting_slot_permutations(rom);
        fourth_course_slots(rom);
        race_three_time_over(rom);
        grid_signal_poses(rom);
        std::vector<std::pair<srf::CourseId,srf::PlayerCar>> replays;
        for(const auto& course:srf::courses)replays.emplace_back(course.id,srf::PlayerCar::four_wd);
        for(auto course:{srf::CourseId::sky_ramp,srf::CourseId::harbor_city})
            for(auto car:{srf::PlayerCar::coupe,srf::PlayerCar::f_type})replays.emplace_back(course,car);
        for(const auto& [id,car]:replays) {
            const auto& course=srf::course_definition(id);
            std::string filename(course.slug);
            std::replace(filename.begin(),filename.end(),'-','_');
            filename+=car==srf::PlayerCar::coupe?"_coupe_inputs.csv":
                      car==srf::PlayerCar::f_type?"_f_type_inputs.csv":"_clean_inputs.csv";
            const auto inputs=load(std::filesystem::path(SRF_TEST_FIXTURES)/filename);
            const bool clean_four_car_finish=course.id==srf::CourseId::sunset_valley ||
                                             course.id==srf::CourseId::night_owl ||
                                             course.id==srf::CourseId::kings_forest ||
                                             course.id==srf::CourseId::sea_breeze ||
                                             course.id==srf::CourseId::white_land ||
                                             course.id==srf::CourseId::night_cruise ||
                                             course.id==srf::CourseId::lake_side ||
                                             course.id==srf::CourseId::big_ravine ||
                                             course.id==srf::CourseId::sky_ramp ||
                                             course.id==srf::CourseId::harbor_city;
            srf::NativeRace race(rom,car,course.id);
            srf::CourseScenery scenery(rom,course.id);
            srf::CourseEnvironment environment(rom,course.id);
            using ContentFrame=std::tuple<std::vector<srf::SceneryPose>,srf::EnvironmentState,
                std::vector<std::uint8_t>,std::vector<std::uint8_t>,std::vector<std::uint8_t>,
                srf::EnvironmentVideoState,srf::EnvironmentPresentation,
                std::vector<srf::SceneryPose>,std::vector<std::uint8_t>,
                std::vector<srf::SceneryPose>,std::vector<std::uint8_t>,std::vector<srf::SceneryPose>,
                std::vector<srf::SceneryPose>,std::vector<std::uint8_t>,
                std::vector<srf::SceneryPose>,std::vector<std::uint8_t>,
                std::vector<srf::ScenerySprite>,std::vector<std::uint8_t>,
                std::vector<srf::SceneryPose>,std::vector<std::uint8_t>,std::vector<srf::ScenerySprite>>;
            const auto initial=race.racers();
            std::vector<std::array<srf::Vehicle,4>> first_states;
            first_states.reserve(inputs.size());
            const auto drive=[&](bool replay) {
                srf::RaceAudioState audio;
                std::vector<ContentFrame> content;
                unsigned previous_lap=0,lap_events=0;
                for(const auto input:inputs) {
                    require(!race.ended(),"Fixture continued after race ended");
                    srf::RaceAudioOutput events;
                    const auto player=race.racers()[3].body;
                    const auto acknowledged_laps=race.completion().acknowledged_laps;
                    race.step(srf::prepare_player_input(audio,race.racers()[3].body,input.buttons,race.countdown_active(),events),input.ticks);
                    if(replay)require(same(first_states.at(content.size()),race.racers()),"Racer or wheel state changed after restart");
                    else first_states.push_back(race.racers());
                    if(clean_four_car_finish)
                        require(race.racers()[3].body.word(0x82)==0,"Clean fixture took damage before finish");
                    scenery.step(player,acknowledged_laps);environment.advance_video(input.ticks);environment.step(player,acknowledged_laps);
                    if(course.id==srf::CourseId::sunset_valley && (environment.state().flags&0x20))
                        require(acknowledged_laps==1,"Sunset Valley dusk started outside the second lap");
                    content.emplace_back(std::vector<srf::SceneryPose>(scenery.poses().begin(),scenery.poses().end()),
                        environment.state(),environment.events().sound_commands,environment.events().effects,
                        std::vector<std::uint8_t>(scenery.effects().begin(),scenery.effects().end()),
                        environment.video_state(),environment.presentation(),
                        std::vector<srf::SceneryPose>(race.rock_poses().begin(),race.rock_poses().end()),
                        std::vector<std::uint8_t>(race.rock_effects().begin(),race.rock_effects().end()),
                        std::vector<srf::SceneryPose>(race.cone_poses().begin(),race.cone_poses().end()),
                        std::vector<std::uint8_t>(race.cone_effects().begin(),race.cone_effects().end()),
                        std::vector<srf::SceneryPose>(race.bridge_poses().begin(),race.bridge_poses().end()),
                        std::vector<srf::SceneryPose>(race.prop_poses().begin(),race.prop_poses().end()),
                        std::vector<std::uint8_t>(race.prop_effects().begin(),race.prop_effects().end()),
                        std::vector<srf::SceneryPose>(race.pickup_poses().begin(),race.pickup_poses().end()),
                        std::vector<std::uint8_t>(race.pickup_effects().begin(),race.pickup_effects().end()),
                        std::vector<srf::ScenerySprite>(race.breakable_sprites().begin(),race.breakable_sprites().end()),
                        std::vector<std::uint8_t>(race.breakable_effects().begin(),race.breakable_effects().end()),
                        std::vector<srf::SceneryPose>(race.bonus_poses().begin(),race.bonus_poses().end()),
                        std::vector<std::uint8_t>(race.bonus_effects().begin(),race.bonus_effects().end()),
                        std::vector<srf::ScenerySprite>(race.spray_sprites().begin(),race.spray_sprites().end()));
                    if(!race.rock_poses().empty())
                        require(((course.id==srf::CourseId::sunset_valley || course.id==srf::CourseId::white_land) && acknowledged_laps==2) ||
                                (course.id==srf::CourseId::big_ravine && (acknowledged_laps==1 || acknowledged_laps==2)),
                                "Falling hazards appeared outside their original lap");
                    const auto lap=race.racers()[3].body.word(0x7a);
                    if(lap!=previous_lap) {require(lap==previous_lap+1,"Skipped a lap");++lap_events;previous_lap=lap;}
                }
                require(lap_events==3 && race.completion().finished && race.failure()==srf::RaceFailure::none,"Race did not finish three valid laps");
                require(race.phase()==inputs.size(),"Race stopped advancing before finish");
                require(race.racers()[3].body.word(0x82)==0,"Clean driving fixture took damage");
                for(unsigned i=0;i<3;++i)require(race.racers()[i].body.word(0x7a)>=2,"Opponent did not complete multiple laps");
                if(clean_four_car_finish)
                    for(unsigned i=0;i<3;++i)require(race.racers()[i].body.word(0x7a)==3,"Opponent did not complete three laps");
                return content;
            };
            const auto first_content=drive(false);const auto final=race.racers();const auto time=race.elapsed_centiseconds();
            const auto seconds=race.remaining_seconds();const auto standings=race.standings();
            race.reset();require(same(initial,race.racers()) && race.phase()==0 && !race.ended(),"Restart failed to restore starting grid");
            scenery.reset();environment.reset();
            const auto repeated_content=drive(true);
            require(first_content==repeated_content,"Course scenery or sound triggers differ after full-race restart");
            require(same(final,race.racers()) && time==race.elapsed_centiseconds() && seconds==race.remaining_seconds() && standings==race.standings(),"Complete race replay differs after restart");
            unsigned commands=0,effects=0,scenery_effects=0;
            for(const auto& frame:first_content){commands+=std::get<2>(frame).size();effects+=std::get<3>(frame).size();scenery_effects+=std::get<4>(frame).size();}
            if(course.id==srf::CourseId::aqua_tunnel)require(commands>=6 && effects>=3,"Aqua complete race missed tunnel/proximity triggers");
            else if(course.id==srf::CourseId::sunset_valley) {
                unsigned enter=0,leave=0,backdrop_switches=0,dusk_phases=0,crossing_sounds=0;
                std::array<bool,5> saw_running{},saw_jumping{};
                unsigned rock_spawns=0,rock_phases=0;
                bool alternate=false;
                for(const auto& frame:first_content) {
                    const auto& rocks=std::get<7>(frame);
                    const auto& rock_sounds=std::get<8>(frame);
                    rock_spawns+=std::count(rock_sounds.begin(),rock_sounds.end(),0x53);
                    require(std::count(rock_sounds.begin(),rock_sounds.end(),0x5a)==0,
                            "Clean Sunset driving fixture hit a rock");
                    if(!rocks.empty()) {
                        ++rock_phases;
                        require(rocks.size()==2 && std::all_of(rocks.begin(),rocks.end(),[](const auto& rock){
                            return rock.visible && rock.visual_descriptor==0xb4d0;
                        }),"Sunset rock model or count mismatch");
                    }
                    const auto& poses=std::get<0>(frame);
                    const auto& sounds=std::get<4>(frame);
                    crossing_sounds+=std::count(sounds.begin(),sounds.end(),0x33);
                    require(poses.empty() || poses.size()==5,"Unexpected number of Sunset crossing animals");
                    for(std::size_t i=0;i<poses.size();++i) {
                        if(!poses[i].visible)continue;
                        if(poses[i].visual_descriptor==0xb2d8)saw_running[i]=true;
                        if(poses[i].visual_descriptor==0xb2f4)saw_jumping[i]=true;
                    }
                    const auto& signals=std::get<2>(frame);
                    enter+=std::count(signals.begin(),signals.end(),0x0f);
                    leave+=std::count(signals.begin(),signals.end(),0x10);
                    const auto& presentation=std::get<6>(frame);
                    if(presentation.alternate_backdrop!=alternate){++backdrop_switches;alternate=presentation.alternate_backdrop;}
                    if(std::get<1>(frame).flags&0x20)++dusk_phases;
                }
                require(enter==12 && leave==12 && backdrop_switches==6,"Sunset Valley missed tunnel or backdrop transitions");
                require(rock_spawns==1 && rock_phases==27 && race.rock_poses().empty(),
                        "Sunset rock spawn, lifetime or retirement mismatch");
                require(dusk_phases==63 && std::get<6>(first_content.back()).palette_weight==31,"Sunset Valley did not complete the dusk ramp");
                require(crossing_sounds==1 && std::all_of(saw_running.begin(),saw_running.end(),[](bool v){return v;}) &&
                        std::all_of(saw_jumping.begin(),saw_jumping.end(),[](bool v){return v;}),"Missed Sunset crossing sequence");
                require(std::none_of(scenery.poses().begin(),scenery.poses().end(),[](const auto& pose){return pose.visible;}),
                        "Crossing animals still visible at finish");
                std::cout<<"Sunset transitions: "<<enter<<" tunnel entries, "<<leave<<" exits, "<<backdrop_switches<<" backdrop switches, "<<dusk_phases<<" active dusk phases, two falling rocks for "<<rock_phases<<" phases\n";
            }
            else if(course.id==srf::CourseId::easy_ride)
                require(commands==0 && effects==0 && scenery_effects==0 && scenery.poses().empty(),"Unexpected Easy Ride scenery triggers");
            else if(course.id==srf::CourseId::night_owl) {
                require(seconds==11 && time==18940 && standings[3]==4,"Night Owl clean race finish or standings mismatch");
                unsigned hits=0,rotated=0,flyovers=0;
                for(const auto& frame:first_content) {
                    const auto& cones=std::get<9>(frame);
                    const auto& sounds=std::get<10>(frame);
                    const auto& scenery_sounds=std::get<4>(frame);
                    flyovers+=std::count(scenery_sounds.begin(),scenery_sounds.end(),0x52);
                    require(cones.size()==7,"Night Owl did not draw all seven cone placements");
                    rotated+=std::count_if(cones.begin(),cones.end(),[](const auto& pose){return pose.pitch!=0;});
                    hits+=std::count(sounds.begin(),sounds.end(),0x54);
                }
                require(hits==2 && rotated==24,"Night Owl cone impacts or spin cadence diverged");
                require(flyovers==3,"Night Owl missed a lap-triggered flyover");
            } else if(course.id==srf::CourseId::kings_forest) {
                require(seconds==25 && time==15542 && standings[3]==4,"King's Forest clean finish changed");
                std::array<unsigned,2> seen{};
                for(const auto& frame:first_content) {
                    const auto& bridges=std::get<11>(frame);
                    require(bridges.size()==2,"King's Forest must expose both animated bridges");
                    for(unsigned i=0;i<bridges.size();++i) {
                        const auto& b=bridges[i];
                        require(b.visible && b.visual_descriptor==0xfa0c && b.frame<5,"Invalid bridge render pose");
                        seen[i]|=1u<<b.frame;
                    }
                }
                require(seen[0]==31 && seen[1]==31,"A bridge did not cycle through all five poses");
            } else if(course.id==srf::CourseId::harbor_city) {
                const auto index=static_cast<unsigned>(car);
                constexpr std::array<unsigned,3> elapsed{18769,18165,19055},left{33,39,30};
                constexpr std::array<unsigned,3> sprays{496,288,476},sign_hits{0,2,2};
                constexpr std::array<std::array<unsigned,3>,3> lap_end{{{927,1766,2698},{875,1690,2613},{872,1657,2738}}};
                std::vector<unsigned> laps;
                unsigned checkpoints=0,hits=0,particles=0;
                for(unsigned i=0;i<first_content.size();++i) {
                    const auto& frame=first_content[i];
                    if(first_states[i][3].body.word(0x7a)>laps.size())laps.push_back(i+1);
                    const auto& props=std::get<12>(frame);
                    require(props.size()==11,"Harbor City movable prop count changed");
                    const auto& impacts=std::get<13>(frame);
                    hits+=std::count(impacts.begin(),impacts.end(),0x55);
                    const auto& pickups=std::get<15>(frame);
                    checkpoints+=std::count(pickups.begin(),pickups.end(),0x41);
                    for(const auto& spray:std::get<20>(frame)){
                        require(spray.table==0x9b63 && spray.frame<4,"Harbor City wet-wheel sprite changed");
                        ++particles;
                    }
                }
                require(std::ranges::equal(laps,lap_end[index]) && standings[3]==4 &&
                        seconds==left[index] && time==elapsed[index],"Harbor City finish or clock changed");
                require(checkpoints==3 && hits==sign_hits[index] && particles==sprays[index],
                        "Harbor City pickups, hinged signs or tire spray changed");
            } else if(course.id==srf::CourseId::sky_ramp) {
                const auto index=static_cast<unsigned>(car);
                constexpr std::array<unsigned,3> phases{2703,2811,2689},elapsed{18806,19574,18706},left{2,15,3};
                constexpr std::array<std::array<unsigned,3>,3> lap_end{{{911,1750,2703},{979,1891,2811},{883,1686,2689}}};
                unsigned laps=0,checkpoints=0,flight_calls=0;
                for(unsigned i=0;i<first_content.size();++i) {
                    const auto lap=first_states[i][3].body.word(0x7a);
                    if(lap!=laps) {
                        require(i+1==lap_end[index].at(laps),"Sky Ramp lap timing changed");
                        laps=lap;
                    }
                    for(auto sound:std::get<15>(first_content[i]))checkpoints+=sound==0x41;
                    for(auto sound:std::get<19>(first_content[i]))flight_calls+=sound==0x0e;
                }
                require(inputs.size()==phases[index] && time==elapsed[index] && seconds==left[index] &&
                        standings[3]==4 && checkpoints==3 && flight_calls==1,"Sky Ramp finish, clock or triggers changed");
            } else if(course.id==srf::CourseId::big_ravine) {
                require(inputs.size()==3034 && seconds==19 && time==21165 && standings[3]==4,
                        "Big Ravine clock, finish or standings changed");
                std::array<unsigned,3> laps{};
                std::array<unsigned,6> bridge_frames{};
                std::vector<unsigned> spawn,checkpoint,grip;
                unsigned rock_poses=0,boosts=0,repairs=0;
                auto surface=initial[3].body.word(0x8a);
                for(unsigned i=0;i<inputs.size();++i) {
                    const auto& frame=first_content[i];
                    const auto& car=first_states[i][3].body;
                    const auto lap=car.word(0x7a);
                    if(lap && !laps.at(lap-1))laps[lap-1]=i+1;
                    if(surface!=car.word(0x8a)){surface=car.word(0x8a);grip.push_back(i+1);}
                    const auto& bridges=std::get<11>(frame);
                    require(bridges.size()==6,"Big Ravine span count changed");
                    for(unsigned j=0;j<bridges.size();++j) {
                        const auto& bridge=bridges[j];
                        require(bridge.visible && bridge.frame<5,"Big Ravine bridge pose out of range");
                        if(bridge.visual_descriptor==0xc068 || bridge.visual_descriptor==0xc084)
                            require(bridge.frame==2,"Big Ravine fixed span moved");
                        else require(bridge.visual_descriptor==0xe368,"Unexpected Big Ravine bridge model");
                        bridge_frames[j]|=1u<<bridge.frame;
                    }
                    for(const auto& rock:std::get<7>(frame)) {
                        require(rock.visible && rock.visual_descriptor==0xb4d0,"Clean Big Ravine run struck a rock");
                        ++rock_poses;
                    }
                    for(auto sound:std::get<8>(frame)) {
                        require(sound==0x53,"Unexpected Big Ravine falling-rock sound");
                        spawn.push_back(i+1);
                    }
                    for(auto sound:std::get<15>(frame)) {
                        if(sound==0x41)checkpoint.push_back(i+1);
                        else if(sound==0x25)++boosts;
                        else if(sound==0x1b)++repairs;
                        else throw std::runtime_error("Unexpected Big Ravine pickup sound");
                    }
                }
                require(laps==std::array<unsigned,3>{1061,2050,3034} && grip==std::vector<unsigned>{332,1245},
                        "Big Ravine lap timing or traction changed");
                require(std::ranges::count(bridge_frames,4u)==2 && std::ranges::count(bridge_frames,31u)==4 &&
                        spawn==std::vector<unsigned>{1802,2783} && rock_poses==108,"Big Ravine bridge or rock sequence changed");
                require(checkpoint==std::vector<unsigned>{587,1565,2550} && boosts==6 && repairs==6,
                        "Big Ravine checkpoint or pickups changed");
            } else if(course.id==srf::CourseId::lake_side) {
                require(inputs.size()==2260 && seconds==14 && time==15648 && standings[3]==4,
                        "Lake Side race clock, finish or standings changed");
                std::array<unsigned,3> lap_phases{};
                unsigned bridge_frames=0,checkpoints=0,boosts=0,repairs=0;
                for(std::size_t i=0;i<first_content.size();++i) {
                    const auto& current=first_content[i];
                    const auto& bridges=std::get<11>(current);
                    require(bridges.size()==1 && bridges.front().visual_descriptor==0xc404 &&
                            bridges.front().visible && bridges.front().frame<16,"Lake Side bridge pose changed");
                    bridge_frames|=1u<<bridges.front().frame;
                    const auto& cones=std::get<12>(current);
                    require(cones.size()==4 && std::ranges::all_of(cones,[](const auto& p) {
                        return p.visible && p.visual_descriptor==0xac80;
                    }),"Lake Side cone placements changed");
                    for(auto effect:std::get<15>(current)) {
                        if(effect==0x41)++checkpoints;
                        else if(effect==0x25)++boosts;
                        else if(effect==0x1b)++repairs;
                        else throw std::runtime_error("Unexpected Lake Side pickup effect");
                    }
                    const auto lap=first_states[i][3].body.word(0x7a);
                    if(lap && !lap_phases.at(lap-1))lap_phases[lap-1]=static_cast<unsigned>(i+1);
                }
                require(bridge_frames==0xffff && checkpoints==3 && boosts==4 && repairs==6,
                        "Lake Side missed a bridge pose, checkpoint or pickup");
                require(lap_phases==std::array<unsigned,3>{790,1523,2260},"Lake Side lap timing changed");
            } else if(course.id==srf::CourseId::night_cruise) {
                require(inputs.size()==2364 && seconds==7 && time==16390 && standings[3]==4,
                        "Night Cruise finish or clock changed");
                std::vector<std::size_t> cones,signs,checkpoints,repairs;
                unsigned folded=0;
                for(std::size_t i=0;i<inputs.size();++i) {
                    const auto& props=std::get<12>(first_content[i]);
                    require(props.size()==9,"Night Cruise sign/cone count changed");
                    for(const auto& prop:props) {
                        require(prop.visual_descriptor==0xac80 || prop.visual_descriptor==0xaf58,
                                "Unexpected Night Cruise roadside model");
                        if(i+1==inputs.size() && prop.visual_descriptor==0xaf58 && prop.frame==3)++folded;
                    }
                    for(auto sound:std::get<13>(first_content[i])) {
                        require(sound==0x54 || sound==0x55,"Unexpected Night Cruise roadside sound");
                        (sound==0x54?cones:signs).push_back(i+1);
                    }
                    for(auto sound:std::get<15>(first_content[i])) {
                        require(sound==0x41 || sound==0x1b,"Unexpected Night Cruise pickup sound");
                        (sound==0x41?checkpoints:repairs).push_back(i+1);
                    }
                }
                require(folded==3 && cones==std::vector<std::size_t>{269,279,287} &&
                        signs==std::vector<std::size_t>{355,358,711},"Night Cruise roadside impacts changed");
                require(checkpoints==std::vector<std::size_t>{488,1204,1911} &&
                        repairs==std::vector<std::size_t>{941,1661},"Night Cruise checkpoint or repair events changed");
            } else if(course.id==srf::CourseId::white_land) {
                require(inputs.size()==2560 && seconds==13 && time==17786 && standings[3]==4,
                        "White Land clean race finish changed");
                std::vector<std::size_t> grip,boosts,repairs;
                std::uint16_t previous=initial[3].body.word(0x8a);
                std::vector<std::uint32_t> ice;
                unsigned snowball_poses=0,spawns=0;
                for(std::size_t i=0;i<inputs.size();++i) {
                    const auto next=first_states[i][3].body.word(0x8a);
                    if(next!=previous)grip.push_back(i+1);
                    previous=next;
                    for(auto sound:std::get<15>(first_content[i]))
                        (sound==0x25?boosts:repairs).push_back(i+1);
                    for(const auto& sprite:std::get<16>(first_content[i])) {
                        require(sprite.table==0x996d,"Unexpected debris on the clean White Land line");
                        if(std::find(ice.begin(),ice.end(),sprite.command_offset)==ice.end())ice.push_back(sprite.command_offset);
                    }
                    for(const auto& rock:std::get<7>(first_content[i])) {
                        require(rock.visible && rock.visual_descriptor==0xb6e4,"Unexpected White Land hazard model");
                        ++snowball_poses;
                    }
                    for(auto sound:std::get<8>(first_content[i])) {
                        require(sound==0x11 && i+1==1982,"Unexpected White Land hazard sound");
                        ++spawns;
                    }
                    require(std::get<17>(first_content[i]).empty(),"Clean White Land fixture hit a breakable");
                }
                require(ice.size()==6 && grip==std::vector<std::size_t>{100,149,926,976,1289},
                        "White Land ice objects or grip transitions changed");
                require(boosts==std::vector<std::size_t>{311,515,1124,1972} &&
                        repairs==std::vector<std::size_t>{636,770,1613,2319,2451},"White Land pickup timing changed");
                require(spawns==1 && snowball_poses==68,"Missing White Land final-lap snowballs");
            } else if(course.id==srf::CourseId::sea_breeze) {
                require(seconds==16 && time==13497 && standings[3]==4,"Sea Breeze clean race finish changed");
                std::vector<std::size_t> impacts;
                std::vector<std::size_t> boosts,repairs;
                unsigned visible_pickups=0;
                std::array<unsigned,2> turned{};
                for(std::size_t i=0;i<first_content.size();++i) {
                    const auto& signs=std::get<12>(first_content[i]);
                    require(signs.size()==2,"Missing Sea Breeze sign placements");
                    for(unsigned slot=0;slot<signs.size();++slot) {
                        const auto& sign=signs[slot];
                        require(sign.visible && sign.visual_descriptor==0xb700 && !sign.pitch && !sign.roll,
                                "Incorrect Sea Breeze movable sign pose");
                        if(i && sign.yaw!=std::get<12>(first_content[i-1])[slot].yaw)++turned[slot];
                    }
                    for(const auto sound:std::get<13>(first_content[i])) {
                        require(sound==0x59,"Unexpected Sea Breeze sign sound");
                        impacts.push_back(i+1);
                    }
                    for(const auto sound:std::get<15>(first_content[i])) {
                        require(sound==0x25 || sound==0x1b,"Unexpected Sea Breeze pickup sound");
                        (sound==0x25?boosts:repairs).push_back(i+1);
                    }
                    const auto& pickups=std::get<14>(first_content[i]);
                    require(pickups.size()>=4,"Sea Breeze lost a pickup placement");
                    // The four persistent placements precede the rising collection icons.
                    for(std::size_t child=4;child<pickups.size();++child)
                        if(pickups[child].visible)++visible_pickups;
                }
                require(impacts==std::vector<std::size_t>{515,556} && turned==std::array<unsigned,2>{6,6},
                        "Sea Breeze sign contact or spin timing changed");
                require(boosts==std::vector<std::size_t>{199,801,1440} &&
                        repairs==std::vector<std::size_t>{669,942,1299,1941} && visible_pickups==63,
                        "Sea Breeze pickup contacts or presentation changed");
                require(effects==9 && commands==0 && scenery_effects==0,"Sea Breeze proximity sound totals changed");
            }
            std::cout<<course.slug<<": "<<inputs.size()<<" phases, 3 laps, zero damage, "<<seconds<<" seconds left; opponents ";
            for(unsigned i=0;i<3;++i)std::cout<<final[i].body.word(0x7a)<<' ';
            std::cout<<"laps; restart identical; "<<commands<<" tunnel commands, "<<effects<<" proximity effects, "<<scenery_effects<<" scenery effects\n";
        }
        championship_grid_and_retry(rom);
        bonus_tally_grid_and_retry(rom);
        harbor_prop_impacts(rom);
        lake_side_route_sentinel(rom);
        night_cruise_flyover(rom);
        return 0;
    }catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
