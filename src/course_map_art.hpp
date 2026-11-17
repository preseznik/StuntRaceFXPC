#pragma once

#include "srf/backdrop.hpp"
#include "srf/bonus_hud_tiles.hpp"
#include "srf/championship.hpp"
#include "srf/course_map.hpp"
#include <bitset>

namespace srf {

// CPU07:D9D1 arranges the noise tiles once. GSU07:EE88 fills their pixels
// every VBlank using its own persistent register and a different feedback word.
class CourseMapNoise {
    std::array<std::uint16_t,32*32> offsets_{};
    std::vector<std::uint8_t> ink_;
    std::size_t cycle_start_{};

    static std::uint16_t next(unsigned seed,unsigned feedback) {
        unsigned mixed=((seed&255)<<8)^seed;
        mixed=((mixed&255)<<8)|(mixed>>8);
        const unsigned v=((mixed>>8)<<1)^mixed;
        const unsigned n=(v>>1)^feedback^((v&1)?0x8180:0);
        return static_cast<std::uint16_t>(n==0xaa55 && !(v&1)?0:n);
    }

public:
    CourseMapNoise(std::uint16_t layout_seed,std::uint16_t pixel_seed) {
        for(auto& offset:offsets_) {
            layout_seed=next(layout_seed,0xff80);
            const unsigned tile=layout_seed&31;
            offset=static_cast<std::uint16_t>((tile&15)*128+(tile>>4)*8);
        }
        // A finite 16-bit generator allows random access even if presentations
        // are repeated or skipped. Not every initial seed is inside the cycle.
        std::bitset<65536> seen;
        auto seed=pixel_seed;
        while(!seen.test(seed)) {
            seen.set(seed);
            seed=next(seed,0xff00);
            ink_.push_back(static_cast<std::uint8_t>(0x80+16*(seed&7)));
        }
        for(auto s=pixel_seed;s!=seed;s=next(s,0xff00))++cycle_start_;
    }

    void tile_pixels(std::span<std::uint8_t,2048> pixels,unsigned tick) const {
        auto start=std::uint64_t(tick)*2048;
        if(start>=ink_.size())start=cycle_start_+(start-cycle_start_)%(ink_.size()-cycle_start_);
        for(auto& pixel:pixels) {
            pixel=ink_[start];
            if(++start==ink_.size())start=cycle_start_;
        }
    }
    unsigned at(std::span<const std::uint8_t,2048> tiles,unsigned x,unsigned y) const {
        return tiles[offsets_[(y>>3)*32+(x>>3)]+(y&7)*16+(x&7)];
    }
};

class CourseMapArt {
    friend class StuntBoardArt; // Both boards use the same SNES tile and color formats.
    static constexpr int width=256, height=224;
    using Palette=std::array<std::uint32_t,256>;
    const SpeedClass league_;
    std::vector<ChampionshipRecord> records_;
    std::array<std::uint16_t,256> colors_{};
    std::vector<std::uint8_t> sprites_,board_,covered_board_;
    std::array<std::uint8_t,32*32> locked_{};
    int card_x_{},card_top_{};
    CourseMapNoise noise_;

    static unsigned tile_pixel(const std::vector<std::uint8_t>& tiles,unsigned tile,unsigned x,unsigned y) {
        const unsigned at=32*(tile&1023)+2*((tile&0x8000)?7-y:y);
        const unsigned bit=(tile&0x4000)?x:7-x;
        const unsigned ink=((tiles[at]>>bit)&1)|(((tiles[at+1]>>bit)&1)<<1)|
                           (((tiles[at+16]>>bit)&1)<<2)|(((tiles[at+17]>>bit)&1)<<3);
        return ink?((tile>>10)&7)*16+ink:0;
    }

    static unsigned glyph(char c) {
        if(c>='a' && c<='z')c=static_cast<char>(c-'a'+'A');
        if(c>='A' && c<='Z')return 10+c-'A';
        if(c>='0' && c<='9')return c-'0';
        if(c=='\'')return 38;
        return 36; // The cartridge dash, also used in empty score slots.
    }

    // SNES object sizes here are 8 or 16; large objects advance 16 tiles per row.
    void obj(std::span<std::uint32_t> out,const Palette& palette,int x,int y,
             unsigned tile,unsigned bank,int size=8,bool flip_x=false,bool flip_y=false) const {
        const auto ink=std::span<const std::uint32_t,16>(palette.data()+128+16*bank,16);
        for(int j=0;j<size;j+=8)for(int i=0;i<size;i+=8) {
            const int dx=flip_x?size-8-i:i, dy=flip_y?size-8-j:j;
            draw_bonus_hud_tile(out,width,x+dx,y+dy,sprites_,tile+i/8+j*2,ink,flip_x,flip_y);
        }
    }

    void text(std::span<std::uint32_t> out,const Palette& palette,int x,int y,
              std::string_view words,unsigned bank) const {
        for(const auto c:words) {
            if(c!=' ')obj(out,palette,x,y,glyph(c),bank);
            x+=8;
        }
    }

