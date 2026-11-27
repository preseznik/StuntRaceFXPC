#include "srf/streaming.hpp"

#include <algorithm>
#include <stdexcept>

namespace srf {
namespace {
// Floor division is explicit: negative positions must not round toward zero.
int floor_div(int value, int divisor) {
    return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}
std::array<int, 2> cell_components(std::int16_t x, std::int16_t z) {
    return {(floor_div(x, 1024) + 32) & 0xfffe,
            (floor_div(z, 2048) + 16) * 64};
}
void validate_state(const StreamingCourse& course, const CollisionStreamState& state) {
    if (state.objects.size() != course.objects.size())
        throw std::invalid_argument("Collision streaming state belongs to a different course");
}
ActivationMask read_mask(std::span<const std::uint8_t> data, std::size_t offset) {
    if (offset + 4 > data.size()) throw std::runtime_error("Activation mask outside ROM");
    ActivationMask result{data[offset], data[offset + 1], data[offset + 2], data[offset + 3], {}};
    const auto count = static_cast<std::size_t>(result.width) * result.height;
    if (!count || result.width > 32 || result.height > 32 ||
        result.origin_x >= result.width || result.origin_z >= result.height ||
        offset + 4 + count > data.size()) throw std::runtime_error("Invalid original activation mask");
    result.cells.assign(data.begin() + offset + 4, data.begin() + offset + 4 + count);
    return result;
}
}

StreamingCourse extract_easy_ride_streaming(const Rom& rom) {
    return extract_streaming(rom,extract_easy_ride(rom));
}
void CollisionAnimation::reset() {
    visible=current=counter=0;
    pause=hold;
    reversed=false;
}
void CollisionAnimation::advance() {
    if(pause){--pause;return;}
    visible=current;
    if(++counter!=period)return;
    counter=0;
    if(reversed ? current==0 : current+1==frames) {
        reversed=!reversed;
        pause=hold;
    } else if(reversed)--current;
    else ++current;
}
StreamingCourse extract_streaming(const Rom& rom, const Scene& scene) {
    const auto& data = rom.bytes();
    StreamingCourse course{read_mask(data, 0x123bd), read_mask(data, 0x12388), {}};
    if(scene.authored) {
        for(unsigned i=0;i<scene.points.size();++i) {
            const auto a=scene.points[i].position,b=scene.points[(i+1)%scene.points.size()].position;
            course.objects.push_back({i+1,original_spatial_cell((a.x+b.x)/2,(a.z+b.z)/2),0});
        }
        return course;
    }
    for (const auto& object : scene.objects) {
        if (!object.descriptor_address) continue;
        const bool static_shape=object.callback_address==0x08cd2d && object.opcode==2;
        const bool animated_shape=(object.callback_address==0x08caf6 || object.callback_address==0x08ca45) && object.opcode==0x0a;
        if (!static_shape && !animated_shape)
            throw std::runtime_error("Unsupported collision streaming callback");
        course.objects.push_back({object.command_offset,
            original_spatial_cell(static_cast<std::int16_t>(object.position.x),
                                  static_cast<std::int16_t>(object.position.z)), 0});
    }
    return course;
}

std::uint16_t original_spatial_cell(std::int16_t x, std::int16_t z) {
    const auto parts = cell_components(x, z);
    return static_cast<std::uint16_t>(parts[0] + parts[1]);
}

std::vector<ActivationCell> activation_cells(const ActivationMask& mask,
                                            std::int16_t x, std::int16_t z) {
    if (!mask.width || !mask.height || mask.width > 32 || mask.height > 32 ||
        mask.origin_x >= mask.width || mask.origin_z >= mask.height ||
        mask.cells.size() != static_cast<std::size_t>(mask.width) * mask.height)
        throw std::invalid_argument("Invalid activation mask");
    const auto center = cell_components(x, z);
    std::vector<ActivationCell> result;
    for (unsigned row = 0; row < mask.height; ++row) {
        for (unsigned col = 0; col < mask.width; ++col) {
            const auto cx = center[0] + (static_cast<int>(col) - mask.origin_x) * 2;
            const auto cz = center[1] + (static_cast<int>(mask.origin_z) - static_cast<int>(row)) * 64;
            const auto kind = mask.cells[row * mask.width + col];
            if ((static_cast<std::uint16_t>(cx) & 0xffc0U) ||
                (static_cast<std::uint16_t>(cz) & 0xf800U) || kind == 0x2e) continue;
            result.push_back({kind, static_cast<std::uint16_t>(cx + cz)});
        }
    }
    return result;
}

CollisionStreamState reset_collision_stream(const StreamingCourse& course) {
    return {std::vector<StreamObjectState>(course.objects.size()), {}};
}

void refresh_collision_stream(const StreamingCourse& course, CollisionStreamState& state,
                              std::int16_t x, std::int16_t z, bool compact) {
    validate_state(course, state);
    for (const auto cell : activation_cells(compact ? course.compact : course.normal, x, z)) {
        // Original cell chains are traversed in reverse insertion order. Refresh
        // only assigns timers, so retain that order without storing another list.
        for (std::size_t i = course.objects.size(); i-- > 0;) {
            const auto& object = course.objects[i];
            if (object.cell_offset != cell.offset ||
                (cell.kind == 0x23 && (object.activation_flags & 0x10U))) continue;
            if ((state.objects[i].timer & 0x80U) == 0) state.objects[i].timer = 3;
        }
    }
}

void update_collision_stream(const StreamingCourse& course, CollisionStreamState& state) {
    validate_state(course, state);
    for (std::size_t i = 0; i < course.objects.size(); ++i) {
        auto& object = state.objects[i];
        if (!object.timer) continue;
        const auto initialize = (object.timer & 0x80U) != 0;
        if (initialize) object.timer = 3;
        else if (--object.timer == 0) { object.attached = false; continue; }
        if (initialize || !object.initialized) {
            object.initialized = true;
            if (!object.attached) {
                const auto id = course.objects[i].command_offset;
                if (std::find(state.active_command_offsets.begin(), state.active_command_offsets.end(), id)
                    != state.active_command_offsets.end())
                    throw std::logic_error("Collision object reactivated before original GSU cleanup");
                state.active_command_offsets.insert(state.active_command_offsets.begin(), id);
                object.attached = true;
            }
            object.keepalive = true;
        } else if (object.attached) object.keepalive = true;
        else object.timer = 0xff; // D070 requests CD55 initialization next CPU pass.
    }
}

void finish_collision_stream(const StreamingCourse& course, CollisionStreamState& state) {
    validate_state(course, state);
    std::erase_if(state.active_command_offsets, [&](std::uint32_t id) {
        const auto found = std::find_if(course.objects.begin(), course.objects.end(),
            [id](const StreamObject& object) { return object.command_offset == id; });
        if (found == course.objects.end()) throw std::logic_error("Unknown streamed collision object");
        auto& object = state.objects[static_cast<std::size_t>(found - course.objects.begin())];
        const bool remove = !object.keepalive;
        object.keepalive = false;
        return remove;
    });
}
}
