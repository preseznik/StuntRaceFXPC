#include "srf/environment.hpp"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition,const char* message) { if(!condition)throw std::runtime_error(message); }
void blend_reference() {
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"aqua_blend_reference.csv");
    std::string line;require(bool(std::getline(file,line)),"Missing original RGB555 blend trace");
    unsigned compared=0;
    while(std::getline(file,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        unsigned from,to,weight,result;
        require(bool(row>>from>>to>>weight>>result),"Invalid original RGB555 blend trace");
        require(srf::blend_environment_rgb555(static_cast<std::uint16_t>(from),
            static_cast<std::uint16_t>(to),static_cast<std::uint8_t>(weight))==result,
            "RGB555 blend differs from original CPU routine");
        ++compared;
    }
    require(compared==1536,"Incomplete original RGB555 blend trace");
}
void video_reference() {
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"aqua_video_reference.csv");
    std::string line;require(bool(std::getline(file,line)),"Missing original video trace");
    srf::EnvironmentState state;
    srf::EnvironmentVideoState video;
    unsigned compared=0;
    while(std::getline(file,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        unsigned frame,flags_in,speed,flags_out,weight,level,green,counter,direction,bucket,
            mode,red_command,green_command,blue_command,fixed,active_mode;
        require(bool(row>>frame>>flags_in>>speed>>flags_out>>weight>>level>>green>>counter>>
            direction>>bucket>>mode>>red_command>>green_command>>blue_command>>fixed>>active_mode),
            "Invalid original video trace");
        // The natural oracle supplies CPU callback flags/speed; no native driver
        // or contact algorithm is used to generate the expected video controls.
        state.flags=static_cast<std::uint8_t>(flags_in);
        state.palette_weight=static_cast<std::uint8_t>(weight);
        srf::advance_environment_video(state,video,static_cast<std::uint8_t>(speed),1);
        const srf::EnvironmentVideoState expected{
            static_cast<std::uint8_t>(level),static_cast<std::uint8_t>(green),
            static_cast<std::uint8_t>(counter),static_cast<std::uint8_t>(direction),
            static_cast<std::uint8_t>(bucket),static_cast<std::uint8_t>(mode),
            static_cast<std::uint8_t>(active_mode),
            {static_cast<std::uint8_t>(red_command),static_cast<std::uint8_t>(green_command),
             static_cast<std::uint8_t>(blue_command)},static_cast<std::uint16_t>(fixed)};
        if(state.flags!=flags_out || video!=expected)
            throw std::runtime_error("Video controls differ from original at frame "+std::to_string(frame));
        ++compared;
    }
    require(compared==4800,"Incomplete original video trace");

    // Chunking elapsed video ticks must not change the original counters. These
    // batches cover fade, hold, reversal, stopping, low-speed and boosted pulses.
    srf::EnvironmentState singles,batches;
    srf::EnvironmentVideoState one,many;
    for(const auto [flags,speed,ticks]:std::array<std::array<unsigned,3>,7>{{
        {10,0,30},{8,40,130},{8,80,130},{8,129,24},{4,129,25},{10,0,7},{6,0,12}}}) {
        singles.flags=batches.flags=static_cast<std::uint8_t>(flags);
        for(unsigned i=0;i<ticks;++i)srf::advance_environment_video(singles,one,static_cast<std::uint8_t>(speed),1);
        srf::advance_environment_video(batches,many,static_cast<std::uint8_t>(speed),ticks);
        require(one==many && singles==batches,"Video controls depend on tick batch size");
    }
}
struct Expected { unsigned callback,iteration,contact,flags,second,palette,command,effect; };
std::vector<Expected> reference() {
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"aqua_environment_reference.csv");
    std::string line;require(bool(std::getline(file,line)),"Missing original environment trace");
    std::vector<Expected> result;
    while(std::getline(file,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);Expected value{};
        require(bool(row>>value.callback>>value.iteration>>value.contact>>value.flags>>value.second>>
                     value.palette>>value.command>>value.effect),"Invalid environment trace");
        result.push_back(value);
    }
    require(result.size()==25,"Incomplete original environment trace");return result;
}
srf::VehicleState player_at(const srf::EnvironmentTrigger& trigger,bool inside) {
    srf::VehicleState player;
    player.word(0x16)=2;player.word(0x64)=1;
    for(unsigned axis=0;axis<3;++axis) {
        player.word(0x0c+axis*2)=256;
        player.word(0x1e +axis*2)=static_cast<std::uint16_t>(trigger.position[axis]);
    }
    if(!inside)player.word(0x20)=static_cast<std::uint16_t>(trigger.position[1]+trigger.extent+256);
    return player;
}
void night_lighting(const srf::Rom& rom) {
    srf::CourseEnvironment lighting(rom,srf::CourseId::night_owl);
    require(lighting.triggers().size()==4,"Missing original Night Owl light triggers");
    const auto& up=*std::find_if(lighting.triggers().begin(),lighting.triggers().end(),
        [](const auto& t){return t.callback_address==0x09a32c;});
    const auto& down=*std::find_if(lighting.triggers().begin(),lighting.triggers().end(),
        [](const auto& t){return t.callback_address==0x09a3c3;});
    auto player=player_at(up,true);
    lighting.step(player);lighting.step(player);
    require(lighting.state().palette_weight==2,"Night Owl palette rise did not start");
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"night_owl_lighting_reference.csv");
    std::string line;require(bool(std::getline(file,line)),"Missing Night Owl PPU reference");
    unsigned samples=0;
    while(std::getline(file,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        unsigned frame,height;std::string expected;
        require(bool(row>>frame>>height>>expected),"Invalid Night Owl PPU reference");
        player.word(0x20)=static_cast<std::uint16_t>(height);
        lighting.step(player);lighting.advance_video(1);
        std::array<std::uint8_t,256> bytes{};
        const auto values=lighting.presentation().row_subtract_rgb555;
        for(unsigned y=0;y<128;++y) {
            bytes[y*2]=static_cast<std::uint8_t>(values[y]);
            bytes[y*2+1]=static_cast<std::uint8_t>(values[y]>>8);
        }
        if(srf::sha256(bytes)!=expected)throw std::runtime_error("Night Owl original PPU row mismatch at frame "+std::to_string(frame));
        ++samples;
    }
    require(samples==253 && lighting.state().palette_weight==30,"Night Owl rise extent or coverage changed");
    player=player_at(down,true);
    for(unsigned i=0;i<19;++i)lighting.step(player);
    lighting.advance_video(130);
    auto p=lighting.presentation();
    require(lighting.state().flags==0 && !p.palette_weight &&
            std::all_of(p.row_subtract_rgb555.begin(),p.row_subtract_rgb555.end(),[](auto c){return !c;}),
            "Night Owl lamps failed to turn off");
    lighting.reset();require(lighting.presentation()==p,"Night Owl restart retained lighting");
}
void forest_weather(const srf::Rom& rom) {
    srf::CourseEnvironment environment(rom,srf::CourseId::kings_forest);
    require(environment.triggers().size()==1,"Unexpected King's Forest bridge trigger count");
    const auto& bridge=environment.triggers().front();
    require(bridge.callback_address==0x09a788 && bridge.extent==3840,"Missing original bridge weather box");
    const auto inside=player_at(bridge,true),outside=player_at(bridge,false);
    environment.step(inside);
    require(environment.events().sound_commands==std::vector<std::uint8_t>{0x0e},"Bridge ambience initial off");
    environment.step(inside);
    require(environment.events().sound_commands==std::vector<std::uint8_t>{0x13} &&
            environment.presentation().fog_weight==15 && !environment.state().flags,"Unexpected first lap fog");
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"kings_forest_weather_reference.csv");
    std::string line;require(bool(std::getline(file,line)),"Missing original King's Forest weather trace");
    unsigned compared=0;
    while(std::getline(file,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        unsigned phase,flags,fog,palette;
        require(bool(row>>phase>>flags>>fog>>palette) && phase==compared,"Invalid bridge weather reference");
        environment.step(inside,1);environment.advance_video(phase%8);
        require(environment.state().flags==flags && environment.presentation().fog_weight==fog &&
                environment.presentation().palette_weight==palette,"Weather differs from original on lap two");
        ++compared;
    }
    require(compared==286,"Incomplete bridge weather trace");
    environment.step(outside,1);environment.step(outside,1);
    require(environment.events().sound_commands==std::vector<std::uint8_t>{0x0e},"Forest bridge exit ambience");
    environment.step(inside,1);environment.step(inside,1);
    require(!environment.state().flags && environment.events().sound_commands==std::vector<std::uint8_t>{0x13},
            "Fog rearmed on second crossing");
    environment.reset();
    require(environment.presentation().fog_weight==15 && !environment.presentation().palette_weight,
            "King's Forest restart did not restore opening weather");
}
void sunset_transitions(const srf::Rom& rom) {
    srf::CourseEnvironment environment(rom,srf::CourseId::sunset_valley);
    const auto triggers=environment.triggers();
    require(triggers.size()==14,"Unexpected Sunset Valley environment inventory");
    const auto trigger=[&](unsigned callback) -> const srf::EnvironmentTrigger& {
        const auto it=std::find_if(triggers.begin(),triggers.end(),
            [callback](const auto& value){return value.callback_address==callback;});
        require(it!=triggers.end(),"Missing Sunset environment trigger");
        return *it;
    };
    const auto dusk=player_at(trigger(0x09e781),true);
    environment.step(dusk,0);
    environment.step(dusk,0);
    require(environment.state().flags==0,"Sunset began on the first lap");
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"sunset_dusk_reference.csv");
    std::string line;
    require(bool(std::getline(file,line)),"Missing original Sunset palette trace");
    unsigned comparisons=0;
    while(std::getline(file,line)) {
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);
        unsigned call,flags,weight;
        require(bool(row>>call>>flags>>weight),"Invalid original Sunset palette trace");
        environment.step(dusk,1);
        environment.advance_video(call%7);
        require(environment.state().flags==flags && environment.presentation().palette_weight==weight,
                "Sunset palette ramp differs from original phase trace");
        ++comparisons;
    }
    require(comparisons==65,"Incomplete Sunset palette trace");
    const auto enter=player_at(trigger(0x09e80d),true);
    const auto leave=player_at(trigger(0x09e87f),true);
    environment.step(enter,1);
    environment.step(enter,1);
    require(environment.presentation().alternate_backdrop,"Alternate Sunset tilemap was not selected");
    environment.step(leave,1);
    environment.step(leave,1);
    require(!environment.presentation().alternate_backdrop && environment.presentation().palette_weight==31,
            "Leaving the alternate map reset or lost the dusk palette");
    environment.step(dusk,1);
    environment.step(dusk,1);
    require(environment.state().flags==0,"Sunset ramp rearmed on the same lap");
    environment.reset();
    require(environment.presentation().palette_weight==0,"Restart retained the dusk palette");
    const auto& roadside=trigger(0x09e097);
    require(roadside.extent==320,"Sunset roadside contact extent differs from original");
    const auto outside=player_at(roadside,false);
    const auto inside=player_at(roadside,true);
    const std::array contact_sequence{inside,inside,inside,outside,outside,inside,inside};
    for(std::size_t i=0;i<contact_sequence.size();++i) {
        environment.step(contact_sequence[i],1);
        const auto& sounds=environment.events().effects;
        const bool on_entry=i==1 || i==6;
        require(on_entry ? sounds==std::vector<std::uint8_t>{0x5a} : sounds.empty(),
                "Original roadside contact sound did not follow entry and re-entry");
    }
}

