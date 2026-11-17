#include "srf/race.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace srf {
RaceClockTuning extract_easy_ride_clock(const Rom& rom) {
    return extract_course_clock(rom,CourseId::easy_ride);
}
RaceClockTuning extract_course_clock(const Rom& rom, CourseId id,std::uint16_t vehicle_definition) {
    const auto decimal=[](std::uint16_t packed) {
        unsigned result=0,multiplier=1;
        for(unsigned digit=0;digit<4;++digit) {
            const auto nibble=(packed>>(digit*4))&15U;
            if(nibble>9)throw std::runtime_error("Invalid original decimal race-clock data");
            result+=nibble*multiplier;
            multiplier*=10;
        }
        return static_cast<std::uint16_t>(result);
    };
    course_definition(id); // Reject unknown selectors before reading ROM tables.
    // Four regular Speed Trax races per class. CPU09:EE5C indexes the
    // checkpoint table by class (0D4A), then by round within it (0D48).
    const unsigned course=static_cast<unsigned>(id),round=course%4,league=course/4;
    // 03:EC43 maps the chosen definition to 0D89. 03:EFBB uses that
    // selector for all three clock entries, independently of menu car order.
    unsigned car;
    switch(vehicle_definition) {
    case 0x81b9:car=0;break;
    case 0x853d:car=1;break;
    case 0x8411:car=2;break;
    default:throw std::invalid_argument("Unsupported race clock car");
    }
    const auto table=Rom::lorom_offset(0x03d813)+course*12+car*3;
    const auto bonus_table=rom.u16(Rom::lorom_offset(0x09f115)+league*2);
    return {decimal(rom.bytes()[table]),
        {decimal(rom.bytes()[table+1]),decimal(rom.bytes()[table+2])},
        decimal(rom.bytes()[Rom::lorom_offset(0x090000U|bonus_table)+round]),
        decimal(rom.u16(Rom::lorom_offset(0x03e857)))};
}

void advance_race_clock(RaceClock& clock,const RaceClockTuning& tuning,std::uint8_t ticks) {
    for(unsigned tick=0;tick<ticks;++tick) {
        clock.elapsed_units=std::min<std::uint32_t>(5999999,clock.elapsed_units+tuning.units_per_tick);
        if(clock.remaining_fraction<tuning.units_per_tick) {
            if(clock.remaining_seconds) {
                --clock.remaining_seconds;
                clock.remaining_fraction=std::uint16_t(clock.remaining_fraction+10000-tuning.units_per_tick);
            } else clock.remaining_fraction=0;
        } else clock.remaining_fraction=std::uint16_t(clock.remaining_fraction-tuning.units_per_tick);
    }
}

void extend_race_clock(RaceClock& clock,std::uint16_t seconds) {
    clock.remaining_seconds=std::uint16_t((unsigned(clock.remaining_seconds)+seconds)%10000);
}

RaceFailure race_failure(std::uint16_t flags) {
    if(!(flags&1))return RaceFailure::none;
    return (flags&0x1000)?RaceFailure::out_of_bounds:RaceFailure::wrecked;
}

void apply_race_pickup(VehicleState& player,RacePickup kind,std::uint8_t fraction) {
    // GSU04:BF51/BF75 apply the fraction to reserve $0180 or damage $0200.
    if(kind==RacePickup::boost) {
        if(player.word(0x40)&1)return; // CPU08:B7E7 prevents boosting a wreck.
        player.word(0x6a)=std::min(384,int(player.word(0x6a))+3*fraction);
    } else {
        player.word(0x82)=std::max(0,int(player.word(0x82))-4*fraction);
    }
}

namespace {
std::uint16_t word(std::int32_t value) { return static_cast<std::uint16_t>(value); }
std::int32_t signed_word(std::uint16_t value) {
    return value < 0x8000U ? value : static_cast<std::int32_t>(value) - 0x10000;
}
bool overflow_add(std::uint16_t a, std::uint16_t b) {
    const auto n = signed_word(a) + signed_word(b);
    return n < -32768 || n > 32767;
}
bool overflow_sub(std::uint16_t a, std::uint16_t b) {
    const auto n = signed_word(a) - signed_word(b);
    return n < -32768 || n > 32767;
}
std::uint16_t abs_word(std::uint16_t value) {
    return (value & 0x8000U) != 0 ? word(-signed_word(value)) : value;
}
void validate_count(std::size_t count) {
    if (count == 0 || count > 1024) throw std::invalid_argument("Invalid course route count");
}
}

std::vector<RoutePoint> extract_easy_ride_route(const Rom& rom) {
    return extract_route(extract_easy_ride(rom));
}
std::vector<RoutePoint> extract_route(const Scene& scene) {
    std::vector<RoutePoint> route;
    route.reserve(scene.points.size());
    for (const auto& p : scene.points) {
        route.push_back({{static_cast<std::int16_t>(p.position.x),
                          static_cast<std::int16_t>(p.position.y),
                          static_cast<std::int16_t>(p.position.z)},
                         p.unknown_word, p.rom_offset});
    }
    return route;
}

std::optional<RouteMatch> nearest_route_point(std::span<const RoutePoint> route,
                                             RacePosition position) {
    validate_count(route.size());
    std::optional<RouteMatch> match;
    std::uint16_t best = 0x7fff;
    for (std::size_t i = 0; i < route.size(); ++i) {
        const auto& p = route[i].position;
        const auto x = word(p[0]), y = word(p[1]), z = word(p[2]);
        const auto qx = word(position[0]), qy = word(position[1]), qz = word(position[2]);
        if (overflow_sub(x, qx)) continue;
        const auto dx = abs_word(word(signed_word(x) - signed_word(qx)));
        // The original does not test Y subtraction or the two doublings for overflow.
        const auto dy4 = word(static_cast<std::int32_t>(abs_word(word(signed_word(y) - signed_word(qy)))) * 4);
        if (overflow_add(dx, dy4)) continue;
        const auto xy = word(static_cast<std::int32_t>(dx) + dy4);
        if (overflow_sub(z, qz)) continue;
        const auto dz = abs_word(word(signed_word(z) - signed_word(qz)));
        if (overflow_add(dz, xy)) continue;
        const auto distance = word(static_cast<std::int32_t>(dz) + xy);
        if (distance >= best) continue;
        best = distance;
        match = RouteMatch{static_cast<std::uint16_t>(i + 1), distance};
    }
    return match;
}

