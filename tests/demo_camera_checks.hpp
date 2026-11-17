#pragma once
#include "srf/demo_camera.hpp"
#include "srf/demo_drive.hpp"

inline void check_demo_camera(const srf::Rom& rom) {
    srf::DemoDrive drive(rom);
    srf::DemoCamera camera(rom);
    std::uint32_t hash=2166136261u;
    for(unsigned phase=1;phase<=462;++phase) {
        while(drive.phase()<phase)drive.advance();
        const auto& car=drive.car_at(phase);
        const auto pose=camera.view(phase,car);
        // At body phase 7 the packet's yaw is C020. Original wheel callback
        // 08:BFA8 uses 40, while a full-word negation would give 3F.
        if(phase==7 && demo_sprite_facing(pose)!=0x40)throw std::runtime_error("Demo wheel facing");
        if(pose!=camera.view(phase,car))throw std::runtime_error("Repeated demo field advanced camera");
        for(int value:{pose.eye[0],pose.eye[1],pose.eye[2],pose.pitch,pose.yaw})
            hash=(hash^static_cast<std::uint16_t>(value))*16777619u;
    }
    // Independent original CPU02:D7F1 uploads, all 462 rendered Easy Ride
    // scene packets after initialization, including airborne tracking and exit.
    if(hash!=0x812e7cc2u)throw std::runtime_error("Original attract camera poses");
}