    void panel_title(std::span<std::uint32_t> out,const Palette& p,unsigned bank) const {
        for(unsigned i=0;i<9;++i) {
            const auto tile=(i==0 || i==8)?0xb0:0xb1;
            obj(out,p,66+16*i,106,tile,bank,16,i==8,true);
            obj(out,p,66+16*i,62,tile,bank,16,i==8);
        }
        for(int y:{90,78}) {
            obj(out,p,66,y,0xd0,bank,16);obj(out,p,194,y,0xd0,bank,16,true);
        }
        obj(out,p,79,82,36,0);obj(out,p,189,82,36,0);
        text(out,p,90,82,class_name(league_),1);
        text(out,p,146,82,"CLASS",1);
    }

    void record_panel(std::span<std::uint32_t> out,const Palette& p,const CourseMapFrame& frame) const {
        const unsigned page=frame.record_page, phase=frame.record_car_phase;
        const auto title=page?courses[4*static_cast<unsigned>(league_)+page-1].name:std::string_view("BEST TOTAL");
        text(out,p,137-4*static_cast<int>(title.size()),94,title,3);
        constexpr std::array<unsigned,5> border{5,0,3,2,1};
        panel_title(out,p,border[page]);
        for(unsigned rank=0;rank<3;++rank) {
            const int y=122+17*rank;
            const unsigned bank=2-rank;
            std::array<unsigned,8> time{36,36,38,36,36,39,36,36};
            std::string_view name="---";
            if(rank<records_.size()) {
                const auto& record=records_[rank];
                const unsigned t=page?record.course_times[page-1]:record.total();
                time={t/60000%10,t/6000%10,38,t/1000%6,t/100%10,39,t/10%10,t%10};
                name=record.initials;
                const bool truck=record.car==PlayerCar::four_wd, formula=record.car==PlayerCar::f_type;
                const unsigned wheel=phase&1;
                const int lift=phase==2?2:static_cast<int>(wheel);
                obj(out,p,formula?80:79,y-8+lift,formula?48:truck?52:50,5,16);
                obj(out,p,88,y+3,(formula||truck)?72+wheel:57-wheel,5,8,true);
                obj(out,p,truck?78:77,y+3,truck?73-wheel:56+wheel,5);
            }
            for(unsigned i=0;i<time.size();++i)obj(out,p,101+8*i,y,time[i],bank);
            for(unsigned i=0;i<3 && i<name.size();++i)
                obj(out,p,171+8*i,y,glyph(name[i]),bank);
            if(page)obj(out,p,65,y,0xf0,bank,8,true);
            else obj(out,p,65,y,rank+1,bank);
            if(page<4)obj(out,p,201,y,0xf0,bank);
        }
        if(frame.prompt_visible)text(out,p,97,175,"PUSH START",3);
        if(frame.play_cancel_visible) {
            text(out,p,125,175,"PLAY",frame.cancel_selected?4:3);
            text(out,p,125,190,"CANCEL",frame.cancel_selected?3:4);
            obj(out,p,105,frame.cancel_selected?188:173,58,0,16);
        }
    }

    void class_outline(std::span<std::uint32_t> out,const Palette& p,int top,unsigned phase) const {
        // The 36 border objects cycle three palettes around the selected row.
        const auto bank=[phase](unsigned edge){return 2+(phase+edge)%3;};
        for(unsigned col=0;col<15;++col) {
            const bool corner=col==0 || col==14;
            const unsigned tile=corner?0xb3:0xb4;
            obj(out,p,8+16*col,top,tile,bank(15-col),16,col==14);
            obj(out,p,8+16*col,top+63,tile,bank(col+1),16,col!=0,true);
        }
        for(unsigned row=1;row<=3;++row) {
            obj(out,p,8,top-1+16*row,0xc3,bank(row),16);
            obj(out,p,232,top-1+16*row,0xc3,bank(4-row),16,true,true);
        }
    }

    void skip_offer(std::span<std::uint32_t> out,const Palette& p) const {
        text(out,p,109,94,"BONUS",3);obj(out,p,157,94,1+static_cast<unsigned>(league_),3);
        panel_title(out,p,5);
        // CPU's two-row grant labels use the tall OBJ alphabet.
        for(unsigned row=0;row<2;++row) {
            const int y=121+24*row;
            const auto tall=[&](int x,unsigned tile){
                obj(out,p,x,y,tile,2-row);obj(out,p,x,y+8,tile+16,2-row);
            };
            constexpr std::array<unsigned,5> extra{94,145,125,123,90};
            constexpr std::array<unsigned,4> time{125,114,118,94};
            constexpr std::array<unsigned,3> car{92,90,123};
            for(unsigned i=0;i<extra.size();++i)tall(115+8*i,extra[i]);
            if(row==0){for(unsigned i=0;i<time.size();++i)tall(159+8*i,time[i]);}
            else {for(unsigned i=0;i<car.size();++i)tall(159+8*i,car[i]);}
            obj(out,p,103,y+4,220,3);obj(out,p,83,y,row?190:188,6,16);
        }
        text(out,p,129,175,"PLAY",4);text(out,p,129,190,"SKIP",3);
        obj(out,p,109,188,58,0,16);
    }

