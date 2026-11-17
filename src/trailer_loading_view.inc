#include "bonus_dashboard.hpp"
#include "bonus_hud_map.hpp"
#include "bonus_hud_boost.hpp"
#include "bonus_tally_art.hpp"

// Original 208 x 128 Bonus Trax playfield. All graphics come from the supplied ROM.
namespace {
class TrailerLoadingView {
    struct Timing {
        std::array<unsigned,4> lights;
        unsigned drive, music, retract, late_lamp;
    };
    static constexpr Timing schedules[3][3]{
        {{{379,430,481,536},537,592,610,0},{{380,432,485,540},540,596,613,0},{{379,430,484,539},539,595,612,0}},
        {{{380,433,489,544},544,598,616,0},{{381,435,489,543},543,597,615,0},{{380,433,488,542},542,596,614,0}},
        {{{381,437,491,546},547,598,615,0},{{378,430,483,538},538,590,607,2},{{376,429,484,540},540,591,608,1}}
    };
    // CPU02:DAD6 consumes camera-object words +08..+10. Knots are relative to
    // the starting truck, sampled by original scene phase, including the pan at
    // the ramp. The original frame remains held between those scene phases.
    struct CameraKey { unsigned phase; float x,y,z,pitch,yaw; };
    static constexpr CameraKey orbit[]{
        {1,1999,-1160,5999,-1616,36096},{3,1968,-1146,5968,-1616,36064},
        {4,1910,-1101,5910,-1616,36000},{6,1725,-1005,5725,-1616,35808},
        {9,1313,-909,5313,-1568,35280},{13,598,-750,4598,-1488,34096},
        {16,-17,-612,3982,-1408,32736},{18,-453,-515,3546,-1296,31472},
        {20,-815,-433,3161,-1168,30144},{23,-1205,-340,2588,-960,28240},
        {26,-1467,-271,2019,-848,26224},{28,-1591,-235,1641,-768,24752},
        {32,-1759,-179,888,-608,21248},{36,-1858,-137,160,-400,17264},
        {38,-1869,-131,-131,-352,15696},{40,-1850,-136,-384,-400,14256},
        {43,-1776,-161,-710,-528,12432},{47,-1622,-218,-1077,-800,10304},
        {52,-1378,-311,-1472,-1248,7824},{54,-1288,-344,-1595,-1408,7056},
        {57,-1165,-386,-1727,-1568,6160},{64,-911,-456,-1891,-1920,4656},
        {72,-648,-516,-1961,-2240,3296},{87,-188,-613,-1985,-2768,960},
        {89,-147,-643,-1953,-2960,768},{93,-90,-734,-1829,-3632,480},
        {95,-45,-896,-1880,-3876,240},{97,-26,-1038,-1932,-4264,112},
        {99,-15,-1131,-1961,-4643,32},{101,-9,-1187,-1977,-4949,32},
        {107,-2,-1247,-1994,-5437,0},{121,0,-1262,-2000,-5616,0}
    };
    const srf::Rom& rom;
    const Timing& timing;
    const unsigned league;
    const unsigned car_index;
    srf::BonusHudMap map;
    srf::BonusHudBoost boost;
    srf::BonusDashboard dashboard;
    srf::BonusTallyArt tally_art;
    const std::vector<std::uint8_t> signal;
    std::vector<std::uint32_t> art;
    std::array<std::uint32_t,16> palette{};
    unsigned scene_phase=1;
    srf::RaceClock displayed_clock{};
    std::int16_t displayed_speed{};

    void warning_digit(unsigned offset,unsigned age) {
        // Measured GSU source-texel increments (8.8 fixed point) in the four
        // warning poses. The enlarged glyph can extend above the playfield.
        struct Pose { int left,top,step; };
        static constexpr Pose poses[]{{181,9,256},{179,5,210},{177,2,180},{176,-1,160}};
        const auto [left,top,step]=poses[std::min(3u,age/1000)];
        static constexpr unsigned inks[]{0,15,7,6};
        for(int y=std::max(0,top);(y-top)*step<32*256;++y) {
            const int row=(y-top)*step/256;
            for(int x=left;(x-left)*step<16*256;++x) {
                const int col=(x-left)*step/256;
                const auto ink=(rom.bytes()[offset+32*row+col/4]>>(2*(col%4)))&3;
                if(ink)art[y*208+x]=palette[inks[ink]];
            }
        }
    }