std::uint16_t advance_route_index(std::uint16_t index, std::uint16_t count,
                                  std::uint16_t vehicle_flags) {
    validate_count(count);
    if (index == 0 || index > count) throw std::invalid_argument("Route index outside original course");
    if ((vehicle_flags & 2U) != 0) return index == 1 ? count : static_cast<std::uint16_t>(index - 1);
    return index == count ? 1 : static_cast<std::uint16_t>(index + 1);
}

bool update_route_progress(RouteProgress& p, std::uint16_t count) {
    validate_count(count);
    // CC2E/CC80 return index zero when no point survives the signed-distance tests.
    // CDCA compares that sentinel against the last accepted index like any other word.
    if (p.current_index > count || p.accepted_index > count)
        throw std::invalid_argument("Route progress outside original course");
    if (p.current_index == p.accepted_index) return false;
    const auto difference = signed_word(word(static_cast<std::int32_t>(p.current_index) - p.accepted_index));
    if (difference < 0) {
        if (p.accepted_index != count) return false;
    } else if (difference >= 5) return false;
    p.accepted_index = p.current_index;
    if (p.current_index == 1) p.laps = word(static_cast<std::int32_t>(p.laps) + 1);
    return true;
}

std::uint16_t route_next_distance(std::span<const RoutePoint> route,
                                  std::uint16_t index, std::uint16_t flags,
                                  RacePosition position) {
    validate_count(route.size());
    // CD83 computes the preceding record for index zero; the forward CCA4 path
    // then advances to the first real point.
    const auto next = index==0 && !(flags&2U) ? 1U :
        advance_route_index(index, static_cast<std::uint16_t>(route.size()), flags);
    const auto& target = route[next - 1].position;
    const auto dx = abs_word(word(static_cast<std::int32_t>(position[0]) - target[0]));
    const auto dz = abs_word(word(static_cast<std::int32_t>(position[2]) - target[2]));
    // In normal track range these magnitudes are nonnegative signed words.
    // DIV2 rounds a negative value toward zero; preserve the -32768 edge as well.
    return word(signed_word(dx) / 4 + signed_word(dz) / 4);
}

std::vector<std::uint16_t> rank_racers(std::span<const RouteProgress> racers) {
    if (racers.size() > 16) throw std::invalid_argument("Too many race participants");
    std::vector<std::uint16_t> ranks(racers.size(), 0);
    for (std::size_t rank = 1; rank <= racers.size(); ++rank) {
        std::optional<std::size_t> best;
        for (std::size_t i = 0; i < racers.size(); ++i) {
            if (ranks[i]) continue;
            if (!best) { best = i; continue; }
            const auto& a = racers[i];
            const auto& b = racers[*best];
            if (signed_word(a.laps) > signed_word(b.laps) ||
                (a.laps == b.laps && (a.accepted_index > b.accepted_index ||
                 (a.accepted_index == b.accepted_index && signed_word(a.next_distance) < signed_word(b.next_distance))))) best = i;
        }
        ranks[*best] = static_cast<std::uint16_t>(rank);
    }
    return ranks;
}

LapEvent update_race_completion(RaceCompletion& state, std::uint16_t laps) {
    if (state.finished || static_cast<std::uint8_t>(laps) <= state.acknowledged_laps) return LapEvent::none;
    ++state.acknowledged_laps;
    if (state.acknowledged_laps == 1) return LapEvent::first_lap;
    if (state.acknowledged_laps == 2) return LapEvent::second_lap;
    state.finished = true;
    return LapEvent::finished;
}

namespace {
std::int16_t wrap(std::int32_t value) {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(value));
}
std::int16_t divide_two(std::int16_t value) {
    return value == -1 ? 0 : static_cast<std::int16_t>(value >= 0 ? value / 2 : (value - 1) / 2);
}
std::int16_t divide_power(std::int16_t value, unsigned count) {
    while (count--) value = divide_two(value);
    return value;
}
std::int16_t high_multiply(std::int16_t a, std::int16_t b) {
    const auto product = static_cast<std::uint32_t>(static_cast<std::int32_t>(a) * b);
    return wrap(static_cast<std::int32_t>(product >> 16));
}
std::int16_t dot_high(RacePosition a, RacePosition b) {
    return wrap(static_cast<std::int32_t>(high_multiply(a[0], b[0])) +
                high_multiply(a[1], b[1]) + high_multiply(a[2], b[2]));
}
RacePosition quarter_rotate(RacePosition v, std::uint8_t yaw, bool inverse) {
    if (inverse) yaw = static_cast<std::uint8_t>(0U - yaw);
    switch (yaw) {
    case 0: return v;
    case 0x40: return {wrap(-v[2]), v[1], v[0]};
    case 0x80: return {wrap(-v[0]), v[1], wrap(-v[2])};
    case 0xc0: return {v[2], v[1], wrap(-v[0])};
    default: throw std::invalid_argument("Unvalidated collision orientation");
    }
}
}

