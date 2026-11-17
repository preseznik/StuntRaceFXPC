#pragma once
#include "srf/stunt_record.hpp"
#include "srf/stunt_course.hpp"
#include <filesystem>
#include <vector>

namespace srf {
std::vector<StuntRecord> load_stunt_scores(const std::filesystem::path& path, StuntCourse course);
std::vector<StuntRecord> save_stunt_score(const std::filesystem::path& path, StuntCourse course, const StuntRecord& score);
}
