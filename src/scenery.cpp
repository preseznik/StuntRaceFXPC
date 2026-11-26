#include "srf/scenery.hpp"
#include "srf/presentation.hpp"
#include "srf/sunset_rocks.hpp"

#include <algorithm>
#include <cstdlib>

namespace srf {
namespace {
// CPU02:D7E0 initializes the phase with 2. The original display upload may
// interrupt and clear that scratch byte before D1BF, D40C or D46A reads it.
// Native presentation and simulation run independently: use the phase seed.
constexpr unsigned crossing_animation_seed=2;
int signed_word(int value) {
    const auto word=static_cast<std::uint16_t>(value);
    return word<0x8000 ? word : int(word)-0x10000;
}
int approach(int from,int target,int divisor) {
    int difference=signed_word(target-from);
    if (!difference) return from;
    // CPU03:CDC1/CDF4 clamp a nonzero delta before division, including negative
    // deltas. C++ signed division supplies the original truncation toward zero.
    if (std::abs(difference)<divisor) difference=difference<0 ? -divisor : divisor;
    return signed_word(from+difference/divisor);
}
bool contact(const VehicleState& body,const Vertex& trigger,int extent,unsigned excluded=0x042d) {
    // Original candidate eligibility, followed by CF92's player identity mask.
    if ((body.word(0x16)&excluded) || !(body.word(0x16)&2) || !(body.word(0x64)&1)) return false;
    const int center[]{trigger.x,trigger.y,trigger.z};
    for (unsigned axis=0;axis<3;++axis) {
        const int difference=signed_word(int(body.word(0x1e + 2*axis))-center[axis]);
        if (std::abs(difference)>=extent+int(body.word(0x0c+2*axis))) return false;
    }
    return true;
}
bool activate(std::span<const ActivationCell> cells,const Vertex& position,std::uint8_t& timer) {
    const auto cell=original_spatial_cell(static_cast<std::int16_t>(position.x),
                                          static_cast<std::int16_t>(position.z));
    if (std::any_of(cells.begin(),cells.end(),[cell](const ActivationCell& value) {
            return value.offset==cell;
        })) timer=3;
    return timer && --timer;
}
int signed_byte(int value) {
    const auto byte=static_cast<std::uint8_t>(value);
    return byte<128 ? byte : int(byte)-256;
}
// CPU03:C27F/C3FF/C57C use the SNES unsigned multiplier on twice the
// magnitude, then restore the sign. Each product truncates before summation.
int product(int coordinate,int coefficient) {
    const int magnitude=((2*std::abs(coordinate))&255)*std::abs(coefficient)/256;
    return (coordinate<0)!=(coefficient<0) ? -magnitude : magnitude;
}
std::array<int,2> rotate_pair(int first,int second,int sine,int cosine) {
    return {signed_byte(product(first,cosine)+product(second,sine)),
            signed_byte(-product(first,sine)+product(second,cosine))};
}
}

CourseScenery::CourseScenery(const Rom& rom,CourseId course,bool easy_ride_demo) {
    const auto scene=extract_course_scene(rom,course);
    activation_=extract_streaming(rom,scene).normal;
    for (const auto& source:scene.objects) {
        if(course==CourseId::sky_ramp && (source.callback_address==0x0996fc ||
           source.callback_address==0x099c4a || source.callback_address==0x099c97))
            sky_gates_.push_back({source});
        if(source.callback_address==0x098c16)lake_actor_=LakeActor{source};
        if(source.callback_address==0x098bd4 && course==CourseId::night_cruise) {
            auto p=source.position;
            p.y-=380;
            const auto descriptor=source.template_word&0x7fff;
            const auto diameter=original_sprite_diameter(rom.u16(descriptor+16),rom.bytes()[descriptor+7],80);
            sprites_.push_back({source.command_offset,p,0x9973,static_cast<std::uint16_t>(diameter)});
        }
        if((course==CourseId::kings_forest || (course==CourseId::easy_ride && easy_ride_demo)) &&
           (source.callback_address==0x098b3b || source.callback_address==0x09e0ff)) {
            // 09:8B3B and E0FF install a trunk, then spawn a screen-facing
            // child at the same X/Z. CPU03:D1E6 copies the *adjusted* parent Y.
            const bool tall=source.callback_address==0x098b3b;
            const auto sprite=[&](unsigned descriptor,unsigned table,int y,unsigned size) {
                const auto data=descriptor&0x7fff;
                const auto diameter=original_sprite_diameter(rom.u16(data+16),rom.bytes()[data+7],
                                                             static_cast<std::uint8_t>(size));
                auto position=source.position;position.y+=y;
                sprites_.push_back({source.command_offset,position,static_cast<std::uint16_t>(table),
                                    static_cast<std::uint16_t>(diameter)});
            };
            sprite(tall?0xa104:0xa0cc,0x9971,-140,tall?60:100);
            sprite(tall?0xa120:0xa0e8,0x9973,tall?-1540:-760,100);
            if(!tall)interactive_trees_.push_back({source.command_offset,sprites_[sprites_.size()-2].position});
        }
        if (source.callback_address==0x09d13f) crossing_trigger_=source;
        if (source.callback_address==0x09c870) flyover_trigger_=source;
        if (source.callback_address==0x09e53c) {
            trigger_=source.position;
            has_trigger_=true;
        }
        if (source.callback_address!=0x09e5bd && source.callback_address!=0x09e641 &&
            source.callback_address!=0x09e6e1) continue;
        fish_.push_back({{source.command_offset,source.template_word,source.position,
                          source.orientation,0,0,0,false},source.callback_address});
    }
    for (const auto& source:scene.objects) {
        if((course==CourseId::sea_breeze || course==CourseId::white_land || course==CourseId::night_cruise ||
            course==CourseId::lake_side || course==CourseId::big_ravine || course==CourseId::sky_ramp ||
            course==CourseId::harbor_city)
            && source.callback_address==0x09abf3) {
            // 09:AC06 creates B7FC in single-player and copies XYZ/yaw. The
            // post still uses its own course model (EE04 on Night Cruise).
            auto p=source.position;p.y-=480;
            checkpoint_panel_=SceneryPose{0,0xb7fc,p,source.orientation};
        }
        if (source.callback_address!=0x09cab1 && source.callback_address!=0x09cea8 &&
            source.callback_address!=0x09cfe3 &&
            source.callback_address!=0x09c9b0) continue;
        rotating_.push_back({{source.command_offset,source.template_word,source.position,
                              source.orientation,0,0,0,false},source.callback_address});
    }
    if(!interactive_trees_.empty())tree_surfaces_=extract_collision(rom,scene);
    for (unsigned i=0;i<256;++i) {
        sine_[i]=static_cast<std::int8_t>(rom.bytes()[0x16fe + i]);
        cosine_[i]=static_cast<std::int8_t>(rom.bytes()[0x173e + i]);
    }
    reset();
}

void CourseScenery::reset() {
    poses_.clear();
    sky_flights_.clear();
    for(auto& gate:sky_gates_) {gate.timer=0;gate.last_lap=4;gate.contacts=0;}
    for (auto& fish:fish_) {
        fish.tail=fish.active_timer=0;
        poses_.push_back(fish.initial);
    }
    for (auto& actor:rotating_) {
        actor.active_timer=actor.pivot_timer=actor.yaw=actor.pitch=actor.roll=0;
        actor.initialized=actor.visible_latch=false;
        poses_.push_back(actor.initial);
        if(actor.callback==0x09c9b0)poses_.back().visible=true;
    }
    for(auto& tree:interactive_trees_) {
        tree.timer=tree.life=0;tree.contact=tree.latched=tree.shed=false;
    }
    if(checkpoint_panel_) {
        checkpoint_timer_=0;
        poses_.push_back(*checkpoint_panel_);
    }
    if(lake_actor_) {
        *lake_actor_=LakeActor{lake_actor_->trigger};
        auto p=lake_actor_->trigger.position;
        p.x-=1760;p.y+=160;p.z+=3840;
        poses_.push_back({0,0xb754,p,32,0,0,0,false});
    }
    crossing_started_=crossing_contact_=false;
    crossing_timer_=0;
    crossing_animals_={};
    flyover_trigger_timer_=flyover_timer_=flyover_life_=0;
    flyover_contact_=false;flyover_lap_=4;
    effects_.clear();
    previous_player_.reset();
    trigger_timer_=0;
    following_=false;
    rotating_started_=false;
    phase_=0;
}

void CourseScenery::step(const VehicleState& player,std::uint16_t acknowledged_laps,std::span<const Vehicle> other_sky_drivers) {
    // 08:CF92 consumes the preceding GSU phase's candidate. Reconstruct its
    // trigger-only AABB test without inserting a body into racing broadphase.
    const auto& previous=previous_player_ ? *previous_player_ : player;
    auto cells=activation_cells(activation_,
        static_cast<std::int16_t>(previous.word(0x1e)),
        static_cast<std::int16_t>(previous.word(0x22)));
    if (has_trigger_ && activate(cells,trigger_,trigger_timer_)) following_=contact(previous,trigger_,0x1680);
    effects_.clear();
    ++phase_;
    for (std::size_t i=0;i<fish_.size();++i) {
        auto& fish=fish_[i]; auto& pose=poses_[i];
        pose.visible=activate(cells,pose.position,fish.active_timer);
        // CPU02:CDA0 and GSU01:884F select the global counter's lower six
        // bits before modulo twelve. Native race boot defines counter zero;
        // the original menu/countdown history is not synthesized here.
        pose.frame=(phase_&63)%12;
        if (!pose.visible) continue;
        if (following_) {
            pose.yaw=static_cast<std::uint8_t>((player.word(0x24)>>8)+128+
                (signed_word(player.word(0x4a))<0 ? 128 : 0));
            pose.position.x=approach(pose.position.x,signed_word(player.word(0x1e)),4);
            pose.position.z=approach(pose.position.z,signed_word(player.word(0x22)),2);
            const bool middle=fish.callback==0x09e5bd;
            pose.position.z=signed_word(pose.position.z+(middle ? -3 : 3));
            if (!middle) pose.position.x=signed_word(pose.position.x+
                (fish.callback==0x09e641 ? -30 : 30));
            pose.roll=0; // Original object+16 is roll; yaw lives at+15.
            fish.tail=26;
        } else if (fish.tail) {
            --fish.tail;
            pose.position.z=signed_word(pose.position.z-256);
            if (fish.callback!=0x09e5bd) pose.position.x=signed_word(pose.position.x+
                (fish.callback==0x09e641 ? 64 : -64));
        }
    }
    // The original first scene update refreshes CPU cells at (0,0), before the
    // newly initialized GSU vehicle position has been copied into its CPU actor.
    // Preserve that one setup pass; subsequent refreshes use the latched player.
    if (!rotating_started_) {
        step_rotating(activation_cells(activation_,0,0));
        rotating_started_=true;
    } else step_rotating(cells);
    if(checkpoint_panel_) {
        auto& panel=poses_[rotating_.size()];
        // AD50 updates the panel once on allocation, including the setup
        // wake at boot and the next phase's three-tick activation timer.
        if(phase_==1)checkpoint_timer_=3;
        if(activate(cells,panel.position,checkpoint_timer_))panel.pitch-=8;
    }
    if (crossing_trigger_) step_crossing(cells,player,acknowledged_laps);
    if (flyover_trigger_) step_flyover(cells,player,acknowledged_laps);
    if(!interactive_trees_.empty())step_trees(cells,player);
    if(lake_actor_)step_lake_actor(cells,previous,acknowledged_laps);
    if(!sky_gates_.empty()){
        for(const auto& car:other_sky_drivers){
            const auto area=activation_cells(activation_,signed_word(car.body.word(0x1e)),signed_word(car.body.word(0x22)));
            cells.insert(cells.end(),area.begin(),area.end());
        }
        step_sky_flights(cells,player,acknowledged_laps,other_sky_drivers);
    }
    previous_player_=player;
}

void CourseScenery::step_sky_flights(std::span<const ActivationCell> cells,const VehicleState& player,
                                     std::uint16_t player_lap,std::span<const Vehicle> others) {
    for(auto& gate:sky_gates_) {
        const auto& origin=gate.source.position;
        const bool active=activate(cells,origin,gate.timer);
        unsigned laps=(gate.contacts&1)?player_lap:4;
        auto contacts=unsigned(active && contact(player,origin,3840));
        for(unsigned i=0;i<others.size();++i){
            const auto bit=2u<<i;
            const auto& car=others[i].body;
            if(gate.contacts&bit)laps=laps==4?car.word(0x7a):std::max<unsigned>(laps,car.word(0x7a));
            if(active && contact(car,origin,3840))contacts|=bit;
        }
        gate.contacts=contacts;
        const bool crossed=active && gate.last_lap!=laps && (others.empty() || gate.last_lap==4 || laps>gate.last_lap);
        if(!crossed || laps>=3)continue;
        gate.last_lap=static_cast<std::uint8_t>(laps);
        const auto spawn=[&](Vertex offset,Vertex target,unsigned yaw,int y_offset,int z_offset,
                             int forward_step,unsigned approach_ticks,unsigned life) {
            const Vertex p{signed_word(origin.x+offset.x),signed_word(origin.y+offset.y),signed_word(origin.z+offset.z)};
            sky_flights_.push_back({poses_.size(),target,y_offset,z_offset,forward_step,
                                   static_cast<std::uint8_t>(approach_ticks),static_cast<std::uint8_t>(life)});
            poses_.push_back({gate.source.command_offset,0xaa34,p,static_cast<std::uint8_t>(yaw),0,0,0,true});
            effects_.push_back(0x0e);
        };
        if(gate.source.callback_address==0x0996fc) {
            // 09:9793/992F allocate three aircraft on the first and last laps.
            if(laps==1)continue;
            const bool last=laps==2;
            constexpr std::array y{-1920,-960,-1600},z{1280,-1920,0};
            constexpr std::array first_dy{-672,-704,-736},last_dy{-576,-608,-576};
            constexpr std::array first_dz{0,-512,512},last_dz{0,-256,256};
            for(unsigned i=0;i<3;++i)
                spawn({last?9600:-15360,y[i],z[i]},origin,last?64:192,last?last_dy[i]:first_dy[i],
                      last?last_dz[i]:first_dz[i],last?-896:256,last?6:9,last?18:5);
        } else if(laps==1) {
            // 09:9D76/9E08 move the CPU parent before the child approaches it.
            // The nonsolid GSU box stays at its original position until then.
            const bool north=gate.source.callback_address==0x099c4a;
            const Vertex target{signed_word(origin.x-3840),origin.y,signed_word(origin.z+(north?3840:-3840))};
            spawn({8960,-960,north?-7680:7680},target,north?32:96,-160,0,0,0,north?10:12);
        }
    }
    for(auto& flight:sky_flights_) {
        auto& pose=poses_[flight.pose];
        pose.visible=flight.life && activate(cells,pose.position,flight.timer);
        if(!pose.visible)continue;
        auto& p=pose.position;
        if(flight.approach_ticks || !flight.forward_step) {
            p.x=approach(p.x,flight.target.x,4);
            p.z=signed_word(approach(p.z,flight.target.z,4)+flight.z_offset);
            p.y=signed_word(approach(p.y,flight.target.y,flight.forward_step?2:4)+flight.y_offset);
            if(flight.approach_ticks) {--flight.approach_ticks;continue;}
        } else {
            p.x=signed_word(p.x+flight.forward_step);
            p.z=signed_word(p.z+flight.z_offset);
        }
        if(!--flight.life)pose.visible=false;
    }
}

void CourseScenery::step_lake_actor(std::span<const ActivationCell> cells,const VehicleState& player,std::uint16_t laps) {
    auto& actor=*lake_actor_;
    auto& pose=poses_.back();
    // 8C16 allocates a child the first time the parent enters a live cell.
    if(activate(cells,actor.trigger.position,actor.trigger_timer)) {
        if(!actor.allocated){actor.allocated=true;actor.actor_timer=3;}
        actor.near=contact(player,actor.trigger.position,3840);
    }
    if(!actor.allocated || !activate(cells,pose.position,actor.actor_timer)) {
        pose.visible=false;return;
    }
    if(laps==1)actor.revealed=true;
    pose.visible=actor.revealed;
    if(laps!=1 && laps!=2)return;
    if(laps==2 && actor.moves) {
        --actor.moves;
        pose.position.x=signed_word(pose.position.x+90);
        pose.yaw=static_cast<std::uint8_t>(approach(pose.yaw,64,4));
    }
    // 8D34 samples the parent's confirmed proximity once every three active
    // child calls. The original ten-frame vertex stream reverses on exit.
    if(actor.delay){--actor.delay;return;}
    actor.delay=2;
    if(actor.near) {
        if(!actor.calling)effects_.push_back(0x32);
        if(pose.frame<9)++pose.frame;
    } else if(pose.frame)--pose.frame;
    actor.calling=actor.near;
}

void CourseScenery::step_trees(std::span<const ActivationCell> cells,const VehicleState& player) {
    // 09:E1CE emits 58 on each contact edge. Four leaves are allocated only
    // on the first strike. They use the same GSU 04:907A routine as the rocks.
    const auto base=fish_.size()+rotating_.size();
    poses_.resize(base);
    for(auto& tree:interactive_trees_) {
        const bool active=activate(cells,tree.position,tree.timer);
        if(active && tree.contact && !tree.latched) {
            effects_.push_back(0x58);
            if(!tree.shed) {
                tree.shed=true;tree.life=15;
                constexpr int heights[]{-90,-160,-90,-180};
                constexpr Vertex impulses[]{{35,-2,10},{-30,-1,15},{30,-2,-13},{-35,-1,-10}};
                for(unsigned i=0;i<tree.fragments.size();++i) {
                    auto& body=tree.fragments[i];body={};
                    body.word(0x16)=8;body.word(0x1a)=0x0424;
                    body.word(0x1e)=static_cast<std::uint16_t>(tree.position.x);
                    body.word(0x20)=static_cast<std::uint16_t>(tree.position.y+heights[i]);
                    body.word(0x22)=static_cast<std::uint16_t>(tree.position.z);
                    body.word(0x2e)=static_cast<std::uint16_t>(impulses[i].x*8);
                    body.word(0x30)=static_cast<std::uint16_t>(impulses[i].y*8);
                    body.word(0x32)=static_cast<std::uint16_t>(impulses[i].z*8);
                }
            }
        }
        if(active)tree.latched=tree.contact;
        tree.contact=active && contact(player,tree.position,80,0x0424);
        if(!tree.life || !--tree.life)continue;
        const auto angle=static_cast<std::uint8_t>((15-tree.life)*24);
        for(auto& body:tree.fragments) {
            const RacePosition position{static_cast<std::int16_t>(body.word(0x1e)),
                static_cast<std::int16_t>(body.word(0x20)),static_cast<std::int16_t>(body.word(0x22))};
            poses_.push_back({tree.command_offset,0xabf4,{position[0],position[1],position[2]},
                              angle,angle,angle,0,true});
            std::vector<RockContact> terrain;
            for(const auto& shape:tree_surfaces_) {
                if(!(shape.object_flags&0x0424) && collision_bounds_overlap(position,{0,0,0},
                        shape.position,shape.extents))terrain.emplace_back(&shape);
            }
            advance_sunset_rock(body,terrain);
        }
    }
}

void CourseScenery::step_flyover(std::span<const ActivationCell> cells,const VehicleState& player,
                                 std::uint16_t laps) {
    const auto& trigger=*flyover_trigger_;
    const bool active=activate(cells,trigger.position,flyover_trigger_timer_);
    const bool spawn=active && flyover_contact_ && laps<3 && laps!=flyover_lap_;
    flyover_contact_=active && contact(player,trigger.position,1920);
    // CPU 09:C694, C6FB and C762. Each lap crosses the field from a different
    // heading and offset. None of the children allocates a GSU collision body.
    constexpr std::array<Vertex,3> offset{{{3840,-448,-10240},{-2560,-480,-11200},{960,-480,-12160}}};
    constexpr std::array<Vertex,3> velocity{{{-180,-10,0},{260,-10,200},{0,-13,200}}};
    constexpr std::array<std::uint8_t,3> yaw{64,224,0};
    if(spawn) {
        flyover_lap_=static_cast<std::uint8_t>(laps);
        flyover_life_=laps==0?28:22;
        flyover_timer_=3;
        const auto delta=offset[laps];
        const Vertex p{signed_word(trigger.position.x+delta.x),signed_word(trigger.position.y+delta.y),
                       signed_word(trigger.position.z+delta.z)};
        poses_.push_back({trigger.command_offset,0xb770,p,yaw[laps],248,0,0,true});
        effects_.push_back(0x52);
    }
    if(!flyover_life_)return;
    auto& pose=poses_.back();
    if(!activate(cells,pose.position,flyover_timer_))return;
    const auto delta=velocity[flyover_lap_];
    pose.position.x=signed_word(pose.position.x+delta.x);
    pose.position.y=signed_word(pose.position.y+delta.y);
    pose.position.z=signed_word(pose.position.z+delta.z);
    if(!--flyover_life_)poses_.pop_back();
}

void CourseScenery::step_crossing(std::span<const ActivationCell> cells,const VehicleState& player,
                                  std::uint16_t acknowledged_laps) {
    const auto& trigger=*crossing_trigger_;
    if (!crossing_started_) {
        const bool active=activate(cells,trigger.position,crossing_timer_);
        // CPU09:D184 consumes the previous contact and removes the trigger on lap three.
        const bool spawn=active && crossing_contact_ && acknowledged_laps==2;
        crossing_contact_=active && contact(player,trigger.position,0x2a80);
        if (!spawn) return;
        crossing_started_=true;
        effects_.push_back(0x33);
        // D1AB..D3AE creates all five children from the same trigger position.
        constexpr int x_offsets[]{0,-640,-1280,-1600,-2240};
        constexpr int z_offsets[]{0,-640,0,-1280,0};
        for (std::size_t i=0;i<crossing_animals_.size();++i) {
            const Vertex position{signed_word(trigger.position.x+x_offsets[i]),trigger.position.y,
                                  signed_word(trigger.position.z+z_offsets[i])};
            crossing_animals_[i].target_z=signed_word(trigger.position.z+4480);
            poses_.push_back({trigger.command_offset,0xb2d8,position,128,0,0,crossing_animation_seed,true});
        }
    }
    const auto start=fish_.size()+rotating_.size();
    for (std::size_t i=0;i<crossing_animals_.size();++i) {
        auto& actor=crossing_animals_[i];
        auto& pose=poses_[start+i];
        if (actor.stage==CrossingStage::finished) continue;
        // A movement calls 08:CEC3. With no GSU body, 08:D070 sets timer FF.
        // 03:D3B4 spends the next phase on the empty init callback and restores
        // timer 3. Descriptor transitions which do not move skip that pause.
        if (actor.active_timer==0xff) {actor.active_timer=3;continue;}
        pose.visible=activate(cells,pose.position,actor.active_timer);
        if (!pose.visible) continue;
        if (actor.stage==CrossingStage::approach) {
            pose.frame=(pose.frame+1)%6;
            if (pose.position.z==actor.target_z) {
                actor.stage=CrossingStage::change_model;
                continue;
            }
            pose.position.z=signed_word(pose.position.z+640);
        } else if (actor.stage==CrossingStage::change_model || actor.stage==CrossingStage::jump) {
            if (actor.stage==CrossingStage::change_model) {
                pose.visual_descriptor=0xb2f4;
                pose.frame=crossing_animation_seed;
                actor.stage=CrossingStage::jump;
            }
            if (++pose.frame<7) {
                pose.position.z=signed_word(pose.position.z+64);
            } else {
                // D460 falls through to D498 in the same update.
                pose.visual_descriptor=0xb2d8;
                pose.frame=crossing_animation_seed+1;
                actor.stage=CrossingStage::depart;
                actor.target_z=signed_word(pose.position.z+1280);
                pose.position.z=signed_word(pose.position.z+320);
            }
        } else {
            pose.frame=(pose.frame+1)%6;
            if (pose.position.z==actor.target_z) {
                actor.stage=CrossingStage::finished;
                pose.visible=false;
                continue;
            }
            pose.position.z=signed_word(pose.position.z+320);
        }
        actor.active_timer=0xff;
    }
}

void CourseScenery::step_rotating(std::span<const ActivationCell> cells) {
    // Original scene parents precede appended invisible pivots. A newly created
    // pivot is visited later in this same phase; parents use its previous pose.
    for (std::size_t i=0;i<rotating_.size();++i) {
        auto& actor=rotating_[i]; auto& pose=poses_[fish_.size()+i];
        const bool active=activate(cells,pose.position,actor.active_timer);
        // C9F0 can still be submitted by the original draw list before its
        // active cell wakes; it retains its last pose until that happens.
        const bool yaw_orbit=actor.callback==0x09c9b0;
        pose.visible=active || yaw_orbit;
        if (!active) continue;
        if (!actor.initialized) { actor.initialized=true; actor.pivot_timer=3; }
        const bool orbit=actor.callback==0x09cab1 || yaw_orbit;
        int x=actor.callback==0x09cab1 ? 40 : 100;
        int y=yaw_orbit ? 0 : (orbit ? 10 : 70);
        int z=0;
        auto pair=rotate_pair(x,y,sine_[actor.roll],cosine_[actor.roll]); x=pair[0]; y=pair[1];
        if (actor.callback!=0x09cea8) {
            pair=rotate_pair(y,z,sine_[actor.pitch],cosine_[actor.pitch]); y=pair[0]; z=pair[1];
            const auto yaw=static_cast<std::uint8_t>(-actor.yaw);
            pair=rotate_pair(x,z,sine_[yaw],cosine_[yaw]); x=pair[0]; z=pair[1];
        }
        pose.position={signed_word(actor.initial.position.x+x*32),
                       signed_word(actor.initial.position.y+y*32),
                       signed_word(actor.initial.position.z+z*32)};
        if (orbit) {
            pose.yaw=actor.yaw;
            if(!yaw_orbit)pose.frame=(pose.frame+1)%6;
        } else {
            const bool visible=actor.roll>=0x40 && actor.roll<=0xb9;
            pose.visual_descriptor=visible ? actor.initial.visual_descriptor : 0xa040;
            pose.visible=visible;
            pose.roll=static_cast<std::uint8_t>(actor.roll+0xaa);
            if (visible && !actor.visible_latch && actor.callback==0x09cfe3) effects_.push_back(0x38);
            actor.visible_latch=visible;
        }
    }
    for (auto& actor:rotating_) {
        if (!actor.initialized || !activate(cells,actor.initial.position,actor.pivot_timer)) continue;
        if (actor.callback==0x09cab1) {
            actor.yaw=static_cast<std::uint8_t>(actor.yaw-8);
            actor.pitch=static_cast<std::uint8_t>(actor.pitch+12);
        } else if(actor.callback==0x09c9b0)++actor.yaw;
        else actor.roll=static_cast<std::uint8_t>(actor.roll+(actor.callback==0x09cea8 ? -8 : 8));
    }
}
}
