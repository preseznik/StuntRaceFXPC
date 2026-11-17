#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

inline std::vector<std::uint16_t> read_stunt_controls(const std::filesystem::path& path,std::string_view car) {
    std::ifstream input(path);
    std::vector<std::uint16_t> controls;
    std::string line;
    std::getline(input,line);
    while(std::getline(input,line)) {
        std::replace(line.begin(),line.end(),',',' ');
        std::istringstream row(line);
        std::string name;
        unsigned count,buttons;
        if(!(row>>name>>count>>std::hex>>buttons) || !count || count>1200 || buttons>0xffff)
            throw std::runtime_error("Malformed native Stunt Trax input trace");
        if(name==car)controls.insert(controls.end(),count,std::uint16_t(buttons));
    }
    if(controls.empty())throw std::runtime_error("Missing native controls for "+std::string(car));
    return controls;
}
