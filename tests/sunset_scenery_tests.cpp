#include "srf/scenery.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition,const char* error) {
    if (!condition) throw std::runtime_error(error);
}
using Row=std::array<int,12>;
std::vector<Row> load_reference() {
    std::ifstream file(std::filesystem::path(SRF_TEST_FIXTURES)/"sunset_crossing_reference.csv");
    std::string line;
    require(bool(std::getline(file,line)),"Missing Sunset scenery reference");
    std::vector<Row> rows;
    while (std::getline(file,line)) {
        std::istringstream stream(line);
        Row row{};
        for (std::size_t i=0;i<row.size();++i) {
            if (i) {char separator;require(bool(stream>>separator)&&separator==',',"Invalid reference separator");}
            require(bool(stream>>row[i]),"Invalid reference field");
        }
        rows.push_back(row);
    }
    return rows;
}
srf::VehicleState player(int x,int y,int z) {
    srf::VehicleState result;
    result.word(0x1e)=static_cast<std::uint16_t>(x);
    result.word(0x20)=static_cast<std::uint16_t>(y);
    result.word(0x22)=static_cast<std::uint16_t>(z);
    result.word(0x0c)=400;result.word(0x0e)=240;result.word(0x10)=400;
    result.word(0x16)=0x12;result.word(0x64)=1;
    return result;
}
void compare(srf::CourseScenery& scenery,const std::vector<Row>& reference) {
    // Each actor row is its actual WRAM object after that phase's CPU callbacks.
    unsigned phases=0,poses=0,effects=0,frames_matching=0,display_interrupt_frames=0;
    for (std::size_t i=0;i<reference.size();) {
        const auto& sample=reference[i++];
        require(sample[1]==0 && sample[0]==1635+int(phases),"Missing player phase");
        const auto body=player(sample[2],sample[3],sample[4]);
        scenery.step(body,static_cast<std::uint16_t>(sample[10]));
        require(scenery.effects().size()==std::size_t(sample[11]),"Crossing sound phase");
        if (sample[11]) {require(scenery.effects().front()==0x33,"Crossing sound identity");++effects;}
        std::array<bool,5> visible{};
        while (i<reference.size() && reference[i][1]) {
            const auto& row=reference[i++];
            require(row[0]==sample[0] && row[1]>=1 && row[1]<=5,"Unexpected child record");
            require(scenery.poses().size()==5,"Missing spawned child");
            const auto id=static_cast<std::size_t>(row[1]-1);
            const auto& pose=scenery.poses()[id];
            visible[id]=true;
            if (!(pose.visible && pose.position==srf::Vertex{row[2],row[3],row[4]} &&
                    pose.yaw==row[5] && pose.pitch==row[6] && pose.roll==row[7] &&
                    pose.visual_descriptor==row[9])) {
                std::cerr<<"Phase "<<row[0]<<" child "<<row[1]<<" native Z "<<pose.position.z
                         <<" frame "<<pose.frame<<" expected Z "<<row[4]<<" frame "<<row[8]<<'\n';
                throw std::runtime_error("Original child pose mismatch");
            }
            if (pose.frame==row[8]) ++frames_matching;
            else {
                // The original PPU upload cleared direct-page 00 before D46A
                // on phase 1673. This child starts the last six-frame cycle at
                // 1 instead of the synchronous native phase's 3.
                require(id==3 && sample[0]>=1673 && sample[0]<=1680 &&
                        pose.frame==(row[8]+2)%6,"Unexplained animation frame difference");
                ++display_interrupt_frames;
            }
            ++poses;
        }
        for (std::size_t j=0;j<scenery.poses().size();++j)
            require(j<visible.size() && scenery.poses()[j].visible==visible[j],"Child visibility mismatch");
        ++phases;
    }
    require(phases==51 && poses==161 && effects==1 && frames_matching==153 &&
            display_interrupt_frames==8,"Incomplete original crossing reference");
}
}

int wmain() {
    try {
        const auto path=_wgetenv(L"SRF_ROM");
        if (!path) {std::cout<<"Set SRF_ROM to the supplied ROM\n";return 77;}
        const auto rom=srf::Rom::load(path);
        require(srf::extract_world_model(rom,0xb2d8).frames.size()==6 &&
                srf::extract_world_model(rom,0xb2f4).frames.size()==7,"Crossing animation extraction");
        srf::CourseScenery scenery(rom,srf::CourseId::sunset_valley);
        const auto reference=load_reference();
        compare(scenery,reference);
        scenery.reset();
        require(scenery.poses().empty(),"Reset retained spawned animals");
        compare(scenery,reference);
        scenery.reset();
        auto excluded=player(-13568,0,-29248);
        excluded.word(0x16)|=4;
        for (int i=0;i<10;++i) scenery.step(excluded,2);
        require(scenery.poses().empty(),"Ineligible player spawned animals");
        std::cout<<"161 original animal positions and orientations in 51 phases, both models, spawn sound, despawn and restart matched; "
                 <<"153/161 animation frames exact, 8 differ by the original display interrupt seed\n";
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