std::vector<CollisionShape> extract_easy_ride_collision(const Rom& rom) {
    return extract_collision(rom,extract_easy_ride(rom));
}
std::vector<CollisionShape> extract_collision(const Rom& rom, const Scene& scene) {
    const auto meshes = extract_meshes(rom);
    const auto& bytes = rom.bytes();
    const auto read_word = [&bytes](std::size_t p) {
        if (p + 2 > bytes.size()) throw std::runtime_error("Collision descriptor outside ROM");
        return static_cast<std::uint16_t>(bytes[p] | (static_cast<unsigned>(bytes[p + 1]) << 8));
    };
    std::vector<CollisionShape> result;
    for (const auto& object : scene.objects) {
        if (!object.descriptor_address) continue;
        // Rock Field's C90C/C928 placements have an explicit empty collision
        // stream: end vertices, zero planes, empty face group, end model.
        constexpr std::array<std::uint8_t,6> empty_mesh{0x0c,0x30,0,0x14,0xfe,0};
        if(std::ranges::equal(rom.slice(object.mesh_rom_offset,empty_mesh.size()),empty_mesh))continue;
        const auto found = std::find_if(meshes.begin(), meshes.end(), [&](const Mesh& m) {
            return m.rom_offset == object.mesh_rom_offset;
        });
        const bool fixed_pose=object.opcode==0x0a && object.callback_address==0x08ca45;
        const bool animated=fixed_pose || (object.opcode==0x0a && object.callback_address==0x08caf6);
        // CD2D places the eight-pose DCBC and DA70 meshes on the course
        // without an animation callback. Their contact planes stay in pose zero.
        const bool first_pose=object.callback_address==0x08cd2d &&
            (object.descriptor_address==0xdcbc || object.descriptor_address==0xda70);
        const auto frame_parameter=animated?rom.slice(object.command_offset+22,1)[0]:0;
        const auto expected_frames=first_pose?8:animated?frame_parameter:1;
        if (found == meshes.end() || found->frames.empty() ||
            (fixed_pose ? std::size_t(frame_parameter)>=found->frames.size() :
             found->frames.size()!=std::size_t(expected_frames)))
            throw std::runtime_error("Unsupported original collision mesh");
        const auto descriptor = object.descriptor_address & 0x7fffU;
        const auto exponent = bytes[descriptor + 7];
        if (exponent > 6) throw std::runtime_error("Unsupported original collision scale");
        // Easy Ride's confirmed collision streams contain only byte coordinates.
        auto cursor = found->rom_offset;
        for (;;) {
            if (cursor >= bytes.size()) throw std::runtime_error("Truncated collision stream");
            const auto command = bytes[cursor++];
            if (command == 0x0c) break;
            // The full mesh decoder has validated each 1C animation branch.
            // These original bridge descriptors use signed-byte vertices in every 1C branch.
            if (command==0x1c && (first_pose || (animated && (object.descriptor_address==0xe73c ||
                object.descriptor_address==0xcd18 || object.descriptor_address==0xed5c ||
                object.descriptor_address==0xc810 || object.descriptor_address==0xc7f4 ||
                object.descriptor_address==0xfc58)))) break;
            if (command != 4 && command != 0x38)
                throw std::runtime_error("Unsupported original collision vertex encoding");
            if (cursor >= bytes.size()) throw std::runtime_error("Truncated collision stream");
            const auto count = bytes[cursor++];
            cursor += static_cast<std::uint32_t>(count) * 3;
            if (cursor >= bytes.size()) throw std::runtime_error("Truncated collision stream");
        }
        const auto scene_flags = read_word(object.command_offset + 20);
        std::array<std::uint16_t, 3> extents{};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto base = (scene_flags & 0x200U) ? read_word(descriptor + 10 + axis * 2) : 0;
            extents[axis] = word(base + read_word(object.command_offset + 14 + axis * 2));
        }
        if (object.orientation != 0 && object.orientation != 0x80) std::swap(extents[0], extents[2]);
        CollisionShape shape{object.command_offset, object.descriptor_address,
                             {wrap(object.position.x), wrap(object.position.y), wrap(object.position.z)},
                             extents, object.orientation, static_cast<std::uint16_t>(scene_flags | 0x181U), {}};
        std::vector<std::vector<CollisionPlane>> frames(found->frames.size());
        for (std::size_t f=0;f<frames.size();++f) for (const auto& face : found->polygons) {
            const auto flags = face.attributes[0];
            if (!flags) continue;
            if (((flags & 0x40U) && !animated && !first_pose) || (flags & 31U) == 0 || (flags & 31U) > 19)
                throw std::runtime_error("Unsupported original collision face group");
            const auto& vertices=found->frames[f];
            const auto& v=vertices.at(face.indices.at(0));
            const auto scale = 1 << exponent;
            RacePosition normal{};
            for (std::size_t axis = 0; axis < 3; ++axis)
                normal[axis] = wrap(-static_cast<std::int32_t>(std::bit_cast<std::int8_t>(face.attributes[axis + 3])) * 256);
            if (f && (flags&0x40)) {
                // GSU04:9EE3..A028 normalizes two edges separately, then takes
                // their signed Q15 cross product. Frame zero uses the ROM normal.
                const auto edge=[](Vertex a,Vertex b) {
                    return normalize_vehicle_vector({word(a.x-b.x),word(a.y-b.y),word(a.z-b.z)});
                };
                const auto& v1=vertices.at(face.indices.at(1));
                const auto a=edge(v,v1),b=edge(v1,vertices.at(face.indices.at(2)));
                for(unsigned i=0;i<3;++i) {
                    const auto j=(i+1)%3,k=(i+2)%3;
                    const auto cross=std::int64_t(signed_word(a[j]))*signed_word(b[k])-
                                     std::int64_t(signed_word(a[k]))*signed_word(b[j]);
                    normal[i]=wrap(static_cast<std::int32_t>(cross>>15));
                }
            }
            frames[f].push_back({{wrap(v.x*scale),wrap(v.y*scale),wrap(v.z*scale)},normal,flags});
        }
        shape.planes=std::move(frames.front());
        if(animated || first_pose)shape.animated_planes=std::make_shared<const decltype(frames)>(std::move(frames));
        if(fixed_pose)shape.frame=static_cast<std::uint8_t>(frame_parameter); // CPU08:CA45 → CD2D.
        result.push_back(std::move(shape));
    }
    return result;
}

bool collision_bounds_overlap(RacePosition a, std::array<std::uint16_t, 3> a_extents,
                              RacePosition b, std::array<std::uint16_t, 3> b_extents) {
    // The original checks Z, X, then Y; equality is outside the broadphase.
    for (const auto axis : {2U, 0U, 1U}) {
        const auto difference = wrap(static_cast<std::int32_t>(a[axis]) - b[axis]);
        const auto magnitude = wrap(difference < 0 ? -difference : difference);
        const auto extent = word(static_cast<std::int32_t>(a_extents[axis]) + b_extents[axis]);
        if (wrap(static_cast<std::int32_t>(magnitude) - extent) >= 0) return false;
    }
    return true;
}

ContactManifold query_collision_shape(const CollisionShape& shape, RacePosition point,
                                      ContactManifold scratch) {
    for (auto& group : scratch) { group.flags = 0; group.distance = 32000; }
    for (std::size_t axis = 0; axis < 3; ++axis)
        point[axis] = wrap(static_cast<std::int32_t>(point[axis]) - shape.position[axis]);
    point = quarter_rotate(point, shape.orientation, true);
    const auto& planes=shape.frame && shape.animated_planes ? shape.animated_planes->at(shape.frame) : shape.planes;
    for (const auto& plane : planes) {
        const auto id = plane.flags & 31U;
        if (!id || id > 19 || ((plane.flags & 0x40U) && !shape.animated_planes))
            throw std::invalid_argument("Unsupported collision group");
        auto& group = scratch[id - 1];
        if (group.flags & 0xff00U) continue;
        const auto distance = wrap(static_cast<std::int32_t>(dot_high(plane.normal, plane.point)) - dot_high(plane.normal, point));
        if (distance < 0) {
            group.flags = static_cast<std::uint16_t>((group.flags | (static_cast<unsigned>(plane.flags) << 8)) & 0xff00U);
        } else if (plane.flags & 0x80U) {
            group.flags |= plane.flags;
        } else if (std::abs(static_cast<int>(group.distance)) >= std::abs(static_cast<int>(distance))) {
            group.flags = plane.flags;
            group.distance = distance;
            group.normal = plane.normal;
        }
    }
    return scratch;
}

