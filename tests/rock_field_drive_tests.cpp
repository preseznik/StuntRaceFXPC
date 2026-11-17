#include "srf/rock_field.hpp"
#include "srf/rock_field_rules.hpp"
#include <iostream>

namespace {
struct OriginalPose { unsigned phase; int y,z,pitch,speed; };
struct OriginalLaunch {
    srf::PlayerCar car;
    unsigned first_control_field;
    std::array<OriginalPose,5> samples;
    std::array<std::int16_t,5> final_eye;
};
// USA Rev 1 body samples with the controller released. Fields count from the
// first visible trailer image. Each selected car has a different launch.
constexpr OriginalLaunch launches[]{
    {srf::PlayerCar::coupe,453,{{{52,-226,-25408,-632,0},{76,-123,-24734,3088,149},
        {84,-53,-23119,-20,196},{92,-52,-21323,-316,241},{100,-52,-20433,-256,35}}},
        {-22208,-367,-21328,-3548,0}},
    {srf::PlayerCar::four_wd,465,{{{52,-299,-25408,-316,0},{76,-121,-24097,712,239},
        {84,-147,-22201,396,229},{92,-160,-20399,-144,236},{98,-160,-19409,32,96}}},
        {-22208,-481,-20395,-3484,0}},
    {srf::PlayerCar::f_type,448,{{{52,-243,-25408,-772,0},{76,-178,-24876,2764,131},
        {84,-65,-23337,-276,238},{92,-63,-21466,-444,238},{97,-63,-20632,-444,69}}},
        {-22208,-374,-21518,-3406,0}}
};
constexpr OriginalLaunch up_down_launches[]{
    {srf::PlayerCar::coupe,457,{{{52,-226,-19328,-632,0},{76,-123,-18654,3088,149},
        {84,-53,-17039,-20,196},{92,-52,-15243,-316,241},{102,-52,-14341,-256,0}}},
        {-22208,-363,-15182,-3599,0}},
    {srf::PlayerCar::four_wd,456,{{{52,-299,-19328,-316,0},{76,-121,-18017,712,239},
        {84,-147,-16121,396,229},{92,-160,-14319,-144,236},{97,-160,-13424,32,117}}},
        {-22208,-481,-14441,-3450,0}},
    {srf::PlayerCar::f_type,455,{{{52,-243,-19328,-772,0},{76,-178,-18796,2764,131},
        {84,-65,-17257,-276,238},{92,-63,-15386,-444,238},{100,-63,-14411,-444,32}}},
        {-22208,-372,-15308,-3465,0}}
};

// Measured from each original PLAY or TRY AGAIN selection.
constexpr OriginalLaunch ice_launches[][3]{
    {
        {srf::PlayerCar::coupe,451,{{{52,-226,-23168,-632,0},{76,-132,-22524,2860,148},{84,-54,-20932,12,194},{92,-52,-19142,-304,243},{107,-52,-18224,-264,0}}},
            {-21248,-352,-19039,-3647,0}},
        {srf::PlayerCar::four_wd,461,{{{52,-299,-23168,-316,0},{76,-121,-21857,712,239},{84,-147,-19961,396,229},{92,-160,-18159,-144,236},{104,-160,-16876,32,26}}},
            {-21248,-476,-17742,-3663,0}},
        {srf::PlayerCar::f_type,457,{{{52,-243,-23168,-772,0},{76,-178,-22636,2764,131},{84,-65,-21097,-276,238},{92,-63,-19226,-444,238},{107,-63,-18242,-444,0}}},
            {-21248,-363,-19057,-3628,0}},
    },
    {
        {srf::PlayerCar::coupe,453,{{{52,-226,-23168,-632,0},{76,-132,-22524,2860,148},{84,-54,-20932,12,194},{92,-52,-19142,-304,243},{107,-52,-18224,-264,0}}},
            {-21248,-352,-19039,-3647,0}},
        {srf::PlayerCar::four_wd,460,{{{52,-299,-23168,-316,0},{76,-121,-21857,712,239},{84,-147,-19961,396,229},{92,-160,-18159,-144,236},{103,-160,-16903,32,33}}},
            {-21248,-477,-17783,-3641,0}},
        {srf::PlayerCar::f_type,462,{{{52,-243,-23168,-772,0},{76,-178,-22636,2764,131},{84,-65,-21097,-276,238},{92,-63,-19226,-444,238},{108,-63,-18242,-444,0}}},
            {-21248,-362,-19055,-3637,0}},
    },
    {
        {srf::PlayerCar::coupe,450,{{{52,-226,-23168,-632,0},{76,-132,-22524,2860,148},{84,-54,-20932,12,194},{92,-52,-19142,-304,243},{106,-52,-18224,-264,0}}},
            {-21248,-353,-19042,-3641,0}},
        {srf::PlayerCar::four_wd,453,{{{52,-299,-23168,-316,0},{76,-121,-21857,712,239},{84,-147,-19961,396,229},{92,-160,-18159,-144,236},{101,-160,-16978,32,51}}},
            {-21248,-479,-17891,-3584,0}},
        {srf::PlayerCar::f_type,466,{{{52,-243,-23168,-772,0},{76,-178,-22636,2764,131},{84,-65,-21097,-276,238},{92,-63,-19226,-444,238},{109,-63,-18242,-444,0}}},
            {-21248,-361,-19053,-3644,0}},
    },
};

void check_launch(const srf::Rom& rom,srf::StuntCourse course,const OriginalLaunch& original,
                  srf::StuntAttempt attempt=srf::StuntAttempt::first,std::uint32_t expected_camera=0) {
    srf::RockFieldDrive drive(rom,original.car,course,attempt);
    std::size_t index=0;
    std::uint32_t camera=2166136261u;
    const auto sample_eye=[&] {
        for(auto value:drive.trailer_camera())camera=(camera^std::uint16_t(value))*16777619u;
    };
    sample_eye();
    while(!drive.drive_started()) {
        if(drive.video_tick()>original.first_control_field)throw std::runtime_error("Trailer failed to release");
        const bool advanced=drive.advance_intro();
        sample_eye();
        if(!advanced || index==original.samples.size())continue;
        const auto& sample=original.samples[index];
        if(drive.phase()!=sample.phase)continue;
        const auto& body=drive.player().body;
        if(std::int16_t(body.word(0x20))!=sample.y || std::int16_t(body.word(0x22))!=sample.z ||
           std::int16_t(body.word(0x26))!=sample.pitch || body.word(0x4a)!=sample.speed)
            throw std::runtime_error("Trailer body differs at vehicle phase "+std::to_string(sample.phase));
        ++index;
    }
    if(index!=original.samples.size() || drive.video_tick()!=original.first_control_field ||
       drive.trailer_camera()!=original.final_eye || drive.poses()[0].visible || drive.poses()[1].visible)
        throw std::runtime_error("Trailer camera, timing or visibility differs at control handoff");
    if(expected_camera && camera!=expected_camera)throw std::runtime_error("Trailer camera differs during rollout");
}


void check_ice_grip(const srf::Rom& rom) {
    struct GripSample {
        unsigned phase,buttons;
        int vx,vz;
        unsigned yaw,speed;
    };
    // Stock USA Rev 1 video-field replay: accelerate, steer right, coast,
    // countersteer, brake and coast. The GSU latches the pad at each update.
    constexpr GripSample coupe[]{
        {126,0x00,0,0,0,0},
        {153,0x10,0,1034,0,114},
        {163,0x12,297,1397,63492,177},
        {174,0x00,304,1097,62426,142},
        {184,0x11,-70,920,90,117},
        {194,0x40,-79,-1074,65180,65395},
        {245,0x00,-6,166,65018,20},
    };
    constexpr GripSample four_wd[]{
        {118,0x00,0,0,0,0},
        {143,0x10,0,1786,0,236},
        {153,0x12,548,1488,61690,205},
        {164,0x00,403,682,60358,99},
        {172,0x11,580,1506,61648,204},
        {173,0x51,522,1410,61996,190},
        {182,0x40,181,616,62146,82},
        {232,0x00,24,57,62308,7},
    };
    constexpr GripSample f_type[]{
        {124,0x00,0,0,0,0},
        {151,0x10,0,1442,0,172},
        {162,0x12,379,1746,63772,226},
        {173,0x00,256,750,61698,101},
        {182,0x11,165,599,62184,78},
        {183,0x51,132,493,62410,64},
        {192,0x40,-311,-863,62554,65415},
        {239,0x00,-60,-118,61100,65519},
    };

    const auto compare=[&](srf::PlayerCar car,std::span<const GripSample> samples) {
        srf::RockFieldDrive drive(rom,car,srf::StuntCourse::ice_dance);
        srf::RockFieldRules rules(rom,drive.scene(),car);
        while(!drive.drive_started())drive.advance_intro();
        for(const auto& sample:samples) {
            while(drive.phase()<sample.phase) {
                drive.set_event_contact(rules.body_contact(drive.player().body));
                drive.step(static_cast<std::uint16_t>(sample.buttons));
                rules.step(drive.player().body,0);
            }
            const auto& b=drive.player().body;
            if(b.word(0x8a)!=0xc8 || std::int16_t(b.word(0x2e))!=sample.vx ||
               std::int16_t(b.word(0x32))!=sample.vz || b.word(0x24)!=sample.yaw || b.word(0x4a)!=sample.speed)
                throw std::runtime_error("Ice grip differs for car "+std::to_string(unsigned(car))+
                                         " at phase "+std::to_string(sample.phase));
        }
    };
    compare(srf::PlayerCar::coupe,coupe);
    compare(srf::PlayerCar::four_wd,four_wd);
    compare(srf::PlayerCar::f_type,f_type);
}

void check_up_down_retries(const srf::Rom& rom) {
    struct Retry {unsigned release,clock; OriginalPose final; std::array<std::int16_t,5> eye;};
    // Each row starts at the results screen's TRY AGAIN selection, with the
    // controller released as the selected car rolls out of the trailer.
    constexpr Retry retries[][3]{
        {{453,453,{101,-52,-14328,-256,26},{-22208,-365,-15204,-3577,0}},
         {454,455,{96,-160,-13540,32,144},{-22208,-479,-14591,-3415,0}},
         {454,454,{99,-63,-14441,-444,46},{-22208,-373,-15363,-3435,0}}},
        {{454,455,{101,-52,-14328,-256,26},{-22208,-365,-15204,-3577,0}},
         {454,455,{96,-160,-13540,32,144},{-22208,-479,-14591,-3415,0}},
         {458,458,{100,-63,-14411,-444,32},{-22208,-372,-15308,-3465,0}}}
    };
    for(unsigned outcome=0;outcome<2;++outcome)for(unsigned car=0;car<3;++car) {
        const auto attempt=outcome==0?srf::StuntAttempt::after_finish:srf::StuntAttempt::after_timeout;
        srf::RockFieldDrive drive(rom,srf::PlayerCar(car),srf::StuntCourse::up_n_down,attempt);
        const auto& ref=retries[outcome][car];
        for(unsigned f=0;f<ref.release;++f) {
            if(drive.drive_started())throw std::runtime_error("Retry trailer released early");
            drive.advance_intro();
        }
        const auto& body=drive.player().body;
        if(!drive.drive_started() || drive.phase()!=ref.final.phase || drive.first_clock_field()!=ref.clock ||
           drive.trailer_camera()!=ref.eye || std::int16_t(body.word(0x20))!=ref.final.y ||
           std::int16_t(body.word(0x22))!=ref.final.z || std::int16_t(body.word(0x26))!=ref.final.pitch ||
           body.word(0x4a)!=ref.final.speed)
            throw std::runtime_error("Retry trailer release differs from original");
    }
}
}

