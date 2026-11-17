#pragma once

#include "srf/bonus_hud_tiles.hpp"
#include "srf/native_race.hpp"
#include "srf/presentation.hpp"
#include "srf/world.hpp"
#include <cmath>
#include <numbers>

namespace srf {
// The menu renders the player descriptor with a stationary camera 631 units from
// the pivot, a 128-unit focal length and a 104x88 viewport (GSU $0034..003E).
// Wheel and eye definitions come from the same attachment lists used in a race.
class BootCarArt {
    struct Point {float x,y,z;};
    struct Attachment {Vertex local; unsigned table,diameter; bool eye;};
    struct Car {WorldModel body;std::vector<Attachment> attachments;};
    const Rom& rom_;
    std::array<Car,3> cars_;
    WorldMaterials materials_;
    std::vector<WorldSprite> sprites_;
    using Palette=std::array<std::uint32_t,256>;
    const WorldSprite* sprite(unsigned table,unsigned phase) const {
        auto it=std::find_if(sprites_.begin(),sprites_.end(),[&](const auto& s){return s.material_table==table && s.phase==phase;});
        return it==sprites_.end()?nullptr:&*it;
    }
    static float edge(const Point& a,const Point& b,float x,float y) {
        return (b.x-a.x)*(y-a.y)-(b.y-a.y)*(x-a.x);
    }
    static void triangle(std::span<std::uint32_t> out,const Point& a,const Point& b,const Point& c,
            unsigned ink,const Palette& palette,const WorldSprite* texture,unsigned ia,unsigned ib,unsigned ic) {
        const auto area=edge(a,b,c.x,c.y);
        if(area>=0)return;
        const int min_x=std::max(32,int(std::floor(std::min({a.x,b.x,c.x}))));
        const int max_x=std::min(135,int(std::ceil(std::max({a.x,b.x,c.x}))));
        const int min_y=std::max(95,int(std::floor(std::min({a.y,b.y,c.y}))));
        const int max_y=std::min(182,int(std::ceil(std::max({a.y,b.y,c.y}))));
        for(int y=min_y;y<=max_y;++y)for(int x=min_x;x<=max_x;++x){
            const auto wa=edge(b,c,float(x),float(y))/area,wb=edge(c,a,float(x),float(y))/area,wc=1-wa-wb;
            if(wa<-.0001f || wb<-.0001f || wc<-.0001f)continue;
            if(texture) {
                const auto coord=[&](unsigned n){return wa*texture->texcoords[ia][n]+wb*texture->texcoords[ib][n]+wc*texture->texcoords[ic][n];};
                const auto u=std::clamp(int(std::round(coord(0))),0,int(texture->width)-1);
                const auto v=std::clamp(int(std::round(coord(1))),0,int(texture->height)-1);
                const auto index=texture->palette_indices[v*texture->width+u];
                if(index%16)out[y*256+x]=palette[index];
            } else out[y*256+x]=palette[ink];
        }
    }
public:
    explicit BootCarArt(const Rom& rom):rom_(rom),materials_(extract_world_materials(rom)),
            sprites_(extract_vehicle_sprites(rom,CourseId::night_cruise)) {
        const auto bytes=rom.bytes();
        // The menu's flat material bank is 03:A545. Race palettes use 03:A325.
        for(unsigned i=0;i<256;++i)if((materials_.source_words[i]>>8)==0x3e)
            materials_.palette_indices[i].fill(bytes[0x1a545+(materials_.source_words[i]&255)]);
        constexpr unsigned definitions[]{0x853d,0x8411,0x81b9};
        for(unsigned n=0;n<3;++n){
            auto& car=cars_[n];const auto base=0x48000+(definitions[n]&0x7fff);
            car.body=extract_world_model(rom,rom.u16(base));
            const auto add=[&](unsigned part,Vertex local,bool eye){
                const auto address=0x48000+part;
                const auto descriptor=rom.u16(address)&0x7fff;
                const auto size=rom.u16(address+4);
                const auto diameter=original_sprite_diameter(rom.u16(descriptor+16),bytes[descriptor+7],std::uint8_t(size));
                car.attachments.push_back({local,unsigned(rom.u16(address+2))+(eye?0u:8u),diameter,eye});
            };
            for(unsigned i=0;i<bytes[base+0x25];++i){
                const auto at=base+0x28+i*14;const auto part=rom.u16(at);
                const int lift=rom.u16(0x48000+part+4)+20;
                add(part,{-std::int16_t(rom.u16(at+2)),std::int16_t(rom.u16(at+4))-lift,std::int16_t(rom.u16(at+6))},false);
            }
            for(unsigned i=0;i<bytes[base+0x60];++i){
                const auto at=base+0x61+i*5;const auto part=rom.u16(at);
                if(rom.u16(0x48000+part)==0xa190)add(part,{2*std::int8_t(bytes[at+2]),2*std::int8_t(bytes[at+3]),2*std::int8_t(bytes[at+4])},true);
            }
        }
    }
    void draw(std::span<std::uint32_t> out,PlayerCar selected,unsigned phase,unsigned brightness) const {
        const auto& car=cars_[static_cast<unsigned>(selected)];
        const auto angle=float(std::numbers::pi)*(1.f-float(phase%64)/32.f);
        const auto cy=std::cos(angle),sy=std::sin(angle);
        constexpr float cp=.94953f,sp=.31368f;
        const auto project=[&](const Vertex& v) {
            const float x=cy*v.x+sy*v.z,z=cy*v.z-sy*v.x;
            const float depth=631+sp*v.y+cp*z;
            return Point{float(32+std::floor(51.8f+128*x/depth)),float(95+std::floor(43.9f+128*(cp*v.y-sp*z)/depth)),depth};
        };
        Palette palette;
        for(unsigned i=0;i<256;++i){
            const auto col=rom_.u16(0xe63ac+2*i);unsigned rgb=0;
            for(unsigned shift:{0,5,10})rgb|=((std::max(0,int(col>>shift&31)-1)*brightness+7)/15)<<shift;
            palette[i]=hud_rgba(rgb);
        }
        for(int y=95;y<183;++y)std::fill_n(out.begin()+y*256+32,104,palette[0]);
        const auto render_attachments=[&](bool nearer_layer){
            struct Item{Point point;const Attachment* attachment;};
            std::array<Item,8> items{};unsigned count=0;
            for(const auto& a:car.attachments){auto p=project(a.local);if((p.z<=631)==nearer_layer)items[count++]={p,&a};}
            std::sort(items.begin(),items.begin()+count,[](const auto& a,const auto& b){return a.point.z>b.point.z;});
            for(unsigned i=0;i<count;++i){
                const auto& a=*items[i].attachment;const auto p=items[i].point;
                const unsigned index=a.eye?rom_.bytes()[0x468af+((phase+32)%64)/2]:rom_.bytes()[0x44056+(phase+31)%64];
                const auto* s=sprite(a.table,index);if(!s)continue;
                const int dimension=std::max(1,int(std::round(128.f*a.diameter/p.z)));
                const int x0=int(p.x)-dimension/2,y0=int(p.y)-dimension/2;
                for(int y=std::max(95,y0);y<std::min(183,y0+dimension);++y)for(int x=std::max(32,x0);x<std::min(136,x0+dimension);++x) {
                    auto color=s->palette_indices[(y-y0)*s->height/dimension*unsigned(s->width)+(x-x0)*s->width/dimension];
                    if(color%16)out[y*256+x]=palette[color];
                }
            }
        };
        render_attachments(false);
        const auto& verts=car.body.frames.front();std::vector<Point> points;points.reserve(verts.size());
        for(const auto& v:verts)points.push_back(project(v));
        struct Face {float depth;const Polygon* poly;};std::vector<Face> faces;
        for(const auto& f:car.body.primitives){
            if(f.indices.size()<3)continue;
            float z=0;for(auto index:f.indices)z+=points[index].z;
            faces.push_back({z/f.indices.size(),&f});
        }
        std::stable_sort(faces.begin(),faces.end(),[](const auto& a,const auto& b){return a.depth>b.depth;});
        for(const auto& face:faces){
            const auto& f=*face.poly;const auto& v=f.indices;const auto m=f.attributes[2];
            const float nx=std::int8_t(f.attributes[3]),ny=std::int8_t(f.attributes[4]),nz=std::int8_t(f.attributes[5]);
            const auto shade=unsigned(std::clamp(int(std::round((ny+cy*nz-sy*nx)*11.f/127.f)),6,15));
            const auto index=materials_.palette_indices[m][shade];
            const auto* tex=v.size()==4?sprite(0x8f91,m):nullptr;
            for(unsigned j=1;j+1<v.size();++j)triangle(out,points[v[0]],points[v[j]],points[v[j+1]],index,palette,tex,0,j,j+1);
        }
        render_attachments(true);
    }
};
}
