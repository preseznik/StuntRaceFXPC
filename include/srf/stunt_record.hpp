#pragma once
#include "srf/native_race.hpp"
#include <string>

namespace srf {
struct StuntRecord {
    unsigned stars{}, seconds{};
    std::string initials;
    PlayerCar car = PlayerCar::four_wd;
};
}