ContactResponse respond_to_contacts(const ContactManifold& contacts, std::uint8_t orientation,
                                    const ContactResponseInput& input) {
    ContactResponse result;
    auto skip_vertical = input.skip_vertical_plane;
    for (const auto& group : contacts) {
        if (!group.flags) continue;
        result.contact_flags |= group.flags;
        if (group.flags & 0xff00U || group.distance < 0) continue;
        if (std::abs(static_cast<int>(group.distance)) <= std::abs(static_cast<int>(result.minimum_distance)))
            result.minimum_distance = group.distance;
        if (group.distance == 32000) continue;
        auto n = quarter_rotate(group.normal, orientation, false);
        if (!n[0] && !n[2]) {
            if (skip_vertical) continue;
            skip_vertical = true;
            result.vertical_plane_seen = true;
        }
        const auto penetration = wrap(static_cast<std::int32_t>(group.distance) * 16);
        RacePosition doubled{};
        const auto& velocity = !n[1] && input.wall_step ? input.wall_step->velocity : input.velocity;
        for (std::size_t i = 0; i < 3; ++i) doubled[i] = wrap(static_cast<std::int32_t>(velocity[i]) * 2);
        const auto nv = wrap(-dot_high(doubled, n));
        result.normal_velocity = nv;
        result.response_applied = true;
        const auto product = static_cast<std::uint32_t>(static_cast<std::int32_t>(nv) * wrap(input.damping));
        auto magnitude = wrap(static_cast<std::int32_t>((product >> 8) & 65535U));
        if (magnitude < 0) {
            magnitude = wrap(divide_two(magnitude) + penetration);
            if (input.flags & 0x100U) magnitude = 0;
        } else if (magnitude < 2000) {
            magnitude = wrap(static_cast<std::int32_t>(magnitude) + penetration);
        }
        const auto force_y = high_multiply(magnitude, n[1]);
        if (!n[1]) {
            magnitude = wrap(static_cast<std::int32_t>(std::max<int>(nv, 0)) * 4 + penetration);
            const auto extra = high_multiply(wrap(static_cast<std::int32_t>(magnitude) * 2), 4096);
            magnitude = wrap(static_cast<std::int32_t>(magnitude) + extra);
            if (std::abs(static_cast<int>(extra)) > 100)
                result.body_flags |= input.lateral_offset < 0 ? 0x100U : 0x200U;
        }
        if (n[0] || n[2]) result.body_flags |= 0x400U;
        const RacePosition force{high_multiply(magnitude, n[0]), force_y, high_multiply(magnitude, n[2])};
        for (std::size_t i = 0; i < 3; ++i) {
            auto applied = std::clamp<int>(force[i], -5000, 5000);
            if (!n[1] && input.wall_step) {
                const auto spring = wrap(penetration + high_multiply(wrap(penetration * 2), 4096));
                const auto continuous = std::clamp<int>(high_multiply(spring, n[i]), -5000, 5000);
                // Split AFTER each original component limiter. At unity this
                // sums to the exact original response, even across saturation.
                if (input.wall_step->apply_impulse)
                    result.impulse[i] = wrap(result.impulse[i] + applied - continuous);
                else applied = continuous;
            }
            result.force[i] = wrap(static_cast<std::int32_t>(result.force[i]) + applied);
            result.displacement[i] = wrap(static_cast<std::int32_t>(result.displacement[i]) + high_multiply(penetration, n[i]));
        }
    }
    return result;
}

WheelContactTuning extract_wheel_contact_tuning(const Rom& rom,
                                               std::array<std::uint16_t, 3> parameters) {
    WheelContactTuning result;
    const auto bytes = rom.slice(0x16fe, result.sine_bytes.size());
    std::copy(bytes.begin(), bytes.end(), result.sine_bytes.begin());
    result.linear_transfer = parameters[0];
    result.angular_mode = parameters[1];
    result.displacement_limit = parameters[2];
    if (result.angular_mode > 10 || (result.angular_mode & 1))
        throw std::invalid_argument("Unsupported original wheel angular transfer mode");
    return result;
}

namespace {
std::int16_t multiply_q15(std::int16_t a, std::int16_t b) {
    const auto product = static_cast<std::uint32_t>(static_cast<std::int32_t>(a) * b);
    return wrap(static_cast<std::int32_t>(product >> 15));
}
RacePosition transform_force(RacePosition v, const std::array<std::uint16_t, 9>& m, bool transpose) {
    RacePosition out{};
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j)
            out[i] = wrap(out[i] + multiply_q15(v[j], wrap(m[transpose ? i * 3 + j : j * 3 + i])));
    }
    return out;
}
void add_clamped(RacePosition& target, RacePosition value) {
    for (std::size_t i = 0; i < 3; ++i)
        target[i] = wrap(target[i] + std::clamp<int>(value[i], -5000, 5000));
}
void transfer_wheel_forces(VehicleState& body, const VehicleState& wheel,
                           VehicleForces& forces, RacePosition contact_force,
                           const WheelContactTuning& tuning,
                           const std::array<std::uint16_t, 9>& orientation) {
    RacePosition offset{wrap(wheel.word(0x36)), wrap(wheel.word(0x38)), wrap(wheel.word(0x3a))};
    const bool zero = !(offset[0] || offset[1] || offset[2]);
    for (std::size_t i = 0; i < 3; ++i) {
        auto scale = zero ? static_cast<std::int16_t>(32767) :
            wrap(offset[i] * wrap(tuning.linear_transfer));
        if (scale < 0) scale = wrap(-scale);
        forces.words[i] = word(wrap(forces.words[i]) + high_multiply(scale, divide_two(contact_force[i])));
    }
    // 04:9BC6 selects a multiplier before the shared original cross product.
    RacePosition scaled = contact_force;
    for (auto& v : scaled) {
        switch (tuning.angular_mode) {
        case 0: case 10: v = wrap(v * 4); break;
        case 2: v = wrap(v * 2); break;
        case 4: break;
        case 6: v = divide_two(v); break;
        case 8: v = divide_power(v, 2); break;
        default: throw std::invalid_argument("Unsupported wheel angular transfer mode");
        }
    }
    const auto local = transform_force(scaled, orientation, true);
    offset = {wrap(wheel.word(0x3c) * 16),
              wrap(word(wheel.word(0x3e) + body.word(0x5a)) * 16),
              wrap(word(wheel.word(0x40) + body.word(0x5c)) * 16)};
    RacePosition torque{};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto j = (i + 1) % 3, k = (i + 2) % 3;
        const auto difference = static_cast<std::uint32_t>(static_cast<std::int32_t>(offset[k]) * local[j]) -
                                static_cast<std::uint32_t>(static_cast<std::int32_t>(offset[j]) * local[k]);
        torque[i] = wrap(static_cast<std::int32_t>(difference >> 15));
    }
    forces.words[3] = word(wrap(forces.words[3]) + torque[1] * 4);
    forces.words[4] = word(wrap(forces.words[4]) + torque[0] * (tuning.angular_mode == 10 ? 12 : 4));
    forces.words[5] = word(wrap(forces.words[5]) + torque[2] * 4);
}
}

