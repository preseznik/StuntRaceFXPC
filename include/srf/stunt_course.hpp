#pragma once
#include <array>
#include <string_view>

namespace srf {
// Order follows the original Stunt Trax board and the ROM's course tables.
enum class StuntCourse : unsigned { rock_field, up_n_down, ice_dance };
inline constexpr std::array playable_stunt_courses{StuntCourse::rock_field,StuntCourse::up_n_down,StuntCourse::ice_dance};
enum class StuntAttempt : unsigned { first, after_finish, after_timeout };
constexpr unsigned stunt_rom_index(StuntCourse course) {return 12+static_cast<unsigned>(course);}
constexpr std::string_view stunt_course_name(StuntCourse course) {
    constexpr std::array names{"Rock Field","Up'n Down","Ice Dance"};
    return names.at(static_cast<unsigned>(course));
}
constexpr std::string_view stunt_course_id(StuntCourse course) {
    constexpr std::array ids{"rock-field","up-n-down","ice-dance"};
    return ids.at(static_cast<unsigned>(course));
}
}
