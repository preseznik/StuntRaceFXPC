#pragma once
#include "../src/grid_countdown_art.hpp"

inline void check_grid_countdown_art(const srf::Rom& rom) {
    // Use original RGB555 values. For translucent lettering compare its distinctive
    // colored ink across the whole text area, ignoring the moving track underneath.
    const auto palette=srf::bonus_hud_palette(rom,0xf8960);
    const auto hash=[&](std::span<const std::uint32_t> art,int stride,int x,int y,int w,int h,
                        std::initializer_list<unsigned> inks={}) {
        std::uint32_t value=2166136261u;
        for(int j=y;j<y+h;++j)for(int i=x;i<x+w;++i) {
            const auto rgba=art[j*stride+i];
            const bool keep=inks.size()==0 || std::any_of(inks.begin(),inks.end(),
                [&](unsigned ink){return rgba==palette[ink];});
            const auto rgb=keep?((rgba>>3)&31)|(((rgba>>11)&31)<<5)|(((rgba>>19)&31)<<10):0u;
            value=(value^rgb)*16777619u;
        }
        return value;
    };
    const srf::NativeRace race(rom,srf::PlayerCar::coupe,srf::CourseId::white_land,90,srf::GridOrder{1,4,3,2});
    srf::GridCountdownArt art(rom,srf::CourseId::white_land,srf::PlayerCar::coupe);
    std::vector<std::uint32_t> pixels(208*128);
    srf::GridCountdownFrame f;
    f.clock_visible=true;f.initial_carry=f.remaining_carry=90;f.clock_seconds=40;
    // Expert 40+90 grid. Rightward arrow, transfer to three digits, and
    // contraction with the right edge held, through the first clock-only frame.
    constexpr std::uint32_t transfer[]{
        0x70824c9f,0xd1bde604,0x87d4204d,0xfa083002,0x51e1029d,0x5a6447df,
        0x19a8bf73,0xd043f01c,0x1c967f48,0x81c1d910,0x3a7a73ff,0xe7fa8cd3};
    for(int i=0;i<12;++i) {
        f.closing=i>=7 && i<11;
        f.banner_step=i<5?i:i<7?4:10-i;
        f.clock_seconds=i<5?40:i==5?76:130;f.remaining_carry=130-f.clock_seconds;
        art.draw(pixels,f,race);
        if(hash(pixels,208,0,0,208,40,{5,6,7,11,12,13})!=transfer[i])
            throw std::runtime_error("Original grid transfer arrow or counters differ");
    }
    f.initial_carry=0;f.banner_step=-1;f.clock_seconds=40;
    constexpr unsigned phase[]{25,26,27,28,29,30,39,40,41,42,43,44};
    constexpr std::uint32_t laps[]{
        0x45fc3f25,0xd65e3885,0x3f7df365,0xf10042c5,0x780c6805,0x8ae93a05,
        0xbfa30505,0x383735e5,0x1c868685,0x6cf0a265,0x9e8e0e85,0x7b33e5c5};
    for(unsigned i=0;i<std::size(phase);++i) {
        f.display_phase=phase[i];art.draw(pixels,f,race);
        if(hash(pixels,208,0,40,208,32,{9,10})!=laps[i])
            throw std::runtime_error("Original zero-bank lap lettering differs");
    }
    f.signal_visible=true;f.signal_x=-8;
    constexpr std::uint32_t lamps[]{0x0224c635,0xacb85926,0xddeaff91,0x91f1188a,0x4cb15344};
    for(unsigned i=0;i<std::size(lamps);++i) {
        f.light_stage=i;art.draw(pixels,f,race);
        if(hash(pixels,208,0,6,48,16)!=lamps[i])throw std::runtime_error("Original grid signal differs");
    }
    f.signal_x=-36;art.draw(pixels,f,race);
    if(pixels[13*208]!=srf::hud_rgba(0x0286) || pixels[14*208+1]!=srf::hud_rgba(0x0286))
        throw std::runtime_error("Original retracting lamp edge differs");
    constexpr srf::PlayerCar cars[]{srf::PlayerCar::four_wd,srf::PlayerCar::coupe,srf::PlayerCar::f_type};
    constexpr unsigned spares[]{3,4,3};
    constexpr std::uint32_t dashboards[]{0x2aed4cf7,0xeb2b5f7e,0x9297687a};
    constexpr unsigned marker_x[]{17,22,17},marker_y[]{109,99,95};
    constexpr std::uint16_t marker_color[]{0x7ee0,0x037f,0x593f};
    for(unsigned i=0;i<3;++i) {
        const srf::NativeRace grid(rom,cars[i],static_cast<srf::CourseId>(4*i+2),90,srf::GridOrder{1,4,3,2});
        srf::GridCountdownArt selected(rom,static_cast<srf::CourseId>(4*i+2),cars[i]);
        f.spares=spares[i];
        if(hash(selected.dashboard(f,grid),256,0,0,256,40)!=dashboards[i])
            throw std::runtime_error("Original grid dashboard, car icon or spare count differs");
        f.boost_x=0;f.marker_visible=true;f.map_x=5;selected.draw(pixels,f,grid);
        if(pixels[116*208+5]!=srf::hud_rgba(i==1?0x6318:0x5ad5))
            throw std::runtime_error("Original course boost gauge palette differs");
        if(pixels[marker_y[i]*208+marker_x[i]]!=srf::hud_rgba(marker_color[i]))
            throw std::runtime_error("Original selected-car map marker differs");
    }
    // Raster from qualified fourth races. These hashes include all four
    // overlapping actor markers, the entire course outline and its shadow.
    constexpr std::uint32_t routes[]{0x8c4383c8,0x8716fba5,0xba94464d};
    constexpr srf::GridOrder orders[]{{1,2,3,4},{1,3,2,4},{1,2,3,4}};
    for(unsigned i=0;i<3;++i) {
        const auto course=static_cast<srf::CourseId>(4*i+3);
        const srf::NativeRace grid(rom,cars[i],course,90,orders[i]);
        const srf::GridCountdownArt selected(rom,course,cars[i]);
        srf::GridCountdownFrame visible;
        visible.map_x=5;visible.marker_visible=true;
        visible.signal_x=-8;visible.signal_visible=true;visible.light_stage=4;
        selected.draw(pixels,visible,grid);
        if(hash(pixels,208,0,80,46,32)!=routes[i])
            throw std::runtime_error("Fourth course grid route or colored markers differ");
        if(hash(pixels,208,0,6,48,16)!=(i==2?0xee31648a:0x4cb15344))
            throw std::runtime_error("Fourth course green lamp palette differs");
    }
    // Last locked raster of each first grid. Settle against the original
    // prestart phase clock, as the game does before releasing the cars.
    constexpr std::uint32_t first_routes[]{
        0x35f45b59,0xbb880b75,0xa633f4d9,0x2fc0c623,0xd2a8f14f,0xae78f023,0xb097dc13,0xb7811d4f,0x6563ce93};
    constexpr std::uint32_t first_dash[]{0xe67a5c62,0x9722cbf1,0xaed5c06c};
    for(unsigned i=0;i<9;++i) {
        const auto course=static_cast<srf::CourseId>(4*(i/3));
        const auto car=static_cast<srf::PlayerCar>(i%3);
        srf::NativeRace grid(rom,car,course,0,srf::GridOrder{4,3,2,1});
        srf::GridCountdown start(i/3,i%3,grid.remaining_seconds(),0,2,{4,3,2,1},srf::GridCountdownRound::first);
        while(start.frame().tick+1<start.drive_tick())start.advance();
        while(grid.phase()+1<start.frame().scene_phase)grid.step(0,0);
        srf::GridCountdownArt selected(rom,course,car);
        selected.draw(pixels,start.frame(),grid);
        if(hash(pixels,208,0,80,46,32)!=first_routes[i] ||
           hash(selected.dashboard(start.frame(),grid),256,0,0,256,40)!=first_dash[i%3] ||
           pixels[116*208+5]!=srf::hud_rgba(i/3==1?0x6b59:0x5ad5))
            throw std::runtime_error("Original first-race map, dashboard or boost gauge differs");
    }
}