void finish_wheel_contact(VehicleState& body, VehicleState& wheel,
                          VehicleForces& forces, WheelContactWork& work,
                          const VehicleTuning& tuning,
                          const WheelContactTuning& contact_tuning,
                          const std::array<std::uint16_t, 9>& orientation,
                          std::optional<RacePosition> braking_velocity) {
    if (work.minimum_distance == 32000) {
        wheel.word(0x4a) = wheel.word(0x4c) = wheel.word(0x4e) = 0;
        body.word(0x44) = word(body.word(0x44) + 1);
    } else {
        body.word(0x40) &= 0xfeffU;
        work.flags |= 0x10U;
        if (work.flags & 8U) {
            const auto available = high_multiply(wrap(wheel.word(0x50) * 2), wrap(work.friction));
            auto drive = wrap(body.word(0x3c));
            if (wrap(body.word(0x4e) - 2) < 0 && drive > 0 && wrap(available - drive) < 0) {
                drive = available;
                work.flags |= 0x20U;
                body.word(0x56) = word(body.word(0x56) + 8000);
                work.friction = word(divide_two(wrap(work.friction)));
            }
            RacePosition force{};
            for (std::size_t i = 0; i < 3; ++i)
                force[i] = high_multiply(wrap(drive * 2), wrap(orientation[6 + i]));
            add_clamped(work.force, force);
        }
        const auto angle = static_cast<std::uint8_t>(word(wheel.word(0x24) - body.word(0x24)) >> 8);
        const auto sine = static_cast<std::int16_t>(std::clamp<int>(divide_power(wrap(contact_tuning.sine_bytes[angle] * 256), 2), -4000, 4000));
        const auto cosine_word = wrap(contact_tuning.sine_bytes[angle + 64] * 256);
        const auto cosine = wrap(cosine_word < 0 ? -cosine_word : cosine_word);
        const RacePosition velocity{wrap(-wrap(wheel.word(0x2e))), wrap(-wrap(wheel.word(0x30))), wrap(-wrap(wheel.word(0x32)))};
        const auto local_velocity = transform_force(velocity, orientation, true);
        const auto lateral = local_velocity[0], longitudinal = local_velocity[2];
        wheel.word(0x26) = word(wheel.word(0x26) + longitudinal);
        auto lateral_force = wrap((high_multiply(lateral, cosine) + high_multiply(wrap(longitudinal * 2), sine)) * 4);
        auto capacity = wrap(wheel.word(0x50));
        if (!body.word(0x3c)) capacity = wrap(capacity + high_multiply(wrap(capacity * 2), wrap(tuning.at(0x266c))));
        const auto steering = wrap(body.word(0x3a)), angular = wrap(body.word(0x34));
        if ((steering == 0 || ((steering < 0) != (angular < 0))) && (work.flags & 2U))
            capacity = high_multiply(wrap(capacity * 2), wrap(tuning.at(0x266e)));
        if (body.word(0x42) & 12U) {
            if (work.flags & 2U) capacity = wrap(capacity + high_multiply(wrap(capacity * 2), wrap(tuning.at(0x2674))));
            else if (work.flags & 1U) capacity = wrap(capacity + high_multiply(wrap(capacity * 2), wrap(tuning.at(0x2676))));
        }
        if (work.flags & 2U) work.friction = 18000;
        capacity = high_multiply(wrap(capacity * 2), wrap(work.friction));
        if (wrap(abs_word(word(lateral_force)) - capacity) >= 0) {
            if (lateral_force < 0) capacity = wrap(-capacity);
            const auto excess = abs_word(word(lateral_force - capacity));
            lateral_force = capacity;
            if (wrap(tuning.at(0x2680) - excess) < 0) {
                body.word(0x56) = word(body.word(0x56) + excess);
                work.flags |= 0x20U;
            }
        }
        add_clamped(work.local_force, {lateral_force, 0, 0});
        if (body.word(0x48)) {
            // The high-rate solver can supply a current longitudinal damper
            // velocity independently of its lateral/contact measurement history.
            // The original callback uses the unmodified wheel velocity.
            auto brake_longitudinal = longitudinal;
            if (braking_velocity) {
                auto opposing = *braking_velocity;
                for (auto& v : opposing) v = wrap(-v);
                brake_longitudinal = transform_force(opposing, orientation, true)[2];
            }
            const auto brake = high_multiply(wrap(brake_longitudinal * 2), wrap(body.word(0x48)));
            add_clamped(work.local_force, {0, 0, static_cast<std::int16_t>(std::clamp<int>(brake, -200, 200))});
        }
        const auto friction_force = transform_force(work.local_force, orientation, false);
        for (std::size_t i = 0; i < 3; ++i) work.force[i] = wrap(work.force[i] + friction_force[i]);
        if (work.force[0] || work.force[1] || work.force[2] ||
            work.impulse[0] || work.impulse[1] || work.impulse[2]) {
            auto continuous = forces;
            transfer_wheel_forces(body, wheel, forces, work.force, contact_tuning, orientation);
            if (work.impulse[0] || work.impulse[1] || work.impulse[2]) {
                auto spring = work.force;
                for (unsigned axis = 0; axis < 3; ++axis) spring[axis] = wrap(spring[axis] - work.impulse[axis]);
                transfer_wheel_forces(body, wheel, continuous, spring, contact_tuning, orientation);
                // Difference of the actual transfers preserves rounding, cross
                // products and torque wrap. Transferring the impulse alone does not.
                for (unsigned axis = 0; axis < 6; ++axis)
                    forces.impulses[axis] = word(forces.impulses[axis] + forces.words[axis] - continuous.words[axis]);
            }
        }
        const auto limit = wrap(contact_tuning.displacement_limit);
        if (limit < 0) throw std::invalid_argument("Invalid original wheel displacement limit");
        for (std::size_t i = 0; i < 3; ++i)
            wheel.word(0x4a + i * 2) = word(std::clamp<int>(divide_power(work.displacement[i], 3), -limit, limit));
    }
    if (wheel.word(0x46) & 2U) wheel.word(0x24) = word(wheel.word(0x24) + body.word(0x3a));
    wheel.word(0x46) = work.flags;
}