    void glyph(unsigned offset,int x,int y,const std::array<unsigned,4>& colors,int clip_y) {
        // GSU font atlas, four 2-bit texels per byte, little-endian, 128 texels per row.
        for(int row=0;row<32 && y+row<clip_y;++row) {
            for(int col=0;col<16;++col) {
                const auto ink=(rom.bytes()[offset+32*row+col/4]>>(2*(col%4)))&3;
                if(ink)art[(y+row)*208+x+col]=palette[colors[ink]];
            }
        }
    }
    void object(unsigned id,int x,int y) {
        // The signal uses SNES 16x16 OBJs; each lower pair of tiles is 16 tiles down.
        for(int row=0;row<16;++row)for(int col=0;col<16;++col) {
            const int dest=x+col;
            if(dest<0 || dest>=208)continue;
            const auto tile=(id-0xa0)+col/8+16*(row/8);
            const auto address=32*tile+2*(row%8);
            const int shift=7-col%8;
            unsigned index=0;
            for(unsigned plane=0;plane<4;++plane)
                index|=((signal[address+16*(plane/2)+plane%2]>>shift)&1u)<<plane;
            if(index)art[(y+row)*208+dest]=palette[index];
        }
    }
public:
    float eye_x{},eye_z{};
    TrailerLoadingView(const srf::Rom& source,srf::SpeedClass group,srf::PlayerCar car)
        :rom(source),timing(schedules[static_cast<unsigned>(group)][static_cast<unsigned>(car)]),
         league(static_cast<unsigned>(group)),car_index(static_cast<unsigned>(car)),map(rom,group),boost(rom,league),dashboard(rom,car),
         tally_art(rom),signal(srf::extract_start_light_tiles(rom)),art(208*128),palette(srf::bonus_hud_palette(rom,0xf8960)) {}
    unsigned drive_tick() const {return timing.drive;}
    unsigned music_tick() const {return timing.music;}
    unsigned end_tick() const {return timing.retract+14;}
    void advance(unsigned tick,bool boosting) {boost.advance(boosting,tick>=drive_tick());}
    void latch_instruments(const srf::SpeedBonus& event) {
        // The original displays the completed scene buffer, one object update behind.
        displayed_clock=event.clock();
        latch_speed(event);
    }
    void latch_speed(const srf::SpeedBonus& event) {
        displayed_speed=static_cast<std::int16_t>(event.truck().body.word(0x4a));
    }
    std::uint8_t light_stage(unsigned tick) const {
        return static_cast<std::uint8_t>(std::upper_bound(timing.lights.begin(),timing.lights.end(),tick)-timing.lights.begin());
    }
    void append_audio(unsigned previous,unsigned tick,srf::RaceAudioOutput& output) const {
        srf::append_countdown_audio(2+light_stage(previous),2+light_stage(tick),output);
    }
    std::uint8_t camera(Renderer& renderer,const srf::Vehicle& truck,unsigned tick,unsigned phase) {
        scene_phase=phase;
        const auto at=std::clamp(phase,orbit[0].phase,std::end(orbit)[-1].phase);
        const auto* b=std::lower_bound(std::begin(orbit)+1,std::end(orbit),at,
            [](const auto& key,unsigned p){return key.phase<p;});
        const auto& a=b[-1];
        const float w=float(at-a.phase)/(b->phase-a.phase);
        const auto& body=truck.body;
        const float x=float(signed_word(body.word(0x1e))),y=float(signed_word(body.word(0x20))),z=float(signed_word(body.word(0x22)));
        constexpr float radians=6.28318530718f/65536.f;
        const float heading=signed_word(body.word(0x24))*radians;
        const float dx=std::lerp(a.x,b->x,w),dz=std::lerp(a.z,b->z,w);
        eye_x=x+std::cos(heading)*dx-std::sin(heading)*dz;
        eye_z=z+std::sin(heading)*dx+std::cos(heading)*dz;
        const float blend=std::clamp((int(tick)-int(timing.retract)+1)/14.f,0.f,1.f);
        const auto target=renderer.normalize_world({std::lerp(eye_x,x,blend),
            std::lerp(y+62+std::lerp(a.y,b->y,w),y,blend),std::lerp(eye_z,z,blend)});
        controls.target_x=target[0];controls.target_y=target[1];controls.target_z=target[2];
        controls.yaw=heading-(1-blend)*std::lerp(a.yaw,b->yaw,w)*radians;
        controls.pitch=std::lerp(std::lerp(a.pitch,b->pitch,w)*radians,-.22f,blend);
        controls.zoom=std::lerp(2.f,1.6f,blend);
        controls.distance=renderer.normalized_distance(3400.f)*blend;
        if(blend==1)return static_cast<std::uint8_t>(body.word(0x24)>>8);
        // CPU08:B647 adds the high byte of the original yaw word. The native
        // wheel and far-car paths subtract their view byte instead.
        const auto raw_yaw=static_cast<std::uint16_t>(std::lround(-controls.yaw/radians));
        return static_cast<std::uint8_t>(-(raw_yaw>>8));
    }
    void overlay(Renderer& renderer,unsigned tick,const srf::SpeedBonus& event,unsigned spares,bool show_hud,const srf::BonusTally* tally) {
        std::fill(art.begin(),art.end(),0);
        const auto panel=show_hud?dashboard.draw(displayed_speed,event.truck().body.word(0x82),spares):std::span<const std::uint32_t>{};
        if(tally && !tally->driving_sprites_visible()) {
            tally_art.draw(art,tally->frame());
            const auto& frame=tally->frame();
            renderer.set_bonus_overlay(art,frame.brightness,1.f,0.f,panel,frame.clip_top,frame.clip_bottom);
            return;
        }
        static constexpr unsigned title_end[]{266,265,263};
        if(tick<title_end[league]) {
            static constexpr unsigned letters[]{0xa840c,0xa8c00,0xa881c,0xa8c18,0xa8c10,
                                                0xa8c14,0xa8c0c,0xa8408,0xa9004};
            for(unsigned i=0;i<std::size(letters);++i)
                glyph(letters[i],28+16*i+(i>=5?4:0),40,{0,15,12,13},128);
        }
        const auto digit=[](unsigned n){return 0xa8000+(n/8)*0x400+4*(n%8);};
        const bool warning=event.clock().remaining_seconds<10 && displayed_clock.remaining_seconds<10;
        if(show_hud && scene_phase>=35 && (!tally || tally->driving_digits_visible())) {
            const auto top=std::max(8,32-4*int(scene_phase-35));
            if(warning)warning_digit(digit(displayed_clock.remaining_seconds),9999-displayed_clock.remaining_fraction);
            else {
                const auto number=std::clamp<unsigned>(event.clock().remaining_seconds,10,99);
                const std::array<unsigned,4> colors=number>=20?std::array{0u,15u,5u,11u}:std::array{0u,15u,12u,13u};
                glyph(digit(number/10),172,top,colors,40);
                glyph(digit(number%10),188,top,colors,40);
            }
            // Visible VBlanks of the eight flag-counter positions, relative
            // to signal retraction for each original class/car combination.
            static constexpr unsigned entry[3][3][8]{
                {{15,20,24,28,33,37,42,46},{16,21,25,30,34,38,43,47},{20,25,29,34,38,42,47,51}},
                {{22,26,30,34,38,43,47,51},{18,22,26,31,35,39,43,47},{18,22,26,30,35,39,43,47}},
                {{14,19,23,27,31,35,39,43},{22,26,30,35,39,43,47,51},{23,27,31,35,39,43,47,51}}
            };
            const auto& slide=entry[league][car_index];
            if(tick>=timing.retract+slide[0]) {
                const auto stage=std::upper_bound(std::begin(slide),std::end(slide),tick-timing.retract)-slide;
                const int y=40-4*int(stage);
                const auto flags=event.reward().extra_seconds%100;
                glyph(digit(flags/10),4,y,{0,15,10,9},40);
                glyph(digit(flags%10),20,y,{0,15,10,9},40);
            }
        }
        if(show_hud) {
            map.draw(art,event.truck(),tick);
            boost.draw(art,tick,event.truck().body);
        }
        const int reached=league==2?253:255;
        // The cartridge builds its OAM shadow one VBlank before the displayed DMA.
        const int oam_tick=std::max(0,int(tick)-1);
        const int box=std::min(-8,-8+4*(oam_tick-reached))-
            4*std::max(0,oam_tick-int(timing.retract)+1);
        if(box+64>0) {
            for(unsigned col=0;col<4;++col) {
                const auto tile=col==0?0xa4u:col==3?0xa6u:0xa5u;
                object(tile,box+16*int(col),0);
                object(tile+4,box+16*int(col),16);
            }
            // Lamps are double-buffered with the race display. The sound and
            // input edges precede the visible lamps by four VBlanks. Two
            // Master red lights span a fifth VBlank in the original core.
            const unsigned display_tick=tick>=4?tick-4:0;
            unsigned stage=light_stage(display_tick);
            if(stage && stage==timing.late_lamp && display_tick==timing.lights[stage-1])--stage;
            for(unsigned j=0;j<std::min(3u,stage);++j)object(stage==4?0xa2:0xa0,box+10+14*int(j),8);
        }
        static constexpr unsigned fade[]{1,1,2,3,4,6,8,11,14};
        const unsigned brightness=tick<28?0:tick<37?fade[tick-28]:15;
        // Outside the HUD palette the cartridge reveals the scene from
        // RGB555 (7,10,14) in steps of 3/32. Each layout has its own VBlank cadence.
        static constexpr unsigned palette_ticks[3][11]{
            {42,50,57,64,72,79,86,93,101,108,113},
            {42,49,56,63,70,78,85,92,99,106,112},
            {40,48,55,62,69,77,84,91,98,106,111}
        };
        const auto& changes=palette_ticks[league];
        const auto step=unsigned(std::upper_bound(std::begin(changes),std::end(changes),tick)-std::begin(changes));
        renderer.set_bonus_overlay(art,brightness,std::min(32u,1+3*step)/32.f,warning?52.f:40.f,panel);
    }
};
}
