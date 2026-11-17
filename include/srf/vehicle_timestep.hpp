#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace srf {
// Development conversion helpers. The playable scheduler does not use these.
// Ratio is elapsed time / original callback time, NOT / whole phase time:
// the player receives two callbacks per phase, starting opponents only one.
// One instance belongs to one vehicle and must reset with that vehicle.
class VehicleTimestep {
public:
    explicit VehicleTimestep(unsigned numerator=7,unsigned denominator=30)
        : numerator_(numerator),denominator_(denominator) {
        if(!numerator || numerator>denominator || denominator>1000000)
            throw std::invalid_argument("Vehicle timestep requires 0 < numerator <= denominator <= 1000000");
    }
    void reset() {
        pose_remainder_={};force_remainder_={};contact_remainder_={};phase_=0;
        attachment_history_={};translation_history_={};
        ordered_pose_remainder_={};previous_continuous_force_={};
        horizontal_rest_={};
        yaw_rest_=false;yaw_rest_initialized_=false;
        height_rest_=false;tilt_rest_={};suspension_rest_initialized_=false;
        settled_armed_=settled_holding_=false;settled_load_={};
    }
    // At7/30: steps0,5,9,13,18,22,26,30... start original callback intervals.
    // Event impulses run once at this boundary, not on every physical substep.
    bool begin_step() {
        const bool due=phase_<numerator_;
        phase_=(phase_+numerator_)%denominator_;
        return due;
    }
    // Query after begin_step(): this substep completes the callback interval.
    // At7/30: steps4,8,12,17,21,25,29...; unity starts and ends every step.
    bool ends_callback() const { return phase_<numerator_; }
    std::uint16_t callback_end_position(std::uint16_t before,std::uint16_t after) const {
        if(!ends_callback())throw std::logic_error("Substep does not cross a callback boundary");
        // A120Hz step can straddle the28Hz boundary. Sample the segment at that
        // boundary, using the short wrapped displacement, without advancing it.
        const auto delta=signed_word(static_cast<std::uint16_t>(after-before));
        return static_cast<std::uint16_t>(before+delta*(numerator_-phase_)/numerator_);
    }
    std::uint16_t pose_delta(std::uint16_t original_delta,unsigned axis) {
        return scaled(signed_word(original_delta),pose_remainder_.at(axis));
    }
    void stop_horizontal_axis(unsigned axis) {
        if(axis!=0 && axis!=2)throw std::invalid_argument("Rest constraint requires a horizontal axis");
        horizontal_rest_.at(axis)=true;
        pose_remainder_.at(axis)=0;
    }
    std::uint16_t resting_pose_delta(std::uint16_t original_delta,std::uint16_t velocity,
                                     unsigned axis,bool allow_rest) {
        if(axis>2)throw std::invalid_argument("Rest constraint requires a translation axis");
        // A callback stop is a rest constraint, not a recurring force eraser.
        // Keep integrating forces so sustained demand can cross the original
        // asymmetric threshold. Suppress only displacement of an already
        // stopped horizontal axis until it wakes. Suspension remains live.
        auto& resting=horizontal_rest_.at(axis);
        const auto speed=signed_word(velocity);
        if(!allow_rest || speed < -200 || speed>=200)resting=false;
        if(resting) return 0;
        return pose_delta(original_delta,axis);
    }
    std::uint16_t ordered_pose_delta(std::uint16_t original_delta,std::uint16_t velocity,
                                     unsigned shift,unsigned axis) {
        if(shift>4)throw std::invalid_argument("Unsupported original pose divisor");
        // Explicit Euler advances pose before applying force. Smaller steps
        // otherwise advance new momentum by (H-h)/2 relative to that order.
        // Use the preceding continuous force only; event impulses stay immediate.
        const auto force=velocity ? previous_continuous_force_.at(axis) : 0;
        const auto divisor=std::int64_t(32)*denominator_*denominator_;
        const auto dividend=signed_word(original_delta)*32*denominator_*numerator_
            -force*(16>>shift)*numerator_*(denominator_-numerator_)
            +ordered_pose_remainder_.at(axis);
        const auto delta=dividend/divisor;
        ordered_pose_remainder_[axis]=dividend-delta*divisor;
        return static_cast<std::uint16_t>(delta);
    }
    void prepare_yaw_rest(std::uint16_t velocity,bool allow_rest) {
        if(!yaw_rest_initialized_)yaw_rest_=velocity==0;
        if(!allow_rest || velocity)yaw_rest_=false;
        yaw_rest_initialized_=true;
    }
    void stop_yaw() { yaw_rest_=true; }
    std::uint16_t resting_yaw_pose_delta(std::uint16_t original_delta) {
        return yaw_rest_ ? 0 : pose_delta(original_delta,3);
    }
    std::uint16_t resting_yaw_force_delta(std::uint16_t raw_total,std::uint16_t impulse,
        std::uint16_t velocity,bool allow_rest) {
        const auto force=signed_word(raw_total);
        // Neutral-input yaw rest uses the original unscaled torque demand for
        // breakaway. Waiting for small velocity increments to reach64 delays
        // steering; letting them rotate before each stop changes coast drag.
        // Active input or existing momentum releases immediately. Pitch/roll
        // remain live. A held yaw axis cancels torque that the original callback
        // would add to zero velocity and then discard in its stopping snap.
        if(!allow_rest || velocity || force < -64 || force>=64)yaw_rest_=false;
        if(yaw_rest_)return force_delta(0,0,3,true);
        return force_delta(raw_total,impulse,3);
    }
    void prepare_suspension_rest(std::uint16_t vertical_velocity,
        const std::array<std::uint16_t,2>& tilt_velocity,bool allow_rest) {
        if(!suspension_rest_initialized_) {
            height_rest_=vertical_velocity==0;
            for(unsigned i=0;i<2;++i)tilt_rest_[i]=tilt_velocity[i]==0;
        }
        const auto speed=signed_word(vertical_velocity);
        if(!allow_rest || speed < -50 || speed>=50)height_rest_=false;
        for(unsigned i=0;i<2;++i)
            if(!allow_rest || tilt_velocity[i])tilt_rest_[i]=false;
        suspension_rest_initialized_=true;
    }
    void stop_suspension_axis(unsigned axis) {
        if(axis==1)height_rest_=true;
        else if(axis==4 || axis==5)tilt_rest_[axis-4]=true;
        else throw std::invalid_argument("Suspension rest requires height, pitch or roll");
    }
    std::uint16_t suspension_pose_delta(std::uint16_t original_delta,unsigned axis) {
        bool resting;
        if(axis==1)resting=height_rest_;
        else if(axis==4 || axis==5)resting=tilt_rest_[axis-4];
        else throw std::invalid_argument("Suspension rest requires height, pitch or roll");
        // Height holds after a real stop while support forces keep integrating.
        // A changed load can cross the original vertical velocity threshold.
        // Do not erase either height's force carry or its position fraction.
        return resting ? 0 : pose_delta(original_delta,axis);
    }
    std::uint16_t suspension_torque_delta(std::uint16_t raw_total,std::uint16_t impulse,
        std::uint16_t velocity,unsigned axis,bool allow_rest) {
        if(axis!=4 && axis!=5)throw std::invalid_argument("Suspension torque requires pitch or roll");
        auto& resting=tilt_rest_[axis-4];
        const auto torque=signed_word(raw_total);
        // Like neutral yaw, tilt breaks away on the full callback torque demand.
        // Repeated subthreshold kicks otherwise tilt the car between the
        // original stopping checks and change the next acceleration's contacts.
        if(!allow_rest || velocity || torque < -64 || torque>=64)resting=false;
        if(resting)return force_delta(0,0,axis,true);
        return force_delta(raw_total,impulse,axis);
    }
    std::uint16_t attachment_velocity(std::uint16_t wrapped_position_delta,
                                      unsigned wheel,unsigned axis) {
        // Undo the shorter sampling interval BEFORE the original *8 result
        // narrows to16 bits. Inverting an already-wrapped velocity loses data.
        // The signed position difference still uses the original world wrap.
        auto& remainder=contact_remainder_.at(wheel).at(axis);
        const auto dividend=signed_word(wrapped_position_delta)*8*denominator_+remainder;
        const auto velocity=dividend/numerator_;
        remainder=dividend-velocity*numerator_;
        return static_cast<std::uint16_t>(velocity);
    }
    std::uint16_t attachment_window_velocity(std::uint16_t wrapped_position_delta,
                                              std::uint16_t initial_velocity,
                                              unsigned wheel,unsigned axis) {
        return window_velocity(attachment_history_.at(wheel).at(axis),wrapped_position_delta,initial_velocity);
    }
    std::uint16_t braking_attachment_velocity(std::uint16_t measured_wheel_velocity,
        std::uint16_t body_displacement,std::uint16_t original_body_delta,unsigned wheel,unsigned axis) {
        // The contact window contains delayed translation and rotation. Braking
        // must respond to current translation, not that delay or the numerical
        // pose-order correction. Retain rotational contact history unchanged.
        const auto current=static_cast<std::uint16_t>(signed_word(original_body_delta)*8);
        const auto average=window_velocity(translation_history_.at(wheel).at(axis),body_displacement,current);
        return static_cast<std::uint16_t>(measured_wheel_velocity+current-average);
    }
    void reset_attachment(unsigned wheel) {
        attachment_history_.at(wheel)={};translation_history_.at(wheel)={};
    }
    bool reset_settled_contacts(bool eligible,const std::array<std::uint16_t,6>& velocity) {
        if(!eligible || any_word(velocity))settled_armed_=settled_holding_=false;
        // After all axes stop, the old contact window still contains motion.
        // Seed the next static force evaluation with zero attachment velocity.
        return settled_armed_;
    }
    bool prepare_settled_rest(bool eligible,const std::array<std::uint16_t,6>& velocity,
        const std::array<std::uint16_t,6>& force,const std::array<std::uint16_t,6>& impulse) {
        if(!eligible || any_word(velocity) || any_word(impulse)) {
            settled_armed_=settled_holding_=false;
        } else if(settled_armed_) {
            // A transient zero velocity is not necessarily equilibrium. A
            // static load outside the original stopping deadbands must move
            // the body; otherwise the constraint can pin it above the road.
            for(unsigned axis=0;axis<6;++axis) {
                const auto load=signed_word(force[axis]);
                const int limit=axis==1 ? 50 : axis>=3 ? 64 : 200;
                if(load < -limit || load>=limit) {
                    settled_armed_=false;
                    return false;
                }
            }
            settled_load_=force;settled_armed_=false;settled_holding_=true;
        } else if(settled_holding_ && force!=settled_load_) {
            // A changed support/load must wake on this tick, even without input.
            settled_holding_=false;
        }
        return settled_holding_;
    }
    void finish_settled_rest(bool eligible,const std::array<std::uint16_t,6>& velocity,
        const std::array<std::uint16_t,6>& impulse) {
        // The original all-axis stop arms a constraint for the next step.
        // Unity keeps the original callback unchanged; no force is erased
        // before its original stop decision. Active driving never arms it.
        if(numerator_!=denominator_ && ends_callback() && eligible
           && !any_word(velocity) && !any_word(impulse) && !settled_holding_)
            settled_armed_=true;
    }
    std::uint16_t force_delta(std::uint16_t raw_total,std::uint16_t impulse,unsigned axis,
                              bool discarded=false) {
        auto& remainder=force_remainder_.at(axis);
        if(discarded || settled_holding_) {remainder=0;previous_continuous_force_.at(axis)=0;return 0;}
        const auto limit=[axis](std::uint16_t word) {
            const auto value=signed_word(word);
            return axis==1 || axis>=3 ? std::clamp<std::int64_t>(value,-3000,3000) : value;
        };
        // Capture provenance BEFORE the original Y/torque limiter. Allocate
        // its nonlinear residue to the event: continuous C=limit(raw-I),
        // impulse J=limit(raw_total)-C. At unity C+J is the exact original result;
        // a clipped jump cannot turn gravity into a fictitious negative force.
        const auto continuous=limit(static_cast<std::uint16_t>(raw_total-impulse));
        previous_continuous_force_.at(axis)=continuous;
        const auto event=limit(raw_total)-continuous;
        return static_cast<std::uint16_t>(scaled(continuous,remainder)+event);
    }
private:
    static bool any_word(const std::array<std::uint16_t,6>& values) {
        return std::any_of(values.begin(),values.end(),[](auto value){return value!=0;});
    }
    struct AttachmentHistory {
        std::vector<std::int64_t> samples;
        std::size_t next{};
        std::int64_t sum{},remainder{};
    };
    std::uint16_t window_velocity(AttachmentHistory& history,
        std::uint16_t wrapped_position_delta,std::uint16_t initial_velocity) {
        // Match the original finite-difference horizon, not the quantized
        // displacement of just one short substep. Weight the oldest sample
        // fractionally when a callback contains a noninteger number of steps.
        if(history.samples.empty()) {
            const auto seed=signed_word(initial_velocity)*numerator_;
            history.samples.assign((denominator_+numerator_-1)/numerator_,seed);
            history.sum=seed*std::int64_t(history.samples.size());
        }
        const auto sample=signed_word(wrapped_position_delta)*8*denominator_;
        history.sum+=sample-history.samples[history.next];
        history.samples[history.next]=sample;
        history.next=(history.next+1)%history.samples.size();
        auto dividend=history.sum*numerator_;
        const auto tail=denominator_%numerator_;
        if(tail)dividend-=history.samples[history.next]*(numerator_-tail);
        dividend+=history.remainder;
        const auto divisor=std::int64_t(denominator_)*numerator_;
        const auto velocity=dividend/divisor;
        history.remainder=dividend-velocity*divisor;
        return static_cast<std::uint16_t>(velocity);
    }
    static std::int64_t signed_word(std::uint16_t word) {
        return word>=32768 ? std::int64_t(word)-65536 : word;
    }
    std::uint16_t scaled(std::int64_t value,std::int64_t& remainder) {
        const auto dividend=value*numerator_+remainder;
        const auto result=dividend/denominator_;
        remainder=dividend-result*denominator_;
        return static_cast<std::uint16_t>(result);
    }
    unsigned numerator_,denominator_,phase_{};
    std::array<std::int64_t,6> pose_remainder_{},force_remainder_{};
    std::array<std::int64_t,6> ordered_pose_remainder_{},previous_continuous_force_{};
    std::array<bool,3> horizontal_rest_{};
    bool yaw_rest_{},yaw_rest_initialized_{};
    bool height_rest_{},suspension_rest_initialized_{};
    bool settled_armed_{},settled_holding_{};
    std::array<std::uint16_t,6> settled_load_{};
    std::array<bool,2> tilt_rest_{};
    std::array<std::array<std::int64_t,3>,4> contact_remainder_{};
    std::array<std::array<AttachmentHistory,3>,4> attachment_history_{},translation_history_{};
};
}