void update_contact_candidates(std::vector<std::uint32_t>& candidates,
                               std::span<const std::uint32_t> overlaps) {
    for (const auto id : overlaps) {
        if (std::find(candidates.begin(), candidates.end(), id) == candidates.end())
            candidates.insert(candidates.begin(), id);
    }
    std::erase_if(candidates, [&](std::uint32_t id) {
        return std::find(overlaps.begin(), overlaps.end(), id) == overlaps.end();
    });
}

WheelContactWork update_wheel_contact(VehicleState& body, VehicleState& wheel,
                                      VehicleForces& forces, const CollisionShape* candidate,
                                      const VehicleTuning& tuning,
                                      const WheelContactTuning& contact_tuning,
                                      const std::array<std::uint16_t, 9>& orientation,
                                      std::optional<RacePosition> braking_velocity,
                                      std::optional<WallContactStep> wall_step) {
    WheelContactWork work;
    work.flags = wheel.word(0x46) & 0xfbffU;
    wheel.word(0x2c) = 0;
    if (work.flags & 0x200U) {
        work.flags |= 0x400U;
        wheel.word(0x46) = work.flags;
        return work;
    }
    wheel.word(0x24) = body.word(0x24);
    wheel.word(0x28) = body.word(0x28);
    if (work.flags & 2U) wheel.word(0x24) = word(wheel.word(0x24) + body.word(0x3a));
    if (work.flags & 0x80U) {
        if (work.flags & 2U) wheel.word(0x24) = word(wheel.word(0x24) + body.word(0x3a));
        wheel.word(0x46) = work.flags;
        return work;
    }
    work.flags &= 0xffcfU;
    auto skip_vertical = false;
    const auto accumulate = [&](const ContactResponse& response) {
        for (std::size_t i = 0; i < 3; ++i) {
            work.force[i] = wrap(work.force[i] + response.force[i]);
            work.impulse[i] = wrap(work.impulse[i] + response.impulse[i]);
            work.displacement[i] = wrap(work.displacement[i] + response.displacement[i]);
        }
        if (response.minimum_distance <= work.minimum_distance)
            work.minimum_distance = response.minimum_distance;
        if (response.response_applied) wheel.word(0x42) = word(response.normal_velocity);
        skip_vertical = skip_vertical || response.vertical_plane_seen;
    };
    const auto response_input = [&]() {
        return ContactResponseInput{{wrap(wheel.word(0x2e)), wrap(wheel.word(0x30)), wrap(wheel.word(0x32))},
                                    wheel.word(0x44), work.flags, wrap(wheel.word(0x3c)), skip_vertical, wall_step};
    };
    bool fallback = !candidate;
    if (candidate) {
        const auto contacts = query_collision_shape(*candidate,
            {wrap(wheel.word(0x1e)), wrap(wheel.word(0x20)), wrap(wheel.word(0x22))});
        const bool hit = std::any_of(contacts.begin(), contacts.end(), [](const auto& c) {
            return c.flags && !(c.flags & 0xff00U);
        });
        std::uint16_t flags = 0;
        if (hit) {
            const auto response = respond_to_contacts(contacts, candidate->orientation, response_input());
            flags = response.contact_flags;
            body.word(0x2c) |= response.body_flags;
            accumulate(response);
        }
        bool generic = false;
        if (hit && flags && !(flags & 31U)) {
            fallback = true;
        } else if (hit && flags && !(flags & 32U)) {
            generic = true;
        } else if (candidate->object_flags & 0x40U) {
            generic = true;
        } else {
            switch (candidate->object_flags & 0x3800U) {
            case 0: fallback = true; body.word(0x2c) &= 0xffbfU; break;
            case 0x0800: wheel.word(0x2c) |= 0x00a0U; break;
            case 0x2000: wheel.word(0x2c) |= 0x2020U; break;
            case 0x1000: wheel.word(0x2c) |= 0x1020U; break;
            case 0x1800: wheel.word(0x2c) |= 0x0820U; break;
            default: generic = true; break;
            }
        }
        if (generic && !(candidate->object_flags & 0x4000U) && (body.word(0x8a) & 0x40U))
            wheel.word(0x2c) |= 0x1000U;
        if (!fallback) body.word(0x2c) &= 0xffbfU;
    }
    if (fallback) {
        const auto surface = body.word(0x8a);
        const std::uint16_t flag = (surface & 4U) ? 0x40U : (surface & 0x20U) ? 0xc0U :
                                  (surface & 0x40U) ? 0x1040U : (surface & 2U) ? 0x840U : 0x60U;
        wheel.word(0x2c) |= flag;
    }
    auto ground_distance = wrap(wheel.word(0x20));
    if (ground_distance >= 0) {
        if (wheel.word(0x2c) & 0x20U) {
            const auto ripple = word(divide_power(wrap(wheel.word(0x1e) + wheel.word(0x22)), 3)) & 15U;
            ground_distance = wrap(ground_distance + 10 + ripple);
        }
        if (ground_distance >= 0) {
            ContactManifold ground{};
            ground[0] = {1, ground_distance, {0, wrap(0x8100), 0}};
            accumulate(respond_to_contacts(ground, 0, response_input()));
        }
    }
    work.friction = forces.contact_friction[1];
    if (wheel.word(0x2c) & 0x20U) {
        work.friction = forces.contact_friction[0];
        body.word(0x46) = word(body.word(0x46) + 1);
        if (!work.force[1]) wheel.word(0x2c) &= 0xffdfU;
    }
    finish_wheel_contact(body, wheel, forces, work, tuning, contact_tuning, orientation, braking_velocity);
    return work;
}

void refresh_vehicle_contact_candidates(const Vehicle& vehicle,
                                        std::span<const CollisionShape> active_shapes,
                                        VehicleContactCandidates& candidates) {
    for (std::size_t i = 0; i < vehicle.wheels.size(); ++i) {
        const auto& wheel = vehicle.wheels[i];
        std::vector<std::uint32_t> overlaps;
        const RacePosition p{wrap(wheel.word(0x1e)), wrap(wheel.word(0x20)), wrap(wheel.word(0x22))};
        const std::array<std::uint16_t,3> extents{wheel.word(0xc), wheel.word(0xe), wheel.word(0x10)};
        for (const auto& shape : active_shapes) {
            if (!(wheel.word(0x1a) & shape.object_flags) &&
                collision_bounds_overlap(p, extents, shape.position, shape.extents))
                overlaps.push_back(shape.command_offset);
        }
        update_contact_candidates(candidates[i], overlaps);
    }
}

