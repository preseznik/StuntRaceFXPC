#include "srf/scenery.hpp"
#include "srf/native_race.hpp"
#include "srf/random.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value,const char* reason) { if (!value) throw std::runtime_error(reason); }
bool same_position(const srf::SceneryPose& a,const srf::SceneryPose& b) {
    return a.position==b.position && a.yaw==b.yaw && a.pitch==b.pitch && a.roll==b.roll;
}
srf::VehicleState player(int x=-24128,int z=-17408) {
    srf::VehicleState result;
    result.word(0x1e)=static_cast<std::uint16_t>(x);
    result.word(0x20)=0;
    result.word(0x22)=static_cast<std::uint16_t>(z);
    result.word(0x0c)=400; result.word(0x0e)=240; result.word(0x10)=400;
    result.word(0x16)=0x12; result.word(0x64)=1;
    return result;
}
std::vector<srf::SceneryPose> snapshot(const srf::CourseScenery& scenery) {
    return {scenery.poses().begin(),scenery.poses().end()};
}
void original_capture(const srf::Rom& rom) {
    // Original Snes9x callbacks, natural button-only drive from course1 state.
    // All three actors are still at source positions before phase690. Each
    // observed row is an actual callback exit, including inactive tail updates.
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"aqua_fish_reference.csv");
    std::string line; require(bool(std::getline(input,line)),"Missing original fish capture");
    srf::CourseScenery scenery(rom,srf::CourseId::aqua_tunnel);
    unsigned checks=0,rows=0;
    while (std::getline(input,line)) {
        std::istringstream row(line); std::array<int,32> values{};
        for (unsigned i=0;i<values.size();++i) {
            require(bool(row>>values[i]),"Malformed original fish capture");
            if (i+1<values.size()) { char comma; require(bool(row>>comma)&&comma==',',"Bad capture separator"); }
        }
        srf::VehicleState body;
        constexpr unsigned offsets[]{0x1e,0x20,0x22,0x24,0x4a,0xc,0xe,0x10,0x16,0x64};
        for (unsigned i=0;i<10;++i) body.word(offsets[i])=static_cast<std::uint16_t>(values[i+1]);
        if (!rows) for (int phase=1;phase<values[0];++phase) scenery.step(body);
        scenery.step(body);
        for (unsigned i=0;i<3;++i) {
            const auto base=11+7*i; const auto& pose=scenery.poses()[i];
            require(pose.visible==bool(values[base]),"Original fish activation boundary mismatch");
            if (!values[base]) continue;
            if (!pose.visible || pose.position.x!=values[base+1] || pose.position.y!=values[base+2] ||
                pose.position.z!=values[base+3] || pose.yaw!=values[base+4] || pose.roll!=values[base+5]) {
                std::cerr<<"Original fish mismatch phase "<<values[0]<<" fish "<<i<<": "
                         <<pose.position.x<<','<<pose.position.z<<','<<int(pose.yaw)
                         <<" expected "<<values[base+1]<<','<<values[base+3]<<','<<values[base+4]<<'\n';
                throw std::runtime_error("Original fish callback mismatch");
            }
            ++checks;
        }
        ++rows;
    }
    require(rows==111 && checks==286,"Incomplete original fish callback coverage");
    std::cout<<checks<<" original fish callback poses matched\n";
}
void night_cruise_roadside(const srf::Rom& rom) {
    srf::CourseScenery scenery(rom,srf::CourseId::night_cruise);
    const std::array<int,4> z{2432,6272,-5248,-1408};
    require(scenery.sprites().size()==z.size(),"Night Cruise must draw four tree crowns");
    for(std::size_t i=0;i<z.size();++i) {
        const auto& tree=scenery.sprites()[i];
        require(tree.position==srf::Vertex{5632,-3452,z[i]} && tree.table==0x9973 &&
                tree.diameter==928 && !tree.frame,"Night Cruise tree differs from 09:8BD4");
    }
    require(scenery.poses().size()==1,"Night Cruise checkpoint panel missing");
    const auto& panel=scenery.poses()[0];
    require(panel.visual_descriptor==0xb7fc && panel.position==srf::Vertex{6592,-6240,13312} &&
            panel.yaw==64,"Night Cruise checkpoint differs from original object");
    for(int i=0;i<3;++i)scenery.step(player(6592,13312));
    require(panel.pitch==232,"Checkpoint active pitch differs from 09:AD50");
    scenery.reset();
    require(scenery.poses()[0].pitch==0,"Checkpoint did not reset");
}
void original_rotating_capture(const srf::Rom& rom) {
    // A bounded original boot plus natural button-only driving capture. Rows
    // with command0 carry the phase's player sample and sound-event count;
    // other rows are actual actor callback exits, including hidden poses.
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"aqua_rotating_reference.csv");
    std::string line; require(bool(std::getline(input,line)),"Missing rotating scenery capture");
    using Record=std::array<int,11>;
    std::vector<Record> records;
    while (std::getline(input,line)) {
        Record record{}; std::istringstream row(line);
        for (unsigned i=0;i<record.size();++i) {
            require(bool(row>>record[i]),"Malformed rotating scenery capture");
            if (i+1<record.size()) { char comma; require(bool(row>>comma)&&comma==',',"Bad rotating capture separator"); }
        }
        records.push_back(record);
    }
    srf::CourseScenery scenery(rom,srf::CourseId::aqua_tunnel);
    std::vector<srf::SceneryPose> first_result;
    for (unsigned replay=0;replay<2;++replay) {
        scenery.reset();
        require(scenery.effects().empty(),"Restart retained rotating sound events");
        unsigned checks=0,phases=0,sounds=0;
        for (std::size_t index=0;index<records.size();) {
            const auto sample=records[index++];
            require(!sample[1] && sample[0]==int(++phases),"Missing original scenery phase");
            auto body=player(sample[2],sample[4]); body.word(0x20)=static_cast<std::uint16_t>(sample[3]);
            const auto unchanged=body.words;
            scenery.step(body);
            require(body.words==unchanged,"Rotating scenery mutated vehicle state");
            require(scenery.effects().size()==std::size_t(sample[10]),"Original rotating sound cadence mismatch");
            for (const auto effect:scenery.effects()) require(effect==0x38,"Unexpected rotating effect");
            sounds+=static_cast<unsigned>(scenery.effects().size());
            std::map<unsigned,Record> expected;
            while (index<records.size() && records[index][1]) {
                const auto record=records[index++];
                require(record[0]==sample[0],"Scenery phase grouping");
                expected.emplace(record[1],record);
            }
            for (const auto& pose:scenery.poses().subspan(3)) {
                const auto found=expected.find(pose.command_offset);
                if (found==expected.end()) { require(!pose.visible,"Inactive rotating actor rendered"); continue; }
                const auto& r=found->second;
                if (pose.position.x!=r[2] || pose.position.y!=r[3] || pose.position.z!=r[4] ||
                    pose.yaw!=r[5] || pose.pitch!=r[6] || pose.roll!=r[7] || pose.visual_descriptor!=r[9] ||
                    pose.visible!=(r[9]!=0xa040) || ((r[8]&128) && pose.frame!=(r[8]&63))) {
                    std::cerr<<"Rotating mismatch phase "<<phases<<" command "<<pose.command_offset
                             <<": "<<pose.position.x<<','<<pose.position.y<<','<<pose.position.z
                             <<" angles "<<int(pose.yaw)<<','<<int(pose.pitch)<<','<<int(pose.roll)
                             <<" frame "<<pose.frame<<" expected "<<r[2]<<','<<r[3]<<','<<r[4]
                             <<" angles "<<r[5]<<','<<r[6]<<','<<r[7]<<" frame "<<(r[8]&63)<<'\n';
                    throw std::runtime_error("Original rotating callback mismatch");
                }
                ++checks;
            }
        }
        require(phases==1273 && checks==1790 && sounds==5,"Incomplete rotating reference coverage");
        if (!replay) first_result=snapshot(scenery);
        else require(snapshot(scenery)==first_result,"Full rotating capture changed after restart");
    }
    std::cout<<"1790 original rotating callback poses and 5 effects matched, including restart replay\n";
}
}

