#pragma once

#include "srf/boot_menu.hpp"
#include "srf/backdrop.hpp"
#include "srf/bonus_hud_tiles.hpp"
#include "boot_car_art.hpp"

namespace srf {
class BootMenuArt {
    std::vector<std::uint8_t> tiles_,layout_,sprites_,noise_,letters_;
    std::array<std::uint16_t,256> colors_{};
    std::vector<std::uint16_t> checker_;
    BootCarArt cars_;
    using Palette=std::array<std::uint32_t,256>;
    static unsigned ink(const std::vector<std::uint8_t>& data,unsigned tile,unsigned x,unsigned y,unsigned bits) {
        unsigned result=0;
        const unsigned at=tile*8*bits+2*y;
        for(unsigned b=0;b<bits;++b)result|=((data[at+(b/2)*16+b%2]>>(7-x))&1)<<b;
        return result;
    }
    void object(std::span<std::uint32_t> out,const Palette& p,int x,int y,unsigned tile,unsigned bank,
                int size=8,bool flip_x=false,bool flip_y=false,const std::vector<std::uint8_t>* other=nullptr) const {
        for(int cy=0;cy<size;cy+=8)for(int cx=0;cx<size;cx+=8)
            draw_bonus_hud_tile(out,256,x+(flip_x?size-8-cx:cx),y+(flip_y?size-8-cy:cy),
                other?*other:sprites_,tile+cx/8+2*cy,std::span<const std::uint32_t,16>(p.data()+128+bank*16,16),flip_x,flip_y);
    }
    static unsigned glyph(char c) {
        if(c>='0' && c<='9')return c-'0';
        if(c=='-')return 68;
        const unsigned n=10+c-'A';return (n/16)*32+n%16;
    }
    void text(std::span<std::uint32_t> out,const Palette& p,int x,int y,std::string_view str,unsigned bank,int spacing=8) const {
        for(char c:str) {
            if(c!=' '){const auto id=glyph(c);object(out,p,x,y,id,bank);object(out,p,x,y+8,id+16,bank);}
            x+=c==' '?4:spacing;
        }
    }
    void panel(std::span<std::uint32_t> out,const Palette& p,int dx) const {
        const auto obj=[&](int x,int y,unsigned tile){object(out,p,x+dx,y,tile,5,16);};
        for(int x=24;x<=104;x+=16){obj(x,71,97);obj(x,87,106);obj(x,175,103);obj(x,191,100);}
        obj(8,71,96);obj(24,87,105);obj(8,191,99);obj(24,175,102);
        for(int y:{87,103,119,135,151,167,175}){obj(8,y,108);if(y!=87&&y!=175)obj(24,y,110);}
    }
    void choices(std::span<std::uint32_t> out,const Palette& p,const BootMenuFrame& f,int dx) const {
        static constexpr std::array<std::string_view,4> names{"SPEED TRAX","STUNT TRAX","BATTLE TRAX","TEST RUN"};
        constexpr std::string_view characters="SPEDTRAXUNBL";
        for(unsigned row=0;row<4;++row){
            const auto name=names[row];int x=128-int(name.size())*8+dx;int letter=0;
            for(char c:name){
                if(c!=' ') {
                    unsigned pose=5;
                    bool flip_y=false;
                    // Each letter has eleven successive edge-on/front/back tiles.
                    if(row==f.mode) {
                        constexpr int turn[]{6,7,8,9,10,9,8,7,6,5,4,3,2,1,0,1,2,3,4,5};
                        const int step=int(f.lettering/4)-letter;
                        pose=step>=0&&step<20?turn[step]:5;
                        flip_y=step>=5 && step<15;
                    }
                    const unsigned tile=unsigned(characters.find(c))*64+(pose/8)*32+(pose%8)*2;
                    object(out,p,x,98+22*row,tile,row==f.mode?5:1,16,false,flip_y,&letters_);++letter;
                }
                x+=16;
            }
        }
    }
    void statistics(std::span<std::uint32_t> out,const Palette& p,const BootMenuFrame& f,int dx) const {
        if(f.car==PlayerCar::four_wd) {
            for(unsigned i=0;i<6;++i){object(out,p,dx+183+8*i,98,480+i,5);object(out,p,dx+183+8*i,106,496+i,5);}
            for(unsigned i=0;i<4;++i){const unsigned t=480+std::array{6,7,0,1}[i];object(out,p,dx+191+8*i,138,t,5);object(out,p,dx+191+8*i,146,t+16,5);}
            text(out,p,dx+183,178,"100",5);
        } else if(f.car==PlayerCar::coupe) {
            text(out,p,dx+183,98,"MEDIUM",5);text(out,p,dx+183,138,"MEDIUM",5);text(out,p,dx+183,178,"120",5);
        } else {
            text(out,p,dx+191,98,"WEAK",5);text(out,p,dx+191,138,"SLOW",5);
            for(unsigned i=0;i<3;++i){object(out,p,dx+183+8*i,178,488+i,5);object(out,p,dx+183+8*i,186,504+i,5);}
        }
    }
    void vehicle_labels(std::span<std::uint32_t> out,const Palette& p,const BootMenuFrame& f) const {
        text(out,p,54,76,"PLAYER",5);text(out,p,106,76,"1",3);
        if(f.screen==BootMenuScreen::initials) {
            text(out,p,28,188,"INPUT YOUR NAME",1);
            for(int y:{151,166})for(int i=0;i<3;++i)object(out,p,89+16*i,y,76+i,3,16,false,y==166);
            for(unsigned i=0;i<3;++i)text(out,p,100+9*i,159,std::string_view(f.initials).substr(i,1),
                i==f.cursor && !(f.tick&16)?1:5);
        } else if(f.car==PlayerCar::four_wd)text(out,p,68,188,"4WD",1,12);
        else if(f.car==PlayerCar::coupe)text(out,p,64,188,"COUPE",1);
        else text(out,p,60,188,"F-TYPE",1);
    }
public:
    explicit BootMenuArt(const Rom& rom):tiles_(unpack_rom_graphics(rom,0x17cc78)),layout_(unpack_rom_graphics(rom,0x17ce78)),
        sprites_(unpack_rom_graphics(rom,0x17dd8c)),noise_(unpack_rom_graphics(rom,0x1d9068)),
        letters_(unpack_rom_graphics(rom,0x17f8e8)),cars_(rom) {
        if(tiles_.size()!=4096 || layout_.size()!=4096 || sprites_.size()!=16384 || noise_.size()!=8192 || letters_.size()!=26624)
            throw std::runtime_error("Invalid menu artwork in supplied ROM");
        // The title checker remains underneath the animated green noise for 16 fields.
        const auto checker_map=unpack_rom_graphics(rom,0x1cbecc);
        const auto sheet=rom.slice(0xe3e58,64);
        const std::vector<std::uint8_t> checker_tiles(sheet.begin(),sheet.end());
        checker_.resize(256*224);
        for(unsigned y=0;y<224;++y)for(unsigned x=0;x<256;++x){
            const auto sy=y+1,at=2*(32*(sy/8)+x/8);
            const unsigned tile=checker_map[at]|unsigned(checker_map[at+1])<<8;
            const auto index=ink(checker_tiles,tile&1023,(tile&0x4000)?7-x%8:x%8,(tile&0x8000)?7-sy%8:sy%8,2);
            checker_[256*y+x]=rom.u16(0xe189c+2*(12+index));
        }
        for(unsigned i=0;i<256;++i)colors_[i]=rom.u16(0xe63ac+2*i);
    }
    void draw(std::span<std::uint32_t> out,const BootMenuFrame& f) const {
        if(out.size()!=256*224)throw std::invalid_argument("Menu requires 256x224 pixels");
        Palette p;
        for(unsigned i=0;i<256;++i) {
            unsigned c=colors_[i];
            if(f.screen!=BootMenuScreen::game && i==217 && f.gold_stats)c=colors_[107];
            unsigned rgb=0;for(unsigned n:{0,5,10})rgb|=((((c>>n)&31)*f.brightness+7)/15)<<n;
            p[i]=hud_rgba(rgb);
        }
        for(unsigned y=0;y<224;++y)for(unsigned x=0;x<256;++x) {
            const unsigned sy=y+1,sx=(x+(y<64?f.header_scroll:f.panel_scroll))&511;
            const unsigned index=2*((sx/256)*1024+(sy/8)*32+(sx%256)/8);
            const unsigned tile=layout_[index]|unsigned(layout_[index+1])<<8;
            const auto shade=ink(tiles_,tile&1023,(tile&0x4000)?7-sx%8:sx%8,(tile&0x8000)?7-sy%8:sy%8,4);
            const unsigned nx=x-(f.entering && f.entry==BootMenuEntry::demo && f.tick==40?3:0);
            const unsigned bg=ink(noise_,16*f.noise+((sy/8)%4)*4+(nx/8)%4,nx%8,sy%8,8);
            out[y*256+x]=p[shade?(tile>>10&7)*16+shade:bg];
        }
        std::fill_n(out.begin()+223*256,256,0xff000000u);
        if(!f.show_car)for(int y=95;y<183;++y)std::fill_n(out.begin()+256*y+32,104,0xff000000u);
        if(f.show_car)cars_.draw(out,f.car,f.turntable,f.brightness);
        const bool demo_entry=f.entering && f.entry==BootMenuEntry::demo;
        const int obj_x=demo_entry && f.tick==40?3:0;
        panel(out,p,obj_x);
        if(f.screen==BootMenuScreen::game && f.show_choices)choices(out,p,f,obj_x);
        for(int i=0;i<2;++i) {
            object(out,p,12+obj_x,120+16*i,74,f.bright_arrows?3:1,16,true,i!=0);
            object(out,p,229-int(f.arrow_offset)+obj_x+(demo_entry && f.tick>=39?3:0),
                   120+16*i,74,f.bright_arrows?3:1,16,false,i!=0);
        }
        if(f.show_car || f.switching_stats)statistics(out,p,f,88-int(f.arrow_offset));
        if(f.show_car)vehicle_labels(out,p,f);
        if(f.entering && f.tick<41) {
            const unsigned n=f.tick<17?0:f.tick-17;
            const int shift=demo_entry && f.tick==40?0:f.tick<16?256:f.tick>=39?3:256-std::max(1u,n*(n+1)/2);
            for(unsigned y=0;y<224;++y){
                const auto sy=y+1;
                auto row=out.subspan(y*256,256);
                std::move_backward(row.begin(),row.end()-shift,row.end());
                for(int x=0;x<shift;++x){
                    const unsigned sx=(x-shift)&31;
                    const auto index=ink(noise_,16*f.noise+(sy/8%4)*4+sx/8,sx%8,sy%8,8);
                    const auto color=y==223?0u:colors_[index];
                    if(f.tick<16 && f.entry==BootMenuEntry::title) {
                        const auto old=checker_[y*256+x];const unsigned weight=2*(f.tick+1);
                        unsigned rgb=0;for(unsigned k:{0,5,10})rgb|=((((old>>k&31)*(32-weight))/32+((color>>k&31)*weight)/32))<<k;
                        row[x]=hud_rgba(rgb);
                    } else {
                        row[x]=y==223?0xff000000U:p[index];
                        // The scrolling tilemap exposes the end of its previous
                        // header column one DMA update before the screen edge.
                        if(demo_entry && y<64 && f.tick>=17 && x+int(n)+1>=shift) {
                            const unsigned px=(x-shift)&255;
                            const unsigned j=2*(1024+(sy/8)*32+px/8);
                            const auto tile=layout_[j]|unsigned(layout_[j+1])<<8;
                            const auto shade=ink(tiles_,tile&1023,(tile&0x4000)?7-px%8:px%8,
                                                 (tile&0x8000)?7-sy%8:sy%8,4);
                            if(shade)row[x]=p[(tile>>10&7)*16+shade];
                        }
                    }
                }
            }
        }
    }
};
}