void update_vehicle_wheel_contacts(Vehicle& vehicle, VehicleForces& forces,
                                   std::span<const CollisionShape> active_shapes,
                                   VehicleContactCandidates& candidates,
                                   const VehicleTuning& tuning,
                                   const WheelContactTuning& contact_tuning,
                                   const std::array<std::uint16_t, 9>& orientation) {
    vehicle.body.word(0x44) = vehicle.body.word(0x46) = vehicle.body.word(0x56) = 0;
    for (std::size_t i = 0; i < vehicle.wheels.size(); ++i) {
        auto& wheel = vehicle.wheels[i];
        if (wheel.word(0x46) & 0x800U)
            throw std::invalid_argument("Special wheel contact routine A656 is not recovered");
        const CollisionShape* candidate = nullptr;
        if (!candidates[i].empty()) {
            const auto id = candidates[i].front();
            const auto found = std::find_if(active_shapes.begin(), active_shapes.end(), [id](const auto& s) {
                return s.command_offset == id;
            });
            if (found == active_shapes.end()) throw std::logic_error("Stale original collision candidate");
            candidate = &*found;
        }
        update_wheel_contact(vehicle.body, wheel, forces, candidate, tuning, contact_tuning, orientation);
        vehicle.body.word(0x92) |= wheel.word(0x2c);
    }
}

OpponentTuning extract_opponent_tuning(const Rom& rom) {
    OpponentTuning result;
    const auto sine = rom.slice(0x16fe, result.sine_bytes.size());
    std::copy(sine.begin(), sine.end(), result.sine_bytes.begin());
    for (std::size_t i = 0; i < result.direction_angles.size(); ++i)
        result.direction_angles[i] = rom.u16(0x1e40 + i * 2);
    for (std::size_t configuration = 0; configuration < 42; ++configuration) {
        const auto pointer = rom.u16(0x244d1 + configuration * 2);
        if (pointer < 0xc531 || pointer > 0xc585)
            throw std::runtime_error("Original opponent motion table outside expected ROM range");
        for (std::size_t rank = 0; rank < 7; ++rank) {
            const auto offset = 0x20000U + (pointer & 0x7fffU) - 12 + rank * 4;
            result.distant_motion[configuration][rank] = {rom.u16(offset), rom.u16(offset + 2)};
        }
        result.transition_speed[configuration] = rom.u16(0x24781 + configuration * 2);
    }
    return result;
}

std::uint16_t original_direction_angle(std::int16_t x, std::int16_t z,
                                       const OpponentTuning& tuning) {
    auto a = abs_word(word(x));
    auto b = abs_word(word(z));
    std::uint16_t angle;
    if (!b) angle = 0x4000;
    else if (a == b) angle = 0x2000;
    else {
        const bool x_greater = a > b;
        if (x_greater) std::swap(a, b);
        const auto ratio = (static_cast<std::uint32_t>(a) << 14) / b;
        angle = tuning.direction_angles.at(ratio >> 6);
        if (x_greater) angle = word(0x4000 - angle);
    }
    if ((x < 0) != (z < 0)) angle = word(-static_cast<std::int32_t>(angle));
    if (z < 0) angle = word(angle + 0x8000);
    return angle;
}

void update_opponent_route_controls(VehicleState& body, RacePosition target,
                                    std::uint16_t desired_speed,
                                    const OpponentTuning& tuning) {
    auto desired = wrap(desired_speed);
    if (body.word(0x70) & 0x20U) {
        desired = divide_two(desired);
        desired = wrap(desired + divide_two(desired));
    }
    const auto dx = wrap(target[0] - wrap(body.word(0x1e)));
    const auto dz = wrap(target[2] - wrap(body.word(0x22)));
    auto heading_error = wrap(-static_cast<std::int32_t>(original_direction_angle(dx, dz, tuning)) - body.word(0x24));
    if (body.word(0x40) & 0x100U)
        body.word(0x26) = word(wrap(body.word(0x26)) - divide_power(wrap(body.word(0x26)), 2));
    std::uint16_t controls = 0;
    const auto speed = wrap(body.word(0x4a));
    if (wrap(speed - desired) < 0) controls |= wrap(desired - 551) < 0 ? 0x10U : 0x400U;
    else controls |= 0x40U;
    heading_error = wrap(heading_error - wrap(body.word(0x3a)));
    if (heading_error < 0) {
        if (wrap(heading_error + 910) < 0) {
            if (wrap(heading_error + 4004) < 0) controls &= 0xffefU;
            controls |= 2U;
            if (body.word(0x40) & 0x4000U) controls |= 8U;
        }
    } else if (wrap(heading_error - 910) >= 0) {
        if (wrap(heading_error - 4004) >= 0) controls &= 0xffefU;
        controls |= 1U;
        if (body.word(0x40) & 0x4000U) controls |= 4U;
    }
    if (wrap(speed - 200) < 0) controls |= 0x10U;
    if (body.word(0x7c)) {
        body.word(0x7c) = word(body.word(0x7c) - 1);
        controls &= 0xfbefU;
    }
    auto recovery = wrap(body.word(0x7e));
    if (recovery < 0 || wrap(recovery - 20) >= 0) {
        if (recovery >= 0) recovery = -40;
        recovery = wrap(recovery + 1);
        body.word(0x7e) = word(recovery);
        if (wrap(recovery + 20) < 0) {
            controls &= 0xfbafU;
            controls |= 0x20U;
            controls ^= 3U;
        }
    } else if (wrap(speed - 10) < 0) {
        body.word(0x7e) = word(recovery + 1);
    } else if (recovery) {
        body.word(0x7e) = word(recovery - 1);
    }
    if (body.word(0x44)) controls &= 0xfbefU;
    body.word(0x42) = controls;
}