void night_cruise_weather(const srf::Rom& rom) {
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"night_cruise_environment_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing Night Cruise original environment trace");
    srf::CourseEnvironment environment(rom,srf::CourseId::night_cruise);
    unsigned previous_frame=0,phases=0,commands=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        unsigned frame,phase,lap,dusk,weight,command;int x,y,z;
        require(bool(row>>frame>>phase>>lap>>x>>y>>z>>dusk>>weight>>command),"Invalid Night Cruise original trace");
        srf::VehicleState player;
        player.word(0x16)=0x12;player.word(0x64)=1;
        player.word(0x0c)=player.word(0x10)=400;player.word(0x0e)=240;
        player.word(0x1e)=x;player.word(0x20)=y;player.word(0x22)=z;
        environment.advance_video(frame-previous_frame);
        environment.step(player,lap);
        const auto& sounds=environment.events().sound_commands;
        if((environment.state().flags&32)!=dusk || environment.presentation().palette_weight!=weight ||
           environment.presentation().skyline_phase!=(phase-1)%22 ||
           sounds.size()!=unsigned(command!=0) || (sounds.empty()?0:sounds[0])!=command)
            throw std::runtime_error("Original Night Cruise dusk/tunnel event differs at phase "+std::to_string(phase));
        previous_frame=frame;++phases;commands+=sounds.size();
    }
    require(phases==2266 && commands==24,"Incomplete Night Cruise original race");
}
void white_land_weather(const srf::Rom& rom) {
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"white_land_weather_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing original White Land weather replay");
    srf::CourseEnvironment environment(rom,srf::CourseId::white_land);
    unsigned compared=0,commands=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,lap,x,y,z,flags,fog,weight,effect;
        require(bool(row>>phase>>lap>>x>>y>>z>>flags>>fog>>weight>>effect),"Bad White Land weather sample");
        srf::VehicleState body;
        body.word(0x16)=0x12;body.word(0x64)=1;
        body.word(0x0c)=body.word(0x10)=400;body.word(0x0e)=240;
        body.word(0x1e)=x;body.word(0x20)=y;body.word(0x22)=z;
        environment.step(body,lap);
        const auto& events=environment.events().sound_commands;
        if(environment.state().flags!=flags || environment.presentation().fog_weight!=fog ||
           environment.state().palette_weight!=weight || (events.empty()?0:events.at(0))!=effect || events.size()>1)
            throw std::runtime_error("Original White Land weather/ambience differs at phase "+std::to_string(phase));
        commands+=events.size();++compared;
    }
    require(compared==1995 && commands==12,"Incomplete White Land weather and course audio coverage");
}
}
int wmain() {
    try {
        blend_reference();
        video_reference();
        const auto path=_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);const auto expected=reference();
        sunset_transitions(rom);
        night_lighting(rom);
        forest_weather(rom);
        white_land_weather(rom);
        night_cruise_weather(rom);
        srf::CourseEnvironment environment(rom,srf::CourseId::aqua_tunnel);
        require(environment.triggers().size()==9,"Wrong original Aqua trigger count");
        const std::vector<srf::EnvironmentTrigger> triggers(environment.triggers().begin(),environment.triggers().end());
        unsigned compared=0;
        for(const auto& trigger:triggers) {
            environment.reset();
            auto outside=player_at(trigger,false),inside=player_at(trigger,true);
            // First call only establishes original previous-GSU contact history.
            environment.step(outside);
            for(const auto& row:expected) {
                if(row.callback!=trigger.callback_address)continue;
                // Callback contacts 0,1,1,0,1; current positions lead by one phase.
                const auto& supplied=(row.iteration==0||row.iteration==1||row.iteration==3)?inside:outside;
                const auto original=supplied.words;environment.step(supplied);
                require(supplied.words==original,"Environment modified vehicle state");
                require(environment.state()==srf::EnvironmentState{
                    static_cast<std::uint8_t>(row.flags),static_cast<std::uint8_t>(row.second),
                    static_cast<std::uint8_t>(row.palette)},"Environment modes differ from original callback");
                const auto check=[](const auto& values,unsigned value) {
                    return value?values.size()==1&&values[0]==value:values.empty();
                };
                require(check(environment.events().sound_commands,row.command),"Tunnel command edge differs from original callback");
                require(check(environment.events().effects,row.effect),"Proximity effect edge differs from original callback");
                ++compared;
            }
            environment.reset();
            require(environment.state()==srf::EnvironmentState{} && environment.events().effects.empty() &&
                    environment.events().sound_commands.empty(),"Environment restart retained state");
            // Strict touching boundary is not a contact; opponents cannot toggle it.
            for(unsigned i=0;i<4;++i)environment.step(outside);
            require(environment.state()==srf::EnvironmentState{} && environment.events().effects.empty(),"Touching trigger boundary counted as contact");
            inside.word(0x64)=12;
            for(unsigned i=0;i<4;++i)environment.step(inside);
            require(environment.state()==srf::EnvironmentState{} && environment.events().effects.empty(),"Opponent activated player environment");
        }
        require(compared==45,"Missing duplicate original tunnel trigger comparisons");
        srf::CourseEnvironment easy(rom,srf::CourseId::easy_ride);
        require(easy.triggers().empty(),"Aqua environment leaked into Easy Ride");
        easy.step(player_at(triggers.front(),true));
        easy.advance_video(300);
        require(easy.state()==srf::EnvironmentState{} && easy.events().sound_commands.empty(),"Easy Ride environment changed");
        require(easy.presentation().color_math_layers==0 && !easy.presentation().blank_backdrop,
                "Aqua color operation leaked into Easy Ride");
        environment.reset();environment.advance_video(300);
        require(environment.video_state()==srf::EnvironmentVideoState{},"Environment restart retained video controls");
        std::cout<<"1536 original RGB555 blends; 4800 original video comparisons; 45 original callback comparisons; all 9 boxes, strict bounds, prior phase, edge rearm, restart and opponent exclusion passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
