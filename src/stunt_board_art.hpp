#pragma once
#include "course_map_art.hpp"
#include "srf/stunt_board.hpp"
#include "srf/stunt_record.hpp"

namespace srf {
class StuntBoardArt {
    using Palette=std::array<std::uint32_t,256>;
    std::array<std::uint16_t,256> colors_{};
    std::vector<std::uint8_t> board_,sprites_;
    std::array<std::vector<std::uint8_t>,playable_stunt_courses.size()> data_boxes_;
    std::array<std::vector<StuntRecord>,playable_stunt_courses.size()> scores_;
    CourseMapNoise noise_{0,0};

    void obj(std::span<std::uint32_t> out,const Palette& p,int x,int y,unsigned tile,
             unsigned bank,int size=8,bool flip_x=false,bool flip_y=false) const {
        const auto palette=std::span<const std::uint32_t,16>(p.data()+128+16*bank,16);
        for(int j=0;j<size;j+=8)for(int i=0;i<size;i+=8)
            draw_bonus_hud_tile(out,256,x+(flip_x?size-8-i:i),y+(flip_y?size-8-j:j),
                                sprites_,tile+i/8+j*2,palette,flip_x,flip_y);
    }
    void text(std::span<std::uint32_t> out,const Palette& p,int x,int y,std::string_view word,unsigned bank) const {
        for(const char c:word){
            if(c!=' ')obj(out,p,x,y,CourseMapArt::glyph(c),bank);
            x+=8;
        }
    }
    void outline(std::span<std::uint32_t> out,const Palette& p,const StuntBoard& f) const {
        const int x=24+112*int(f.course&1),y=(f.course&2)?57:137;
        const unsigned phase=2-(f.tick-118)/4%3;
        const auto bank=[&](int step){return 2+(phase+step)%3;};
        for(int i=0;i<6;++i){
            const bool corner=i==0 || i==5;
            obj(out,p,x+16*i,y,corner?179:180,bank(6-i),16,i==5);
            obj(out,p,x+16*i,y+63,corner?179:180,bank(i+1),16,i!=0,true);
        }
        for(int j=1;j<=3;++j){
            obj(out,p,x,y-1+j*16,195,bank(j),16);
            obj(out,p,x+80,y-1+j*16,195,bank(4-j),16,true,true);
        }
    }
    void records(std::span<std::uint32_t> out,const Palette& p,const StuntBoard& f,
                 std::span<const StuntRecord> scores) const {
        constexpr std::array<unsigned,4> borders{1,0,3,2};
        const unsigned border=borders[f.course];
        for(int j=0;j<10;++j){
            const unsigned tile=(j==0 || j==9)?176:177;
            obj(out,p,49+16*j,62,tile,border,16,j==9);
            obj(out,p,49+16*j,94,tile,border,16,j==9,true);
        }
        obj(out,p,49,78,208,border,16);obj(out,p,193,78,208,border,16,true);
        const auto name=StuntBoard::names[f.course];
        text(out,p,129-4*int(name.size()),82,name,3);
        for(unsigned rank=0;rank<3;++rank){
            const int y=111+17*int(rank);const unsigned bank=2-rank;
            obj(out,p,49,y,rank+1,bank);
            const auto* score=rank<scores.size()?&scores[rank]:nullptr;
            obj(out,p,91,y,score?score->stars/10%10:36,bank);
            obj(out,p,99,y,score?score->stars%10:36,bank);
            obj(out,p,107,y,43,7);obj(out,p,115,y,4,7);obj(out,p,123,y,0,7);
            obj(out,p,143,y,score?score->seconds/10%10:36,bank);
            obj(out,p,151,y,score?score->seconds%10:36,bank);
            for(unsigned j=0;j<3;++j)obj(out,p,157+8*j,y,44+j,7);
            text(out,p,185,y,score?std::string_view(score->initials):"---",bank);
            if(score){
                const bool truck=score->car==PlayerCar::four_wd, formula=score->car==PlayerCar::f_type;
                const unsigned phase=f.tick/8%4,wheel=phase&1;
                obj(out,p,formula?63:62,y-8+(phase==2?2:wheel),formula?48:truck?52:50,5,16);
                obj(out,p,71,y+3,(formula||truck)?72+wheel:57-wheel,5,8,true);
                obj(out,p,truck?61:60,y+3,(formula||truck)?73-wheel:56+wheel,5);
            }
        }
        text(out,p,109,164,"PLAY",f.choice==0?3:4);
        text(out,p,109,177,"DATA",f.choice==1?3:4);text(out,p,146,177,"BOX",f.choice==1?3:4);
        text(out,p,109,190,"CANCEL",f.choice==2?3:4);
        obj(out,p,89,162+13*int(f.choice),58,0,16);
    }
public:
    explicit StuntBoardArt(const Rom& rom,const std::array<std::vector<StuntRecord>,playable_stunt_courses.size()>& scores={}):
        board_(512*256),sprites_(unpack_rom_graphics(rom,0x18c770)),scores_(scores){
        auto tiles=unpack_rom_graphics(rom,0x1c989c);
        const auto cards=unpack_rom_graphics(rom,0x18aad0),layout=unpack_rom_graphics(rom,0x18e408);
        if(tiles.size()!=0x3000 || cards.size()!=0x2a00 || layout.size()!=0x1000 || sprites_.size()!=0x2000)
            throw std::runtime_error("Invalid Stunt Trax course board artwork");
        tiles.insert(tiles.end(),cards.begin(),cards.end());
        for(unsigned i=2;i<256;++i)colors_[i]=rom.u16(Rom::lorom_offset(0x1f8200)+2*i);
        for(unsigned y=0;y<512;++y)for(unsigned x=0;x<256;++x){
            const auto at=2*(32*(y/8)+x/8);
            const unsigned tile=layout[at]|(unsigned(layout[at+1])<<8);
            if((tile&1023)>=tiles.size()/32)throw std::runtime_error("Invalid Stunt board tile");
            board_[y*256+x]=static_cast<std::uint8_t>(CourseMapArt::tile_pixel(tiles,tile,x%8,y%8));
        }
        for(unsigned i=0;i<scores_.size();++i)prepare_records(i,tiles);
    }
private:
    void prepare_records(unsigned course,const std::vector<std::uint8_t>& tiles) {
        auto& records=scores_[course];
        if(records.empty())return;
        std::stable_sort(records.begin(),records.end(),[](const auto& a,const auto& b){
            return a.stars==b.stars?a.seconds>b.seconds:a.stars>b.stars;
        });
        // The PLAY panel merges the cars. The DATA BOX writes separate top-three cells
        // into the tilemap for each car, including the driver's initials.
        auto& data_box=data_boxes_[course];
        data_box.assign(board_.begin()+256*360,board_.end());
        std::array<unsigned,3> counts{};
        for(const auto& entry:records) {
            const auto idx=static_cast<unsigned>(entry.car);
            if(idx>=counts.size() || counts[idx]>=3 || entry.initials.size()!=3)continue;
            const int rank=counts[idx]++, row=(entry.car==PlayerCar::four_wd?60:51)+rank;
            const int col=entry.car==PlayerCar::coupe?15:0;
            const unsigned bank=std::array{0x3800,0x3000,0x3400}[rank];
            const auto cell=[&](int cx,unsigned glyph){
                for(int y=0;y<8;++y)for(int x=0;x<8;++x)
                    data_box[(8*row+y-360)*256+8*(col+cx)+x]=static_cast<std::uint8_t>(
                        CourseMapArt::tile_pixel(tiles,bank|glyph,x,y));
            };
            cell(5,entry.stars/10%10);cell(6,entry.stars%10);
            cell(9,entry.seconds/10%10);cell(10,entry.seconds%10);
            for(int i=0;i<3;++i)cell(12+i,CourseMapArt::glyph(entry.initials[i]));
        }
    }
public:
    void draw(std::span<std::uint32_t> out,const StuntBoard& f) const {
        if(out.size()!=256*224)throw std::invalid_argument("Stunt board requires 256x224 pixels");
        std::fill(out.begin(),out.end(),0xff000000u);
        const unsigned light=f.brightness();
        if(!light)return;
        Palette p,dark;
        for(unsigned i=0;i<256;++i){
            p[i]=CourseMapArt::color(colors_[i],light,0);
            dark[i]=CourseMapArt::color(colors_[i],light,25);
        }
        std::array<std::uint8_t,2048> noise;noise_.tile_pixels(noise,f.tick);
        const bool panel=f.panel_visible();
        const bool has_data=f.page==StuntBoard::Page::data && f.course<data_boxes_.size() && !data_boxes_[f.course].empty();
        for(int y=0;y<216;++y){
            const int scroll=y<55?0:f.page==StuntBoard::Page::data?304:80;
            const int inset=std::max({0,72-y,y-201});
            for(int x=0;x<256;++x){
                const int row=(y+scroll+1)%512;
                unsigned ink=has_data && row>=360?data_boxes_[f.course][256*(row-360)+x]:board_[256*row+x];
                if(!ink)ink=noise_.at(noise,x,y+1);
                const bool shade=panel && y>=69 && y<=204 && x>=40+inset && x<=216-inset;
                out[256*y+x]=shade?dark[ink]:p[ink];
            }
        }
        if(f.outline_visible())outline(out,p,f);
        if(panel)records(out,p,f,f.course<scores_.size()?std::span<const StuntRecord>(scores_[f.course]):std::span<const StuntRecord>());
    }
};
}
