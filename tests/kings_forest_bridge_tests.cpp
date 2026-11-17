#include "srf/race.hpp"
#include "srf/streaming.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void check(bool condition,const char* message) {
    if(!condition)throw std::runtime_error(message);
}
struct Bridge {
    srf::CollisionShape shape;
    srf::CollisionAnimation clock;
    unsigned placement;
};
Bridge& bridge_at(std::vector<Bridge>& bridges,unsigned placement) {
    const auto it=std::find_if(bridges.begin(),bridges.end(),[placement](const auto& bridge){
        return bridge.placement==placement;
    });
    check(it!=bridges.end(),"Unknown original bridge placement");
    return *it;
}
void check_poses(std::vector<Bridge>& bridges,const std::string& fixture,
                 unsigned expected_updates,unsigned expected_allocations) {
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/(fixture+"_poses.txt"));
    std::string line;
    unsigned updates=0,allocations=0;
    while(std::getline(file,line)) {
        if(line.empty() || line[0]=='#')continue;
        unsigned phase,z,advance,pose,current,reversed;
        std::istringstream row(line);
        check(bool(row>>phase>>z>>advance>>pose>>current>>reversed),"Invalid bridge clock fixture");
        auto& bridge=bridge_at(bridges,z);
        auto& clock=bridge.clock;
        if(advance){clock.advance();++updates;}else ++allocations;
        check((bridge.shape.frame?bridge.shape.frame:clock.visible)==pose && clock.current==current && clock.reversed==bool(reversed),
              "CPU08:CB15 pose, direction or endpoint hold mismatch");
    }
    check(updates==expected_updates && allocations==expected_allocations,"Incomplete original CPU bridge trace");
}
void check_contacts(std::vector<Bridge>& bridges,const std::string& fixture,unsigned queries,
                    unsigned seed_count,std::string_view expected_hash) {
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/(fixture+"_contacts.txt"));
    std::vector<srf::ContactManifold> seeds;
    std::vector<std::uint8_t> words;
    std::string line;
    unsigned count=0;
    const auto append=[&](std::uint16_t word){
        words.push_back(std::uint8_t(word));words.push_back(std::uint8_t(word>>8));
    };
    while(std::getline(file,line)) {
        if(line.empty() || line[0]=='#')continue;
        std::istringstream row(line);
        char kind;unsigned index;
        check(bool(row>>kind),"Invalid bridge query fixture");
        if(kind=='S') {
            check(bool(row>>index) && index==seeds.size(),"Invalid original contact scratch index");
            srf::ContactManifold seed{};
            unsigned offset,value;
            while(row>>offset>>value) {
                check(offset<95 && offset%5>=2 && value<65536,"Invalid scratch normal word");
                seed[offset/5].normal[offset%5-2]=std::int16_t(value);
            }
            seeds.push_back(seed);
        } else {
            unsigned phase,z,frame,x,y,pz;
            check(kind=='Q' && bool(row>>phase>>z>>frame>>x>>y>>pz>>index),"Invalid contact sample");
            auto& shape=bridge_at(bridges,z).shape;
            check(frame<shape.animated_planes->size(),"Invalid original collision pose");
            shape.frame=std::uint8_t(frame);
            const auto result=srf::query_collision_shape(shape,{std::int16_t(x),std::int16_t(y),std::int16_t(pz)},seeds.at(index));
            for(const auto& group:result) {
                append(group.flags);append(std::uint16_t(group.distance));
                for(auto v:group.normal)append(std::uint16_t(v));
            }
            ++count;
        }
    }
    check(count==queries && seeds.size()==seed_count,"Incomplete original collision trace");
    check(srf::sha256(words)==expected_hash,
          "Original 95-word contact results differ");
}
std::vector<Bridge> extract_bridges(const srf::Rom& rom,srf::CourseId course) {
    const auto scene=srf::extract_course_scene(rom,course);
    std::vector<Bridge> bridges;
    for(auto& shape:srf::extract_collision(rom,scene))if(shape.animated_planes) {
        const auto data=rom.slice(shape.command_offset+22,3);
        const auto placement=std::uint16_t(shape.position[course==srf::CourseId::big_ravine?0:2]);
        bridges.push_back({std::move(shape),{data[0],data[1],data[2]},placement});
        bridges.back().clock.reset();
    }
    return bridges;
}
}
int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if(!path){std::cout<<"Set SRF_ROM to the supplied ROM\n";return 77;}
        const auto rom=srf::Rom::load(path);
        auto forest=extract_bridges(rom,srf::CourseId::kings_forest);
        check(forest.size()==2,"King's Forest bridge count changed");
        check_poses(forest,"kings_forest_bridge",719,12);
        check_contacts(forest,"kings_forest_bridge",554,69,
            "79a96716e200308baa9799a59344edf5234bab879d58e26165eef0b8e352a150");
        auto lake=extract_bridges(rom,srf::CourseId::lake_side);
        check(lake.size()==1 && lake.front().shape.descriptor_address==0xcd18 && lake.front().clock.frames==16,
              "Lake Side bridge descriptor or animation count changed");
        check_poses(lake,"lake_side_bridge",194,6);
        check_contacts(lake,"lake_side_bridge",300,31,
            "597f4e72b12ac5ebbdac491dcc389b6b04892de22a769097766d1b954368f9de");
        auto ravine=extract_bridges(rom,srf::CourseId::big_ravine);
        check(ravine.size()==6,"Big Ravine bridge count changed");
        check_poses(ravine,"big_ravine_bridge",472,18);
        check_contacts(ravine,"big_ravine_bridge",544,38,
            "cc5e5d9f4acf8d0ae08ffbaf4efe86967a3483854eec01e9d8818697568b1458");
        std::cout<<"Original bridge clocks and complete contact queries matched on three courses\n";
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
