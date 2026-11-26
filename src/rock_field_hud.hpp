#pragma once
#include "demo_dashboard.hpp"
#include "grid_countdown_art.hpp"
#include "srf/stunt_scores.hpp"
#include "srf/rock_field_rules.hpp"
#include "srf/stunt_results.hpp"

namespace srf {

class StuntHud {
    BonusDashboard dashboard_;
    BonusHudBoost boost_;
    CourseHudMap map_;
    BonusTallyArt lettering_;
    std::vector<std::uint32_t> overlay_=std::vector<std::uint32_t>(208*128);
    std::vector<std::uint8_t> signal_;
    std::array<std::uint32_t,16> palette_;
    // First visible field of each lamp, then the SPC trailer stop and driving song.
    // Each row follows the same car selection order as the vehicle table.
    struct Cues {std::array<unsigned,4> lights; unsigned stop,start;};
    static constexpr Cues rock_[3]{
        {{295,347,401,456},301,508},{{305,360,414,468},301,520},{{294,347,399,451},301,503}};
    static constexpr Cues up_down_[3][3]{
        {{{298,350,408,460},308,514},{{298,354,405,459},303,514},{{296,349,406,458},303,512}},
        {{{298,350,403,456},306,508},{{298,354,406,457},306,508},{{297,350,403,460},305,512}},
        {{{298,350,407,457},306,511},{{298,354,402,457},306,511},{{297,350,407,457},306,511}}};
    static constexpr Cues ice_[3][3]{
        {{{293,348,404,463},306,505},{{305,357,417,467},305,516},{{297,352,406,472},305,512}},
        {{{294,348,405,471},307,508},{{301,353,408,462},307,515},{{298,353,414,471},308,519}},
        {{{293,348,401,462},306,505},{{296,349,404,455},306,506},{{298,353,411,481},305,522}}};
    Cues cues_;
public:
    StuntHud(const Rom& rom,PlayerCar car,StuntCourse course,StuntAttempt attempt):dashboard_(rom,car,true),boost_(rom,0),
        map_(rom,stunt_rom_index(course)),lettering_(rom),signal_(extract_start_light_tiles(rom)),
        palette_(bonus_hud_palette(rom,0xf8960)),
        cues_(course==StuntCourse::rock_field || course==StuntCourse::blue_lake ? rock_[unsigned(car)] :
              (course==StuntCourse::ice_dance ? ice_ : up_down_)[unsigned(attempt)][unsigned(car)]) {}
    void advance(bool boosting,bool driving) {boost_.advance(boosting,driving);}
    std::span<const std::uint32_t> panel(const VehicleState& body,unsigned stars) {
        return dashboard_.draw(static_cast<std::int16_t>(body.word(0x4a)),body.word(0x82),stars);
    }
    unsigned stage(unsigned tick) const {
        return static_cast<unsigned>(std::upper_bound(cues_.lights.begin(),cues_.lights.end(),tick)-cues_.lights.begin());
    }
    unsigned trailer_song_end() const {
        return cues_.stop;
    }
    bool driving_song_started(unsigned tick) const {return tick>=cues_.start;}
    void draw(Renderer& renderer,const Vehicle& car,const RockFieldRules& rules,unsigned tick,unsigned released,bool show) {
        std::fill(overlay_.begin(),overlay_.end(),0);
        if(show && tick>=32)lettering_.draw_stunt_clock(overlay_,rules.clock().remaining_seconds,rules.display_bank_seconds(),released>75);
        if(show && tick>=158) {
            const int slide=std::clamp(4*(int(tick)-158)-33,-33,5);
            map_.draw_route(overlay_,slide);
            if(tick>=180)map_.draw_marker(overlay_,car,1);
            boost_.draw_at(overlay_,std::min(0,4*(int(tick)-173)),car.body);
        }
        const int slide_in=std::min(-8,4*(int(tick)-171)-8);
        const int slide_out=4*std::max(0,int(released)-69);
        if(slide_in-slide_out>-64)draw_start_signal(overlay_,slide_in-slide_out,stage(tick),signal_,palette_);
        renderer.set_bonus_overlay(overlay_,15,1,40,show?panel(car.body,rules.stars()):std::span<const std::uint32_t>{});
    }
    void ended(Renderer& renderer,const Vehicle& car,const RockFieldRules& rules,bool timeout,unsigned age) {
        std::fill(overlay_.begin(),overlay_.end(),0);
        if(timeout && age>=15)lettering_.draw_stunt_timeout(overlay_);
        const unsigned close=timeout?290:145;
        const unsigned brightness=age<=close?15:15-std::min(15u,(age-close)/2);
        renderer.set_bonus_overlay(overlay_,brightness,1,0,panel(car.body,rules.stars()));
    }
};

// BG2 uses the $18:C770 tile sheet, palette $1F:8600 and a separate OBJ car sheet.
// Draw the finish, records and retry screens at the cartridge's 256x224 resolution.
class RaceResultsArt {
protected:
    std::vector<std::uint8_t> font_,cars_;
    std::array<std::array<std::uint32_t,16>,16> colors_;
    std::vector<std::uint32_t> pixels_;
    void tile(unsigned id,int x,int y,unsigned bank,bool fx=false,bool fy=false) {
        draw_bonus_hud_tile(pixels_,256,x,y,font_,id,colors_[bank],fx,fy);
    }
    void text(std::string_view line,int x,int y,unsigned bank) {
        for(char c:line) {
            if(c!=' ') {
                if(c>='a'&&c<='z')c=char(c-'a'+'A');
                const unsigned id=c>='A'&&c<='Z'?10+c-'A':c>='0'&&c<='9'?c-'0':c=='"'?39:c=='/'?43:c=='\''?38:36;
                tile(id,x,y,bank);
            }
            x+=8;
        }
    }
    void frame(int left,int right) {
        for(int x=left;x<=right;x+=8) {
            const bool edge=x==left || x==right;
            tile(edge?40:41,x,40,5,x==right);
            if(edge)tile(42,x,48,5,x==right);
            tile(edge?40:41,x,56,5,x==right,true);
        }
    }
    static std::string two(unsigned n) {return std::string{char('0'+n/10%10),char('0'+n%10)};}
    explicit RaceResultsArt(const Rom& rom):font_(unpack_rom_graphics(rom,0x18c770)),
        cars_(unpack_rom_graphics(rom,0x18d920)),pixels_(256*224) {
        for(unsigned i=0;i<16;++i)colors_[i]=bonus_hud_palette(rom,0xf8600+i*32);
        DemoDashboard surround(rom);
        const auto base=surround.draw(100,15,0);
        std::copy(base.begin(),base.end(),pixels_.begin());
    }
    void course_header(std::string_view name,std::string_view initials) {
        for(int y=32;y<160;y+=8)for(int x=24;x<232;x+=8) {
            for(int row=y;row<y+8;++row)std::fill_n(pixels_.begin()+row*256+x,8,colors_[0][0]);
            tile(47,x,y,0);
        }
        frame(32,168);frame(184,216);
        text(name,103-4*int(name.size()),47,12);text(initials,192,48,0);
    }
};

class StuntResultsArt : public RaceResultsArt {
    StuntCourse course_;
public:
    StuntResultsArt(const Rom& rom,StuntCourse course):RaceResultsArt(rom),course_(course) {}
    std::span<const std::uint32_t> draw(StuntPage page,const StuntRecord& finish,PlayerCar selected,
        std::span<const StuntRecord> records,unsigned choice,unsigned age,std::span<const std::uint32_t> panel) {
        course_header(stunt_course_name(course_),finish.initials);
        if(page==StuntPage::finish) {
            text("STARS --",48,80,2);text(two(std::min(age,finish.stars)),120,80,3);text("/40",136,80,0);
            const unsigned rest=age>finish.stars?age-finish.stars:0;
            text("REST ---",48,96,2);text(two(std::min(rest,finish.seconds)),120,96,3);text("SEC",136,96,0);
        } else if(page==StuntPage::records) {
            unsigned i=0;
            for(const auto& r:records) {
                if(i==3)break;
                const int y=80+int(i)*24;
                text(std::to_string(i+1),40,y,2);text(two(r.stars)+"/40",88,y,2);
                text(two(r.seconds)+" SEC",144,y,3);text(r.initials,194,y,2);
                ++i;
            }
            for(;i<3;++i) {
                const int y=80+int(i)*24;
                text(std::to_string(i+1),40,y,2);text("--/--",88,y,2);text("-- SEC",144,y,3);text("---",194,y,2);
            }
        } else {
            constexpr std::string_view actions[]{"TRY AGAIN","CHANGE COURSE","CHANGE CAR"};
            for(unsigned i=0;i<3;++i)text(actions[i],48,80+16*i,i==choice?(age%48<32?2:0):4);
        }
        if(page!=StuntPage::records) {
            constexpr unsigned sheet[]{1,2,0};
            const unsigned base=96*sheet[unsigned(selected)];
            for(unsigned y=0;y<6;++y)for(unsigned x=0;x<8;++x)
                draw_bonus_hud_tile(pixels_,256,154+8*x,106+8*y,cars_,base+y*16+x,colors_[10]);
        }
        std::copy(panel.begin(),panel.end(),pixels_.begin()+168*256);
        return pixels_;
    }
};
}