    static std::uint32_t color(unsigned rgb,unsigned brightness,unsigned shade) {
        const auto component=[=](int shift) {
            const int v=static_cast<int>((rgb>>shift)&31)-static_cast<int>(shade);
            return ((v<0?0:v)*brightness+7)/15;
        };
        return hud_rgba(static_cast<std::uint16_t>(component(0)|(component(5)<<5)|(component(10)<<10)));
    }

public:
    // Native sessions use a reproducible seed for each independent title-screen
    // register. A comparison can supply the observed cartridge seeds directly.
    CourseMapArt(const Rom& rom,const Championship& session,const ChampionshipProgress& progress,
                 std::uint16_t layout_seed=0,std::uint16_t pixel_seed=0):
        league_(session.league()),sprites_(unpack_rom_graphics(rom,0x18c770)),
        board_(width*304),noise_(layout_seed,pixel_seed) {
        const auto records=progress.records(league_);records_.assign(records.begin(),records.end());
        for(unsigned i=2;i<colors_.size();++i)colors_[i]=rom.u16(Rom::lorom_offset(0x1f8200)+2*i);
        auto tiles=unpack_rom_graphics(rom,0x1c989c);
        const auto cards=unpack_rom_graphics(rom,0x18998c), layout=unpack_rom_graphics(rom,0x18de88);
        if(tiles.size()!=0x3000 || cards.size()!=0x2a00 || layout.size()!=0x980 || sprites_.size()!=0x2000)
            throw std::runtime_error("Invalid Speed Trax course map artwork");
        tiles.insert(tiles.end(),cards.begin(),cards.end());
        // CPU02:FB4E supplies the four rows of the KEEP OUT card. FB6E
        // points to the upper-left tile of each card in championship order.
        for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x) {
            const auto tile=rom.u16(Rom::lorom_offset(0x02fb4e)+2*((y/8)*4+x/8));
            locked_[y*32+x]=static_cast<std::uint8_t>(tile_pixel(tiles,tile,x%8,y%8));
        }
        for(unsigned row=0;row<38;++row)for(unsigned col=0;col<32;++col) {
            const auto idx=2*(row*32+col);
            const unsigned tile=layout[idx]|(unsigned(layout[idx+1])<<8);
            for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
                board_[(row*8+y)*width+col*8+x]=static_cast<std::uint8_t>(tile_pixel(tiles,tile,x,y));
        }
        covered_board_=board_;
        for(unsigned card=0;card<12;++card) {
            const unsigned index=rom.u16(Rom::lorom_offset(0x02fb6e)+2*card)/2;
            if(card/4==static_cast<unsigned>(league_) && card%4<session.round()) {
                if(card%4+1==session.round()) {
                    card_x_=static_cast<int>((index%32)*8);card_top_=static_cast<int>((index/32)*8);
                }
                continue;
            }
            const unsigned offset=(index/32)*8*width+(index%32)*8;
            for(unsigned y=0;y<32;++y)std::copy_n(locked_.begin()+32*y,32,covered_board_.begin()+offset+width*y);
        }
    }

    void draw(std::span<std::uint32_t> out,const CourseMapFrame& f) const {
        if(out.size()!=width*height)throw std::invalid_argument("Course map requires 256x224 pixels");
        std::fill(out.begin(),out.end(),0xff000000U);
        if(!f.brightness)return;
        Palette palette,dark;
        for(unsigned i=0;i<256;++i) {
            palette[i]=color(colors_[i],f.brightness,0);
            dark[i]=color(colors_[i],f.brightness,f.panel_shade);
        }
        std::array<std::uint8_t,2048> noise;
        noise_.tile_pixels(noise,f.noise_tick);
        const auto& board=f.base_cards?board_:covered_board_;
        for(int y=0;y<216;++y) {
            const int row=y+1+(y<55?0:static_cast<int>(f.board_scroll));
            const int inset=std::max({0,3-(y-f.panel_top),3-(f.panel_bottom-y)});
            for(int x=0;x<width;++x) {
                unsigned ink=board[row*width+x];
                if(!f.base_cards && x>=card_x_ && x<card_x_+32 && row>=card_top_ && row<card_top_+32) {
                    const int v=row-card_top_+8*static_cast<int>(f.card_step);
                    if(v<32)ink=locked_[v*32+x-card_x_];
                }
                if(!ink)ink=noise_.at(noise,x,y+1);
                const bool shade=y>=f.panel_top && y<=f.panel_bottom &&
                    x>=f.panel_left+inset && x<=f.panel_right-inset;
                out[y*width+x]=shade?dark[ink]:palette[ink];
            }
        }
        if(f.records_visible)record_panel(out,palette,f);
        if(f.class_outline_y>=0)class_outline(out,palette,f.class_outline_y,f.outline_phase);
        if(f.skip_offer_visible)skip_offer(out,palette);
    }
};
}