int wmain(int argc,wchar_t** argv) {
    try {
        const auto* path=argc>1?argv[1]:_wgetenv(L"SRF_ROM");
        if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        for(const auto& launch:launches)check_launch(rom,srf::StuntCourse::rock_field,launch);
        for(const auto& launch:up_down_launches)check_launch(rom,srf::StuntCourse::up_n_down,launch);
        check_up_down_retries(rom);
        check_ice_grip(rom);
        // Five original camera words at every video field from the first visible frame.
        constexpr std::uint32_t camera_traces[][3]{
            {0x5c95da4e,0x3b083288,0x53736f10}, {0x240170ce,0x09348f0f,0x091b8cbd},
            {0x7f9f001e,0xf190668c,0x98b9e59d}
        };
        for(unsigned attempt=0;attempt<3;++attempt)for(const auto& launch:ice_launches[attempt])
            check_launch(rom,srf::StuntCourse::ice_dance,launch,srf::StuntAttempt(attempt),
                         camera_traces[attempt][static_cast<unsigned>(launch.car)]);
        std::cout<<"Original trailer launches, retry cameras and all three Ice Dance grip replays passed\n";
        return 0;
    } catch(const std::exception& error) {
        std::cerr<<error.what()<<'\n';
        return 1;
    }
}