namespace {
void night_flyovers(const srf::Rom& rom) {
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"night_owl_flyover_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing original Night Owl flight trace");
    srf::CourseScenery scenery(rom,srf::CourseId::night_owl);
    unsigned spawns=0,previous_lap=3;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);unsigned lap,phase,pitch,yaw,roll;
        int x,y,z;bool spawn,visible;srf::Vertex expected{};
        require(bool(row>>lap>>phase>>x>>y>>z>>spawn>>visible>>
            expected.x>>expected.y>>expected.z>>pitch>>yaw>>roll),"Invalid original Night Owl flight trace");
        if(previous_lap!=lap)scenery.reset();
        previous_lap=lap;
        auto body=player(x,z);body.word(0x20)=static_cast<std::uint16_t>(y);
        scenery.step(body,static_cast<std::uint16_t>(lap));
        const auto poses=scenery.poses();const auto effects=scenery.effects();
        const bool flight=poses.size()==1;
        if(flight!=visible || spawn!=(effects.size()==1 && effects[0]==0x52) ||
           (flight && (poses[0].position!=expected || poses[0].pitch!=pitch ||
                poses[0].yaw!=yaw || poses[0].roll!=roll || poses[0].visual_descriptor!=0xb770)))
            throw std::runtime_error("Original Night Owl flyover diverged at phase "+std::to_string(phase));
        spawns+=spawn;
    }
    require(spawns==3,"Missing Night Owl flight on one or more laps");
}
void forest_trees(const srf::Rom& rom) {
    srf::CourseScenery forest(rom,srf::CourseId::kings_forest);
    const auto trees=forest.sprites();
    require(trees.size()==38 && trees[0]==srf::ScenerySprite{0x2b445,{15232,-140,30912},0x9971,1536} &&
            trees[1]==srf::ScenerySprite{0x2b445,{15232,-1540,30912},0x9973,2176} &&
            trees[34]==srf::ScenerySprite{0x2bc51,{26272,-140,-3808},0x9971,544} &&
            trees[35]==srf::ScenerySprite{0x2bc51,{26272,-760,-3808},0x9973,1088},
            "Original King's Forest trunk or crown recipe changed");
    const auto body=player(26272,-3808);
    forest.step(body);
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"kings_forest_tree_impact.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing original King's Forest impact capture");
    unsigned rows=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);unsigned phase,leaf,yaw,pitch,roll;srf::Vertex position;
        require(bool(row>>phase>>leaf>>position.x>>position.y>>position.z>>yaw>>pitch>>roll),
                "Malformed King's Forest impact capture");
        if(leaf==0)forest.step(body);
        const auto poses=forest.poses();
        require(poses.size()==5,"Wrong number of visible King's Forest actors");
        const auto& p=poses[leaf+1];
        if(p.position!=position || p.yaw!=yaw || p.pitch!=pitch || p.roll!=roll || p.visual_descriptor!=0xabf4)
            throw std::runtime_error("Original leaf pose mismatch at phase "+std::to_string(phase)+
                                     ", leaf "+std::to_string(leaf));
        require(phase==0 ? forest.effects().size()==1 && forest.effects()[0]==0x58 : forest.effects().empty(),
                "Original tree impact sound edge mismatch");
        ++rows;
    }
    require(rows==56,"Incomplete original leaf capture");
    forest.step(body);
    require(forest.poses().size()==1,"Expired leaves survived their original 15-phase lifetime");
    auto away=body;away.word(0x20)=1000;
    forest.step(away);forest.step(body);forest.step(body);
    require(forest.poses().size()==1 && forest.effects().size()==1 && forest.effects()[0]==0x58,
            "Tree contact did not rearm its sound independently of its leaves");
    forest.reset();forest.step(body);forest.step(body);
    require(forest.poses().size()==5,"Restart did not rearm the tree");
}
void forest_orbit(const srf::Rom& rom) {
    srf::CourseScenery forest(rom,srf::CourseId::kings_forest);
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"kings_forest_orbit_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing original King's Forest orbit trace");
    unsigned compared=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,x,z,yaw;srf::Vertex position;
        require(bool(row>>phase>>x>>z>>position.x>>position.y>>position.z>>yaw),"Malformed orbit trace");
        forest.step(player(x,z));
        const auto& pose=forest.poses().front();
        if(pose.position!=position || pose.yaw!=yaw || !pose.visible || pose.visual_descriptor!=0xad7c)
            throw std::runtime_error("Original King's Forest orbit mismatch at phase "+std::to_string(phase)+
                ": "+std::to_string(pose.position.x)+","+std::to_string(pose.position.z)+","+std::to_string(pose.yaw));
        ++compared;
    }
    require(compared==85,"Incomplete King's Forest orbit capture");
}
void sea_breeze_scenery(const srf::Rom& rom) {
    // Numeric exits of 09:CBC3, CAA2 and AD5D, driven by the original
    // player's sampled position from course setup through one full lap.
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"sea_breeze_scenery_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing Sea Breeze scenery capture");
    srf::CourseScenery sea(rom,srf::CourseId::sea_breeze);
    unsigned phases=0,checks=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int phase,command,yaw,pitch,roll,frame,model;srf::Vertex point;
        require(bool(row>>phase>>command>>point.x>>point.y>>point.z>>yaw>>pitch>>roll>>frame>>model),
                "Malformed Sea Breeze scenery capture");
        if(!command) {
            require(phase==int(++phases),"Missing Sea Breeze phase");
            auto body=player(point.x,point.z);body.word(0x20)=static_cast<std::uint16_t>(point.y);
            sea.step(body);
            continue;
        }
        const auto poses=sea.poses();
        const auto match=std::find_if(poses.begin(),poses.end(),[&](const auto& p) {
            return command<0 ? p.visual_descriptor==0xb7fc : p.command_offset==unsigned(command);
        });
        if(match==poses.end() || !match->visible || match->position!=point || match->yaw!=yaw ||
           match->pitch!=pitch || match->roll!=roll || match->visual_descriptor!=model ||
           ((frame&128) && match->frame!=(frame&63)))
            throw std::runtime_error("Original Sea Breeze scenery diverged at phase "+std::to_string(phase)+
                                     ", placement "+std::to_string(command));
        ++checks;
    }
    require(phases==650 && checks==442,"Incomplete Sea Breeze scenery capture");
}
void sea_breeze_spray(const srf::Rom& rom) {
    // The original button-input capture brakes on the flooded section. BEEF
    // records the candidate and RNG seed; D57F records each drawn pose.
    std::ifstream input(std::filesystem::path(SRF_TEST_FIXTURES)/"sea_breeze_spray_reference.csv");
    std::string line;require(bool(std::getline(input,line)),"Missing Sea Breeze spray capture");
    srf::NativeRace race(rom,srf::PlayerCar::four_wd,srf::CourseId::sea_breeze);
    srf::WaterSpray spray(rom,race.racers());
    unsigned rolls=0,spawns=0,ripples=0;
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        std::array<std::uint8_t,4> rng{};
        for(auto& byte:rng){unsigned v;require(bool(row>>v),"Invalid spray RNG sample");byte=v;}
        srf::Vertex center;srf::VehicleState body,wheel;int wx,wz,speed,dx,dz,table,size;
        require(bool(row>>center.x>>center.y>>center.z>>wx>>wz>>speed>>dx>>dz>>table>>size),"Invalid spray inputs");
        body.word(0x4a)=speed;body.word(0x2e)=dx;body.word(0x32)=dz;
        wheel.word(0x2e)=wx;wheel.word(0x32)=wz;
        auto particle=spray.spawn(body,wheel,center,srf::advance_random(rng));
        require(bool(particle)==bool(table),"Original spray allocation did not match");
        ++rolls;
        if(!particle)continue;
        ++spawns;ripples+=table==0x9b79;
        for(unsigned frame=0;frame<4;++frame) {
            srf::Vertex pos;require(bool(row>>pos.x>>pos.y>>pos.z),"Missing original spray pose");
            // Original A1AC rendering uses radius 36 and descriptor exponent 1.
            require(particle->sprite.position==pos && particle->sprite.frame==frame &&
                particle->sprite.table==table && particle->sprite.diameter==72+2*size,"Sea Breeze spray pose mismatch");
            require(particle->advance()==(frame<3),"Sea Breeze spray lifetime mismatch");
        }
    }
    require(rolls==164 && spawns==88 && ripples==8,"Incomplete original Sea Breeze spray coverage");
}
}
int wmain() {
    try {
        const auto rom_path=_wgetenv(L"SRF_ROM");
        if (!rom_path) { std::cout<<"Set SRF_ROM to the supplied ROM\n"; return 77; }
        const auto rom=srf::Rom::load(rom_path);
        forest_trees(rom);
        forest_orbit(rom);
        sea_breeze_scenery(rom);
        sea_breeze_spray(rom);
        night_flyovers(rom);
        original_capture(rom);
        night_cruise_roadside(rom);
        original_rotating_capture(rom);
        srf::CourseScenery easy(rom,srf::CourseId::easy_ride);
        require(easy.poses().empty() && easy.sprites().empty(),"Unexpected Easy Ride scenery");
        srf::CourseScenery scenery(rom,srf::CourseId::aqua_tunnel);
        require(scenery.poses().size()==21,"Original fish and rotating placement count");
        const auto initial=snapshot(scenery);
        auto outside=player(-24128,-17408+6160),inside=player(-24128,-17408+6159);
        scenery.step(outside);
        scenery.step(inside);
        require(same_position(scenery.poses()[0],initial[0]),"Contact must use the previous phase; boundary must be strict");
        scenery.step(inside);
        require(!same_position(scenery.poses()[0],initial[0]),"One-unit trigger entry failed");
        scenery.reset();
        require(snapshot(scenery)==initial,"Scenery reset must restore source poses and frames");
        auto reverse=player(); reverse.word(0x4a)=0xffff; reverse.word(0x24)=0xff00;
        scenery.step(reverse);
        require(scenery.poses()[0].yaw==255,"Reverse fish yaw must wrap at 256");
        const auto unmodified=reverse.words;
        scenery.step(reverse);
        require(reverse.words==unmodified,"Scenery mutated vehicle physics");
        scenery.reset(); auto wrapped=player(); scenery.step(wrapped);
        wrapped.word(0x1e)=32767; scenery.step(wrapped);
        require(scenery.poses()[0].position.x==-26288,"Fish approach must use signed16 wrapped subtraction");
        for (const auto [offset,value]:std::array<std::pair<unsigned,unsigned>,8>{{
             {0x16,0},{0x16,3},{0x16,6},{0x16,10},{0x16,0x22},{0x16,0x402},{0x64,0},{0x64,2}}}) {
            scenery.reset(); auto excluded=player(); excluded.word(offset)=static_cast<std::uint16_t>(value);
            scenery.step(excluded); scenery.step(excluded);
            require(same_position(scenery.poses()[0],initial[0]),"Ineligible player activated fish");
        }
        // Move vertically outside the trigger so the fish remain in the active
        // neighborhood throughout all26 departure callbacks, then hold still.
        scenery.reset(); auto center=player(); scenery.step(center);
        auto departed=center; departed.word(0x20)=7000;
        scenery.step(departed); // The preceding contact still follows this phase.
        const auto departure=snapshot(scenery);
        for (unsigned i=0;i<26;++i) scenery.step(departed);
        const auto settled=snapshot(scenery);
        for (unsigned i=0;i<3;++i) {
            require(settled[i].position.z==departure[i].position.z-26*256,"Fish departure length");
            require(settled[i].position.x==departure[i].position.x+(i==0 ? 0 : i==1 ? 26*64 : -26*64),"Fish departure fan");
        }
        scenery.step(departed);
        for (unsigned i=0;i<3;++i) require(same_position(scenery.poses()[i],settled[i]),"Fish tail expired too late");
        scenery.reset();
        for (unsigned phase=1;phase<=128;++phase) {
            scenery.step(center);
            require(scenery.poses()[0].frame==(phase&63)%12,"Original animation counter wrap");
        }
        const auto first=snapshot(scenery); scenery.reset();
        for (unsigned phase=1;phase<=128;++phase) scenery.step(center);
        require(snapshot(scenery)==first,"Scenery replay changed after restart");
        std::cout<<"Fish trigger, reverse, departure, eligibility, animation and restart checks passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
