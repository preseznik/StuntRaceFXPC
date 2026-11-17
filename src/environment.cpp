#include "srf/environment.hpp"

#include <bit>
#include <algorithm>

namespace srf {
namespace {
RacePosition position(const VehicleState& body) {
    return {std::bit_cast<std::int16_t>(body.word(0x1e)),
            std::bit_cast<std::int16_t>(body.word(0x20)),
            std::bit_cast<std::int16_t>(body.word(0x22))};
}
void latch_color(EnvironmentVideoState& video) {
    // COLDATA bytes select the components to write. A zero byte is a no-op.
    for(const auto command:video.pending_color) {
        for(unsigned channel=0;channel<3;++channel) {
            if(!(command&(0x20u<<channel)))continue;
            const auto mask=static_cast<std::uint16_t>(31u<<(channel*5));
            video.fixed_rgb555=static_cast<std::uint16_t>((video.fixed_rgb555&~mask)|
                ((command&31u)<<(channel*5)));
        }
    }
    video.active_color_mode=video.color_mode;
    video.visible_flash_window=video.flash_window;
}
void hold_color(EnvironmentVideoState& video,std::uint8_t speed) {
    // High speed selects table entry4, not a four-tick period: BA7C jumps to
    // BAB8's decrement/table lookup, yielding20 video ticks.
    unsigned period=20;
    if(speed&0x80)video.speed_bucket=0x80;
    else {
        const auto bucket=static_cast<std::uint8_t>(speed>>5);
        if(bucket!=video.speed_bucket)video.green_counter=0;
        video.speed_bucket=bucket;
        if(bucket==0) {
            video.green_counter=0;video.green_level=0x40;video.green_step=1;
            return;
        }
        period=36-4*bucket; // Original02:B034: 32,28,24 for buckets1..3.
    }
    video.green_counter=static_cast<std::uint8_t>(video.green_counter+1);
    if(video.green_counter!=period)return;
    video.green_counter=0;
    video.green_level=static_cast<std::uint8_t>(video.green_level+video.green_step);
    if(video.green_level==0x40 || video.green_level==0x42)
        video.green_step=static_cast<std::uint8_t>(-video.green_step);
}
void advance_lightning(EnvironmentState& state,EnvironmentVideoState& video) {
    // 02:BB85/BBaf. On EE the HDMA window stays closed for this frame
    // even though the PPU retains the preceding fixed color and add mode.
    video.flash_window=false;
    if(!(state.flags&0x10))return;
    video.color_mode=4;
    if(++video.flash_tick==2) {
        video.flash_tick=0;
        if(++video.flash_pairs==16) {
            video.flash_pairs=0;video.flash_level=0xf0;video.flash_step=0xfe;
            state.flags&=0xef;
        } else {
            video.flash_level=static_cast<std::uint8_t>(video.flash_level+video.flash_step);
            if(video.flash_level==0xf2 || video.flash_level==0xe8)
                video.flash_step=static_cast<std::uint8_t>(-video.flash_step);
        }
    }
    if(video.flash_level!=0xee) {
        video.pending_color.fill(video.flash_level);
        video.flash_window=true;
    }
}
}
std::uint16_t blend_environment_rgb555(std::uint16_t from,std::uint16_t to,std::uint8_t weight) {
    std::uint16_t result{};
    for(unsigned shift=0;shift<15;shift+=5) {
        const int source=(from>>shift)&31,target=(to>>shift)&31;
        const int product=(target-source)*weight;
        const int scaled=product>=0?product/32:-((-product+31)/32);
        result|=static_cast<std::uint16_t>(((source+scaled)&31)<<shift);
    }
    return result;
}
void advance_environment_video(EnvironmentState& state,EnvironmentVideoState& video,
                               std::uint8_t speed,unsigned ticks) {
    while(ticks--) {
        // The visible frame latches the prior frame's prepared COLDATA values;
        // the consumer at scanline194 prepares the next frame's values.
        latch_color(video);
        if(const auto transition=state.flags&6) {
            video.color_mode=2;
            const auto level=static_cast<std::uint8_t>(video.blue_level+(transition==2?1:-1));
            if(level==(transition==2?0x96:0x7f))state.flags&=0xf9;
            else {
                video.blue_level=level;video.pending_color={0x20,0x40,level};
            }
        }
        if(!(state.flags&6) && (state.flags&8)) {
            video.color_mode=2;
            hold_color(video,speed);
            video.pending_color={0x20,video.green_level,0x95};
        }
        advance_lightning(state,video);
    }
}
CourseEnvironment::CourseEnvironment(const Rom& rom,CourseId course) {
    forest_=course==CourseId::kings_forest;
    white_land_=course==CourseId::white_land;
    night_cruise_=course==CourseId::night_cruise;
    lake_side_=course==CourseId::lake_side;
    big_ravine_=course==CourseId::big_ravine;
    sky_ramp_=course==CourseId::sky_ramp;
    harbor_city_=course==CourseId::harbor_city;
    const auto scene=extract_course_scene(rom,course);
    streaming_=extract_streaming(rom,scene);
    streaming_.objects.clear();
    if(course==CourseId::night_owl) {
        night_.emplace();
        const auto bytes=rom.slice(Rom::lorom_offset(0x02b2d9),night_gradient_.size());
        std::copy(bytes.begin(),bytes.end(),night_gradient_.begin());
    }
    for(const auto& object:scene.objects) {
        const auto callback=object.callback_address;
        switch(callback) {
        case 0x09a907:
            if(!harbor_city_)continue;
            break;
        case 0x09a997:
            if(!sky_ramp_)continue;
            break;
        case 0x09ebe5:
            if(!lake_side_ && !harbor_city_)continue;
            break;
        case 0x09e9e1: case 0x09ea63: case 0x09eae7: case 0x09eb69:
        case 0x09cbdb: case 0x09e781: case 0x09e80d: case 0x09e87f: case 0x09e097:
        case 0x09a32c: case 0x09a3c3:
        case 0x09a788: case 0x09a877: case 0x09a293:
        case 0x09a458: case 0x09a4e7: case 0x09a574:
            break;
        case 0x09aa33: case 0x09a621: case 0x09a6f9: case 0x09dd1e:
        case 0x09aabc: case 0x09ab56: case 0x09e8f1: case 0x09e969: case 0x09dfec:
            if(!big_ravine_)continue;
            break;
        default:
            continue;
        }
        const RacePosition pose{static_cast<std::int16_t>(object.position.x),
            static_cast<std::int16_t>(object.position.y),static_cast<std::int16_t>(object.position.z)};
        std::uint16_t extent=0x0780;
        if(callback==0x09cbdb || callback==0x09a788 || callback==0x09a293 || callback==0x09a997)extent=0x0f00;
        if(callback==0x09e097 || callback==0x09dfec)extent=rom.u16(object.command_offset+14);
        triggers_.push_back({object.command_offset,callback,pose,extent});
        streaming_.objects.push_back({object.command_offset,original_spatial_cell(pose[0],pose[2]),0});
    }
    reset();
}
void CourseEnvironment::reset() {
    state_={};events_={};video_={};speed_=0;
    palette_accumulator_=palette_step_=palette_end_=0;
    fog_accumulator_=forest_?0x0f00:white_land_?0x0a00:0;
    bridge_outside_latched_=false;
    fog_entered_=false;
    have_cell_position_=false;
    // Original grid palette cursors: Night Cruise 10, Harbor City 18.
    skyline_phase_=night_cruise_?9:harbor_city_?17:0;
    skyline_blend_phase_=harbor_city_?skyline_phase_:0;
    if(night_) {
        *night_={};night_->table[0]=32;
        for(std::size_t i=0;i<night_gradient_.size();i+=2) {
            night_->table[i+2]=night_gradient_[i];
            night_->table[i+3]=0xe0;
        }
    }
    dusk_triggered_=false;
    contact_.assign(triggers_.size(),false);latched_=contact_;fired_=contact_;
    stream_=reset_collision_stream(streaming_);
}
void CourseEnvironment::step(const VehicleState& player,std::uint16_t acknowledged_laps,
                             std::uint32_t elapsed_centiseconds) {
    events_={};
    if(night_cruise_ || harbor_city_)skyline_phase_=static_cast<std::uint8_t>((skyline_phase_+1)%22);
    if(night_) {
        night_->height=player.word(0x20);
        if(!night_->vehicle_selected) {
            // 03:EC43 maps F-TYPE/COUPE/4WD to original selectors 0/1/2.
            // 09:EE87 selects the starting band height and 02:B21D its bound.
            if(player.word(0x66)==0x81b9 || player.word(0x66)==0x853d) {
                night_->first_count=18;night_->first_limit=20;
            }
            night_->vehicle_selected=true;
        }
    }
    const int velocity=std::bit_cast<std::int16_t>(player.word(0x4a));
    speed_=static_cast<std::uint8_t>(std::min(255,(velocity<0?-velocity:velocity)*60/256));
    const auto pose=position(player);
    // The Lake Side cell scanner reads the preceding CPU mirror, while GSU
    // broadphase below uses the current body. This matters on first activation
    // of its remote ambience object, before the car enters that object's box.
    const auto cell=(lake_side_ || big_ravine_ || sky_ramp_ || harbor_city_) && have_cell_position_?previous_cell_position_:pose;
    refresh_collision_stream(streaming_,stream_,cell[0],cell[2]);
    previous_cell_position_=pose;have_cell_position_=true;
    update_collision_stream(streaming_,stream_);
    for(std::size_t i=0;i<triggers_.size();++i) {
        const auto& trigger=triggers_[i];
        const bool active=stream_.objects[i].keepalive;
        if(active) {
            if(contact_[i]) {
                switch(trigger.callback_address) {
                case 0x09a997:
                    // A9EC..AA11 selects the white overlay and a 1000 endpoint.
                    fog_entered_=true;state_.flags|=0x80;
                    break;
                case 0x09a621: case 0x09a6f9: case 0x09aa33: case 0x09dd1e: {
                    // These callbacks test bits of the BCD tenths digit at
                    // 0E63. A621 also starts the final-lap palette ramp.
                    const auto callback=trigger.callback_address;
                    if(callback==0x09a621 && acknowledged_laps==2 && !dusk_triggered_) {
                        state_.flags|=0x20;dusk_triggered_=true;
                        palette_step_=0x80;palette_end_=0x2000;
                    } else if(!fired_[i] && (acknowledged_laps==0 ||
                              (callback==0x09a621 && acknowledged_laps==1))) {
                        fired_[i]=true;
                        const unsigned tenths=(elapsed_centiseconds/10)%10;
                        const bool thunder=callback==0x09a6f9 ||
                            (callback==0x09aa33 ? (tenths&3)!=0 : (tenths&1)==0);
                        if(thunder) {state_.flags|=0x10;events_.effects.push_back(0x36);}
                    }
                    break;
                }
                case 0x09e8f1:case 0x09e969:
                    if(!latched_[i])events_.sound_commands.push_back(trigger.callback_address==0x09e8f1?0x11:0x10);
                    break;
                case 0x09ebe5:
                    // EC4C sets RGB555 0800 and the increasing BE0F envelope.
                    fog_entered_=true;state_.flags|=0x80;
                    if(!latched_[i])events_.sound_commands.push_back(0x11);
                    break;
                case 0x09a877:
                    // Night Cruise's first-lap gate programs 1A00/1A02 once.
                    if(!acknowledged_laps && !dusk_triggered_) {
                        state_.flags|=0x20;
                        dusk_triggered_=true;palette_step_=0x0a;palette_end_=0x1ffe;
                    }
                    break;
                case 0x09a907:
                    // A95E..A994 sets Harbor's dusk ramp once on the final lap.
                    if(acknowledged_laps==2 && !dusk_triggered_) {
                        state_.flags|=0x20;
                        dusk_triggered_=true;palette_step_=0x20;palette_end_=0x1f20;
                    }
                    break;
                case 0x09a458:case 0x09a4e7:
                    if(!latched_[i])events_.sound_commands.push_back(trigger.callback_address==0x09a458?0x11:0x10);
                    break;
                case 0x09a574:
                    // A5CB..A61E latches on lap two. This trigger never rearms.
                    if(acknowledged_laps==1 && !dusk_triggered_) {
                        state_.flags|=0xa0;
                        dusk_triggered_=true;palette_step_=0x20;palette_end_=0x2000;
                    }
                    break;
                case 0x09a293:case 0x09a788:
                    // A7CD..A876 signals bridge ambience on each edge, and
                    // starts the two-stage weather change once on lap two.
                    if(!latched_[i])events_.sound_commands.push_back(0x13);
                    bridge_outside_latched_=false;
                    if(trigger.callback_address==0x09a788 && acknowledged_laps==1 && !dusk_triggered_) {
                        state_.flags|=0xa0;
                        dusk_triggered_=true;
                        palette_step_=0x20;palette_end_=0x2000;
                    }
                    break;
                case 0x09ab56:case 0x09e9e1:
                    state_.flags|=0x0a;
                    if(!latched_[i])events_.sound_commands.push_back(0x0f);
                    break;
                case 0x09aabc:case 0x09ea63:
                    state_.flags=static_cast<std::uint8_t>((state_.flags&0xf7)|4);
                    if(!latched_[i])events_.sound_commands.push_back(0x10);
                    break;
                case 0x09eae7:state_.palette_weight=31;state_.flags=0x44;break;
                case 0x09eb69:state_.palette_weight=0;state_.flags=0x0a;break;
                case 0x09e781:
                    // Sunset's checkpoint starts the day-to-dusk ramp once,
                    // when the original lap counter at 0DEA reads one.
                    if(acknowledged_laps==1 && !dusk_triggered_) {
                        state_.flags|=0x20;
                        dusk_triggered_=true;
                        palette_step_=0x80;palette_end_=0x2000;
                    }
                    break;
                case 0x09e80d:state_.flags|=0x40;break;
                case 0x09e87f:state_.flags&=0xbf;break;
                case 0x09cbdb:
                    if(!latched_[i])events_.effects.push_back(0x2e);
                    break;
                case 0x09dfec:case 0x09e097:
                    if(!latched_[i])events_.effects.push_back(0x5a);
                    break;
                case 0x09a32c:case 0x09a3c3:
                    if(!latched_[i]) {
                        const bool increasing=trigger.callback_address==0x09a32c;
                        palette_step_=increasing?0x0200:0xfe00;
                        palette_end_=increasing?0x2000:0xfe00;
                        night_->step=increasing?0x20:0xffe0;
                        state_.flags|=0x21;
                    }
                    break;
                }
            } else if(trigger.callback_address==0x09a997) {
                fog_entered_=false;
            } else if(trigger.callback_address==0x09ebe5) {
                // EC95..ECDF starts the fall when the last contact ends.
                fog_entered_=false;
                if(latched_[i] || (harbor_city_ && !fired_[i]))events_.sound_commands.push_back(0x10);
                fired_[i]=true;
            } else if((trigger.callback_address==0x09a788 || trigger.callback_address==0x09a293)
                      && !bridge_outside_latched_) {
                events_.sound_commands.push_back(0x0e);
                bridge_outside_latched_=true;
            }
            latched_[i]=contact_[i];
        }
        // CPU08:CF92/D01E reports player1 only for a confirmed candidate whose
        // body+16 contains2 and body+64 contains1. Ignore opponents/hidden cars.
        const auto extent=trigger.extent;
        const unsigned mask=(trigger.callback_address==0x09e097 || trigger.callback_address==0x09dfec)?0x0424:0x042d;
        contact_[i]=active && !(mask&player.word(0x16)) &&
            (player.word(0x16)&2) && (player.word(0x64)&1) &&
            collision_bounds_overlap(pose,{player.word(0x0c),player.word(0x0e),player.word(0x10)},
                                     trigger.position,{extent,extent,extent});
    }
    finish_collision_stream(streaming_,stream_);
    // 02:C02A calls the relocated BE0F fog consumer just before BE70.
    // At the terminal phase the second palette ramp advances immediately.
    if((lake_side_ || sky_ramp_ || harbor_city_) && (state_.flags&0x80)) {
        // 02:BE0F holds one step below the programmed endpoint. Falling from
        // zero clears bit 80 and restores the original FFFF overlay target.
        if(fog_entered_)fog_accumulator_=std::min<unsigned>(sky_ramp_?0xf00:0x600,fog_accumulator_+0x100);
        else if(fog_accumulator_)fog_accumulator_-=0x100;
        else state_.flags&=0x7f;
    } else if((forest_ || white_land_) && (state_.flags&0x80)) {
        if(fog_accumulator_==0)state_.flags&=0x7f;
        else fog_accumulator_-=white_land_?0x40:0x80;
    }
    // CPU02:BE70 runs after object callbacks once per original race phase.
    // Equality with the programmed endpoint clears the flag without storing it.
    if((state_.flags&0xa0)==0x20) {
        const auto next=static_cast<std::uint16_t>(palette_accumulator_+palette_step_);
        if(next==palette_end_)state_.flags&=0xdf;
        else palette_accumulator_=next;
        const auto weight=static_cast<std::uint8_t>(palette_accumulator_>>8);
        if(harbor_city_ && weight!=state_.palette_weight)skyline_blend_phase_=skyline_phase_;
        state_.palette_weight=weight;
    }
}
void CourseEnvironment::advance_video(unsigned ticks) {
    if(night_) {
        while(ticks--)advance_night_video();
        return;
    }
    advance_environment_video(state_,video_,speed_,ticks);
}
void CourseEnvironment::advance_night_video() {
    auto& light=*night_;
    // HDMA channel 5 writes the previous preparation to COLDATA each scanline.
    // BG1 occupies display y32..159. After the last run the PPU retains its value.
    light.visible.fill(0);
    if(light.active) {
        unsigned row=0,value=0;
        for(unsigned i=0;i<light.table.size() && light.table[i];i+=2) {
            const unsigned next=row+light.table[i];
            if(light.table[i+1]&0xe0)value=(light.table[i+1]&31)*1057;
            for(;row<next;++row)if(row>=32 && row<160)light.visible[row-32]=value;
        }
        for(;row<160;++row)if(row>=32)light.visible[row-32]=value;
    }
    if(!(state_.flags&1))return;
    const auto next=static_cast<std::uint16_t>(light.level+light.step);
    if(next!=0xe000 && next!=0xef00) {
        light.level=next;
        for(unsigned i=1;i<night_gradient_.size();i+=2)
            if(night_gradient_[i]>=(next>>8))light.table[i+2]=static_cast<std::uint8_t>(next>>8);
    }
    // 02:B18D performs the table advance, then B197 checks the following
    // step against E000. The long-held EF state retains EEE0.
    if(static_cast<std::uint16_t>(light.level+light.step)==0xe000) {
        light.active=false;state_.flags&=0xfe;return;
    }
    // 02:B1BB..B294 uses the unsigned CPU mirror of car Y, not camera pitch.
    if(light.previous_height!=light.height) {
        const bool higher=light.height>light.previous_height;
        const int first=light.first_count+(higher?2:-2),middle=light.middle_count+(higher?-1:1);
        if(first>=16 && first<light.first_limit)light.first_count=static_cast<std::uint8_t>(first);
        if(middle>=23 && middle<40)light.middle_count=static_cast<std::uint8_t>(middle);
    }
    light.previous_height=light.height;
    light.table[0]=light.first_count;
    light.table[32]=light.middle_count;
    light.active=true;
}
EnvironmentPresentation CourseEnvironment::presentation() const {
    // CGADSUB83 subtracts fixed RGB555 from BG1+BG2 inside the game viewport;
    // OBJ/HUD is excluded. No half-color operation or full-screen alpha tint.
    EnvironmentPresentation result{video_.fixed_rgb555,state_.palette_weight,
        static_cast<std::uint8_t>(video_.active_color_mode==2?3:0),
        bool(state_.flags&0x40),bool(state_.flags&8)};
    if(video_.active_color_mode==4 && video_.visible_flash_window) {
        result.color_math_layers=3;
        result.additive_fixed_color=true;
    }
    if(night_) {result.fixed_rgb555=0;result.row_subtract_rgb555=night_->visible;}
    result.fog_weight=static_cast<std::uint8_t>(fog_accumulator_>>8);
    result.skyline_phase=skyline_phase_;
    result.skyline_blend_phase=skyline_blend_phase_;
    if((lake_side_ || harbor_city_) && (state_.flags&0x80))result.fog_rgb555=0x0800;
    if((sky_ramp_ || harbor_city_) && !(state_.flags&0x80))result.fog_rgb555=0xffff;
    return result;
}
}