void update_opponent_avoidance(std::span<VehicleState> participants, std::size_t index,
                               const std::array<std::uint16_t, 9>& orientation) {
    if (index >= participants.size()) throw std::invalid_argument("Invalid opponent index");
    auto& body = participants[index];
    const auto initial_controls = body.word(0x42);
    for (std::size_t i = 0; i < participants.size(); ++i) {
        const auto& other = participants[i];
        if (i == index || (other.word(0x16) & 0x400U)) continue;
        if (wrap(body.word(0x4a) - other.word(0x4a)) < 0) continue;
        const auto dx = wrap(other.word(0x1e) - body.word(0x1e));
        const auto dz = wrap(other.word(0x22) - body.word(0x22));
        const auto distance = divide_power(wrap(abs_word(word(dx)) + abs_word(word(dz))), 5);
        const auto vx = wrap(body.word(0x2e) - other.word(0x2e));
        const auto vz = wrap(body.word(0x32) - other.word(0x32));
        auto approach = wrap(multiply_q15(vx, dx) + multiply_q15(vz, dz) + 20);
        if (wrap(approach - distance) < 0) continue;
        const auto side = wrap(multiply_q15(wrap(orientation[0]), dx) + multiply_q15(wrap(orientation[2]), dz));
        if (wrap(abs_word(word(side)) - 512) >= 0) approach = -1;
        if (approach < 0) continue;
        auto controls = body.word(0x42);
        if (side < 0) {
            if (initial_controls & 1U) controls &= 0xfffeU;
            controls |= 2U;
        } else {
            if (initial_controls & 2U) controls &= 0xfffdU;
            controls |= 1U;
        }
        body.word(0x42) = controls;
    }
}

bool update_distant_opponent(Vehicle& vehicle, std::span<const RoutePoint> route,
                             const OpponentTuning& tuning, std::uint8_t configuration,
                             std::uint16_t player_rank) {
    validate_count(route.size());
    if (configuration >= tuning.distant_motion.size())
        throw std::invalid_argument("Unsupported original opponent configuration");
    auto& body = vehicle.body;
    const auto rank_difference = wrap(body.word(0x88) - player_rank);
    if (rank_difference < -3 || rank_difference > 3)
        throw std::invalid_argument("Original opponent rank difference outside four-car race");
    const auto motion = tuning.distant_motion[configuration][static_cast<std::size_t>(rank_difference + 3)];
    auto timer = word(body.word(0x72) + 1);
    body.word(0x72) = timer;
    bool route_changed = false;
    if (!timer || wrap(timer - motion.period) >= 0) {
        if (timer) body.word(0x72) = 0;
        if (body.word(0x58)) {
            body.word(0x58) = advance_route_index(body.word(0x58), static_cast<std::uint16_t>(route.size()), body.word(0x70));
            const auto& target = route[body.word(0x58) - 1].position;
            body.word(0x2e) = word(target[0] - wrap(body.word(0x1e)));
            body.word(0x30) = word(target[1] - 160 - wrap(body.word(0x20)));
            body.word(0x32) = word(target[2] - wrap(body.word(0x22)));
            body.word(0x26) = body.word(0x28) = body.word(0x34) = body.word(0x36) = body.word(0x38) = 0;
            const auto next = advance_route_index(body.word(0x58), static_cast<std::uint16_t>(route.size()), body.word(0x70));
            const auto& facing = route[next - 1].position;
            body.word(0x24) = word(-static_cast<std::int32_t>(original_direction_angle(
                wrap(facing[0] - wrap(body.word(0x1e))), wrap(facing[2] - wrap(body.word(0x22))), tuning)));
            body.word(0x18) = 0;
            for (auto& wheel : vehicle.wheels) wheel.word(0x18) = 0;
            route_changed = true;
        }
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto offset = 0x1e + axis * 2;
        const auto previous = body.word(offset);
        body.word(offset) = word(wrap(body.word(offset)) + high_multiply(wrap(body.word(0x2e + axis * 2)), wrap(motion.fraction)));
        // C618/C625/C632 preserve the previous pose in R1/R2/R3 while SBK
        // writes the interpolated body. C642..C64D copies those old registers.
        for (auto& wheel : vehicle.wheels) wheel.word(offset) = previous;
    }
    return route_changed;
}

bool update_opponent_ai(Vehicle& vehicle, std::span<const RoutePoint> route,
                        const OpponentTuning& tuning, const VehicleTuning& vehicle_tuning,
                        std::uint8_t configuration, std::uint16_t player_rank,
                        std::span<const VehicleState> racers, std::size_t index,
                        VehicleContactCandidates& wheel_candidates,
                        std::array<std::uint16_t, 9>& orientation) {
    if (index >= racers.size() || configuration >= tuning.distant_motion.size())
        throw std::invalid_argument("Invalid original opponent context");
    auto& body = vehicle.body;
    const auto find_route = [&]() {
        const auto match = nearest_route_point(route,
            {wrap(body.word(0x1e)), wrap(body.word(0x20)), wrap(body.word(0x22))});
        body.word(0x58) = match ? match->index : 0;
        return match;
    };
    if (body.word(0x16) & 0x400U) {
        if (body.word(0x18)) body.word(0x1c) |= 0x40U;
        if (!(body.word(0x1c) & 0x10U)) {
            body.word(0x1c) |= 0x10U;
            body.word(0x72) = 0xffffU;
            body.word(0x2e) = body.word(0x30) = body.word(0x32) = 0;
        }
        if (body.word(0x70) & 4U) { find_route(); return true; }
        if (update_distant_opponent(vehicle, route, tuning, configuration, player_rank))
            for (auto& candidates : wheel_candidates) candidates.clear();
        return false;
    }
    if (body.word(0x1c) & 0x10U) {
        body.word(0x1c) &= 0xffefU;
        orientation = vehicle_orientation(body, vehicle_tuning);
        for (auto& wheel : vehicle.wheels) position_vehicle_attachment(body, wheel, orientation);
        if (body.word(0x58)) {
            if (body.word(0x58) > route.size()) throw std::invalid_argument("Opponent route index outside course");
            const auto speed = high_multiply(wrap(route[body.word(0x58) - 1].metadata * 2),
                                              wrap(tuning.transition_speed[configuration]));
            const auto yaw = body.word(0x24) >> 8;
            body.word(0x2e) = word(-high_multiply(wrap(tuning.sine_bytes[yaw] * 256), speed) * 16);
            body.word(0x30) = 0;
            body.word(0x32) = word(high_multiply(wrap(tuning.sine_bytes[yaw + 64] * 256), speed) * 16);
        }
    }
    if (body.word(0x70) & 4U) { find_route(); return true; }
    if (!(body.word(0x2c) & 0x40U)) {
        const auto match = find_route();
        if (match) {
            const auto next = advance_route_index(match->index, static_cast<std::uint16_t>(route.size()), body.word(0x70));
            const auto& target = route[next - 1];
            update_opponent_route_controls(body, target.position, target.metadata, tuning);
        }
    }
    std::vector<VehicleState> current(racers.begin(), racers.end());
    current[index] = body;
    update_opponent_avoidance(current, index, orientation);
    body = current[index];
    return true;
}
}
