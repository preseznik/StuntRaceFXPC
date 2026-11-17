#pragma once
#include "srf/grid_countdown.hpp"
#include <stdexcept>

inline void check_grid_countdown() {
    const auto check=[](bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);};
    // First race, every class and selected car. The visible green and the
    // CPU input release occur on different video fields on the cartridge.
    struct FirstRace {unsigned grant,unlock,clock,first_red,green,music,decrement;};
    constexpr FirstRace first[]{
        {40,539,107,377,543,596,606},{50,540,107,386,544,597,606},{40,542,106,380,547,601,606},
        {50,507,90,351,511,565,573},{60,514,90,360,518,568,578},{50,508,90,350,512,565,574},
        {40,527,106,373,531,582,591},{40,543,106,383,547,600,610},{40,535,106,381,539,590,600}
    };
    for(unsigned i=0;i<9;++i){
        const auto& c=first[i];
        srf::GridCountdown grid(i/3,i%3,c.grant,0,2,{4,3,2,1},srf::GridCountdownRound::first);
        check(grid.drive_tick()==c.unlock,"First race control release");
        for(;!grid.finished();grid.advance()){
            const auto& f=grid.frame();
            check(f.driving==(f.tick>=c.unlock) && f.clock_visible==(f.tick>=c.clock),"First race countdown");
            check(f.spares==2 && !f.initial_carry && !f.remaining_carry && f.banner_step<0,"First race bank and spare cars");
            check(f.clock_seconds==c.grant-unsigned(f.tick>=c.decrement),"First race remaining time");
            check((f.light_stage>0)==(f.tick>=c.first_red) && (f.light_stage==4)==(f.tick>=c.green),"First race lamps");
            check(f.start_music==(f.tick==c.music),"First race course music");
        }
    }
    // First green in VBlanks from the first destination-grid raster. Empty
    // banks run the shorter THREE LAPS TO GO routine in place of EXTRA TIME.
    constexpr unsigned green[3][3][2]{
        {{541,526},{558,526},{554,535}},
        {{557,535},{563,532},{566,525}},
        {{587,546},{582,551},{577,546}}
    };
    for(unsigned league=0;league<3;++league)for(unsigned car=0;car<3;++car){
        for(unsigned bank:{0u,1u,3u,15u,16u,31u,32u,47u,48u,63u,64u,65u,97u,99u}){
            const unsigned grant=league==2 || (league==0 && car==1)?50:40;
            const unsigned unit=bank<32?1:bank<48?2:bank<64?3:4;
            srf::GridCountdown grid(league,car,grant,bank,2);
            check(grid.drive_tick()==green[league][car][bank==0],"Grid control release edge");
            unsigned red=0,go=0,transfer=0,flush=0,music=0,last_camera=0;
            while(!grid.finished()){
                const auto& f=grid.frame();
                check(f.spares==2 && f.grant_seconds==grant,"Grid changed the grant or spare count");
                check(f.scene_phase>=last_camera,"Grid camera went backward");
                last_camera=f.scene_phase;
                check(f.driving==(f.tick>=grid.drive_tick()),"Grid changed control outside green");
                if(!f.driving){
                    const auto p=f.display_phase;
                    const auto credit=p>=48?bank:p<31?0:std::min(bank,(p-30)*unit);
                    check(f.remaining_carry==bank-credit && f.clock_seconds==grant+credit,"Grid transfer amounts");
                    check(f.clock_visible==(p>=25),"Grid first clock presentation");
                    check((f.banner_step>=0)==(bank && p>=25 && p<=52),"Grid bonus banner visibility");
                    check(f.closing==bool(bank && p>=49 && p<=52),"Grid bonus banner retraction");
                }
                if(f.effect==0x0b)++transfer;
                if(f.effect==0x27)++flush;
                if(f.effect==0x49)++red;
                if(f.effect==0x4a){++go;check(f.tick==grid.drive_tick(),"Grid green cue");}
                if(f.start_music){++music;check(f.driving && f.tick>grid.drive_tick(),"Grid race song timing");}
                grid.advance();
            }
            check(red==3 && go==1 && music==1,"Grid signal or music cue count");
            check(transfer==unsigned(bank!=0) && flush==unsigned(bank!=0),"Grid transfer cue count");
            const auto& last=grid.frame();
            check(!last.signal_visible && last.marker_visible && last.light_stage==4,"Grid signal retraction");
            check(last.brightness==15 && last.elapsed_x==148 && last.boost_x==0 && last.map_x==5,"Grid HUD endpoint");
            check(last.clock_seconds==grant+bank-1,"Grid first full second after green");
        }
    }
    struct FourthCase {
        unsigned league,car,grant,bank;
        std::array<std::uint16_t,4> grid;
        std::array<unsigned,4> sounds,lights;
        unsigned unlock,music,first_clock_decrement;
    };
    constexpr FourthCase fourth[]{
        {0,1,60,85,{4,3,2,1},{403,458,513,564},{407,462,517,568},564,622,632},
        // The working CPU crossed a video-field boundary between the green cue and unlock.
        {1,0,40,85,{2,3,1,4},{426,482,538,594},{430,487,543,599},595,652,659},
        {2,2,50,0,{2,4,3,1},{387,441,495,549},{391,445,499,553},549,604,616},
    };
    for(const auto& c:fourth){
        srf::GridCountdown grid(c.league,c.car,c.grant,c.bank,2,c.grid,srf::GridCountdownRound::fourth);
        check(grid.drive_tick()==c.unlock,"Fourth course unlock tick");
        while(!grid.finished()){
            const auto& f=grid.frame();
            for(unsigned i=0;i<4;++i)if(f.tick==c.sounds[i])
                check(f.effect==(i==3?0x4a:0x49),"Fourth course lamp sound");
            const auto lights=std::upper_bound(c.lights.begin(),c.lights.end(),f.tick)-c.lights.begin();
            check(f.light_stage==static_cast<unsigned>(lights),"Fourth course visible lamp");
            check(f.driving==(f.tick>=c.unlock) && f.start_music==(f.tick==c.music),"Fourth course release and music");
            if(f.tick==c.unlock)
                check(f.clock_seconds==c.grant+c.bank && !f.remaining_carry && f.spares==2,"Fourth course grid clock");
            if(f.driving)
                check(f.clock_seconds==c.grant+c.bank-unsigned(f.tick>=c.first_clock_decrement),"Fourth course visible clock decrement");
            grid.advance();
        }
    }
}
