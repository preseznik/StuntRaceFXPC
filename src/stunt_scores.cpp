#include "srf/stunt_scores.hpp"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <system_error>

namespace srf {
namespace {
bool valid(const StuntRecord& r) {
    return r.car<=PlayerCar::f_type && r.stars<=40 && r.seconds<=99 && r.initials.size()==3 &&
        std::all_of(r.initials.begin(),r.initials.end(),[](char c){return c>='A' && c<='Z';});
}
unsigned score_key(const StuntRecord& r) {return r.stars*100+r.seconds;}
std::string_view score_header(StuntCourse course) {
    constexpr std::array headers{"ROCK-FIELD-1","UP-N-DOWN-1","ICE-DANCE-1","BLUE-LAKE-1"};
    return headers.at(static_cast<unsigned>(course));
}
}

std::vector<StuntRecord> load_stunt_scores(const std::filesystem::path& path,StuntCourse course) {
    if(!std::filesystem::exists(path))return {};
    std::ifstream in(path);
    std::vector<StuntRecord> scores;
    std::string header;
    unsigned count{};
    if(!(in>>header>>count) || header!=score_header(course) || count>9)throw std::runtime_error("Invalid Stunt Trax records");
    std::array<unsigned,3> ranks{},last{4100,4100,4100};
    for(unsigned i=0;i<count;++i) {
        unsigned car{};
        StuntRecord r;
        if(!(in>>car>>r.initials>>r.stars>>r.seconds) || car>=ranks.size())throw std::runtime_error("Invalid Stunt Trax record");
        r.car=static_cast<PlayerCar>(car);
        if(!valid(r) || ++ranks[car]>3 || score_key(r)>last[car])throw std::runtime_error("Invalid Stunt Trax ranking");
        last[car]=score_key(r);
        scores.push_back(r);
    }
    if(in>>header)throw std::runtime_error("Unexpected Stunt Trax record");
    return scores;
}

std::vector<StuntRecord> save_stunt_score(const std::filesystem::path& path,StuntCourse course,const StuntRecord& score) {
    if(!valid(score))throw std::invalid_argument("Invalid Stunt Trax finish");
    if(!path.parent_path().empty())std::filesystem::create_directories(path.parent_path());
    auto lock_path=path;lock_path+=L".lock";
    struct Lock { HANDLE file; ~Lock(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);} };
    Lock lock{CreateFileW(lock_path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
        nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
    OVERLAPPED range{};
    if(lock.file==INVALID_HANDLE_VALUE || !LockFileEx(lock.file,LOCKFILE_EXCLUSIVE_LOCK,0,1,0,&range))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Opening Stunt Trax records");
    auto scores=load_stunt_scores(path,course);
    // 07:AE07 compares stars, then rest seconds, among the three records for this car.
    scores.push_back(score);
    std::stable_sort(scores.begin(),scores.end(),[](const auto& a,const auto& b){
        return score_key(a)>score_key(b);
    });
    std::array<unsigned,3> ranks{};
    std::erase_if(scores,[&](const auto& entry){return ++ranks[static_cast<unsigned>(entry.car)]>3;});
    auto temp=path;temp+=L".tmp";
    std::ofstream out(temp,std::ios::trunc);
    out.exceptions(std::ios::failbit|std::ios::badbit);
    out<<score_header(course)<<' '<<scores.size()<<'\n';
    for(const auto& r:scores)out<<static_cast<unsigned>(r.car)<<' '<<r.initials<<' '<<r.stars<<' '<<r.seconds<<'\n';
    out.close();
    if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"Replacing Stunt Trax records");
    return scores;
}
}
