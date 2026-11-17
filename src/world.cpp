#include "srf/world.hpp"
#include "srf/stunt_course.hpp"
#include <windows.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <functional>
#include <map>
#include <numbers>
#include <set>
#include <sstream>
#include <stdexcept>

namespace srf {
namespace {
using Bytes=std::span<const std::uint8_t>;
constexpr std::array<std::uint8_t,8> magic={'S','R','F','W','R','L','D','1'};
constexpr std::size_t package_limit=128u*1024u*1024u;
struct InvalidWorld:std::runtime_error { using std::runtime_error::runtime_error; };
void check(bool good,const char* message) { if(!good) throw InvalidWorld(message); }
int s8(unsigned n) { return n>=128 ? int(n)-256 : int(n); }
int s16(Bytes b,std::size_t p) { const unsigned n=b[p]|(unsigned(b[p+1])<<8); return n>=32768 ? int(n)-65536 : int(n); }
unsigned u16(Bytes b,std::size_t p) { return b[p]|(unsigned(b[p+1])<<8); }
void need(Bytes b,std::size_t p,std::size_t n,std::size_t start) {
    check(p>=start && p<=b.size() && n<=b.size()-p && p-start<=16384 && n<=16384-(p-start),"World model exceeds bounded ROM window");
}
bool vertex_op(unsigned op) { return op==4 || op==8 || op==0x34 || op==0x38; }
struct Vertices { std::vector<Vertex> values; std::size_t end{}; unsigned frames=1; };
Vertices vertices(Bytes b,std::size_t start,unsigned exponent,unsigned frame=0) {
    check(exponent<=8,"Unsupported visual scale exponent");
    Vertices result;
    std::set<std::size_t> seen;
    auto p=start;
    for(unsigned commands=0;commands<128;++commands) {
        need(b,p,1,start); check(seen.insert(p).second,"Cyclic vertex stream");
        const auto op=b[p++];
        if(vertex_op(op)) {
            need(b,p,1,start); const unsigned count=b[p++];
            const unsigned width=(op==8 || op==0x34)?2:1;
            const bool mirror=op==0x34 || op==0x38;
            check(count && result.values.size()+count*(mirror?2:1)<=256,"Invalid world vertex count");
            need(b,p,count*3*width,start);
            const int scale=width==1 ? 1<<exponent : 1;
            for(unsigned i=0;i<count;++i) {
                int xyz[3];
                for(auto& n:xyz) { n=(width==1?s8(b[p]):s16(b,p))*scale; p+=width; }
                result.values.push_back({xyz[0],xyz[1],xyz[2]});
                if(mirror) result.values.push_back({-xyz[0],xyz[1],xyz[2]});
            }
        } else if(op==0x0c) {
            check(!result.values.empty(),"Empty world vertex stream"); result.end=p; return result;
        } else if(op==0x1c) {
            need(b,p,1,start); const unsigned count=b[p++];
            check(count && count<=64 && frame<count && (result.frames==1 || result.frames==count),"Invalid world animation count");
            result.frames=count; need(b,p,2*count,start);
            const auto word=p+2*frame;
            const auto target=std::int64_t(word)+1+s16(b,word);
            check(target>=std::int64_t(start),"Animation branch before model"); p=std::size_t(target);
        } else if(op==0x20) {
            need(b,p,2,start); const auto target=std::int64_t(p)+1+s16(b,p);
            check(target>=std::int64_t(start),"Vertex jump before model"); p=std::size_t(target);
        } else throw InvalidWorld("Unsupported world vertex command");
    }
    throw InvalidWorld("World vertex command limit");
}

class Faces {
    Bytes b;
    WorldModel& model;
    std::size_t start,first;
    std::vector<std::array<unsigned,3>> planes;
    std::map<std::size_t,std::size_t> groups;
    std::vector<std::pair<std::size_t,std::size_t>> ranges;
    std::map<std::size_t,std::vector<std::size_t>> edges;
    std::set<std::size_t> nodes;
    void bounds(std::size_t p,std::size_t n=1) { need(b,p,n,start); check(p>=first,"Control target before face stream"); }
    std::size_t relative(std::size_t p) {
        bounds(p,2); const auto target=std::int64_t(p)+1+s16(b,p);
        check(target>=std::int64_t(first),"Control target before stream"); bounds(std::size_t(target)); return std::size_t(target);
    }
    std::size_t group(std::size_t p) {
        if(groups.contains(p)) return groups.at(p);
        const auto begin=p; bounds(p); check(b[p++]==0x14,"Missing face group");
        for(unsigned i=0;i<256;++i) {
            bounds(p); const unsigned count=b[p];
            if(count==0xfe || count==0xff) { groups[begin]=p+1; ranges.emplace_back(begin,p+1); return p+1; }
            check(count>=2 && count<=32,"Invalid primitive cardinality"); bounds(p,7+count);
            Polygon primitive;
            std::copy_n(b.begin()+p+1,6,primitive.attributes.begin());
            check(count==2 ? primitive.attributes[1]==0xff : primitive.attributes[1]<planes.size(),"Primitive plane outside table");
            std::set<unsigned> unique;
            for(unsigned j=0;j<count;++j) {
                const auto index=b[p+7+j]; check(index<model.frames[0].size(),"World primitive vertex outside model");
                primitive.indices.push_back(index); unique.insert(index);
            }
            check(unique.size()>=(count==2?2u:3u),"Degenerate world primitive");
            if(count>2) {
                const double x=s8(primitive.attributes[3]),y=s8(primitive.attributes[4]),z=s8(primitive.attributes[5]);
                const auto length=std::sqrt(x*x+y*y+z*z);
                check(length>=110 && length<=140,"Invalid world polygon normal");
            }
            check(model.primitives.size()<4096,"Too many model primitives");
            model.primitives.push_back(std::move(primitive)); p+=7+count;
        }
        throw InvalidWorld("Face group primitive limit");
    }
    void bsp(std::size_t p,std::set<std::size_t>& visiting) {
        bounds(p); check(!visiting.contains(p),"BSP cycle");
        if(nodes.contains(p)) return;
        check(nodes.size()<256,"BSP node limit"); nodes.insert(p); visiting.insert(p);
        const auto op=b[p];
        if(op==0x40) ranges.emplace_back(p,p+1);
        else {
            check(op==0x28 || op==0x44,"Unsupported BSP node");
            const unsigned size=op==0x28?5:3; bounds(p,size);
            group(relative(p+(op==0x28?2:1))); ranges.emplace_back(p,p+size);
            if(op==0x28) {
                check(b[p+1]<planes.size(),"BSP plane outside table"); bsp(p+5,visiting);
                if(b[p+4]) bsp(p+4+b[p+4],visiting);
            }
        }
        visiting.erase(p);
    }
public:
    Faces(Bytes bytes,WorldModel& output):b(bytes),model(output),start(output.vertex_rom_offset),first(output.face_rom_offset) {}
    void decode() {
        auto p=first; bounds(p);
        if(b[p]==0x30) {
            bounds(p,2); const unsigned count=b[p+1]; p+=2; bounds(p,count*3+1);
            for(unsigned i=0;i<count;++i,p+=3) {
                std::array<unsigned,3> triangle={b[p],b[p+1],b[p+2]};
                for(auto v:triangle) check(v<model.frames[0].size(),"World plane vertex outside model");
                planes.push_back(triangle);
            }
        }
        first=p; model.control_opcode=b[p];
        if(b[p]==0x14) {
            const auto end=group(p); bounds(end); check(b[end]==0,"Missing direct model end");
        } else if(b[p]==0x3c) {
            std::set<std::size_t> visiting; bsp(p+1,visiting);
            std::size_t end=0; for(const auto& r:ranges) end=std::max(end,r.second);
            bounds(end); check(b[end]==0,"Missing BSP model end");
        } else if(b[p]==0x58) {
            std::set<std::size_t> boundaries;
            std::vector<std::size_t> targets;
            bool ended=false; ++p;
            for(unsigned i=0;i<1024;++i) {
                bounds(p); boundaries.insert(p); const auto op=b[p];
                if(op==0) { ended=true; break; }
                if(op==0x14) { p=group(p); continue; }
                if(op==0x64) { ranges.emplace_back(p,p+1); ++p; continue; }
                check(op==0x5c || op==0x60,"Unsupported visibility command");
                const unsigned size=op==0x5c?6:3; bounds(p,size);
                if(op==0x5c) {
                    check(b[p+1]<planes.size(),"Visibility plane outside table");
                    targets.push_back(relative(p+2)); targets.push_back(relative(p+4));
                } else targets.push_back(relative(p+1));
                ranges.emplace_back(p,p+size); p+=size;
            }
            check(ended,"Missing visibility model end");
            for(auto target:targets) check(boundaries.contains(target),"Visibility target inside another command");
        } else throw InvalidWorld("Unsupported world face control");
        std::sort(ranges.begin(),ranges.end());
        for(std::size_t i=1;i<ranges.size();++i) check(ranges[i-1].second<=ranges[i].first,"Overlapping world face controls");
    }
};
void append(std::vector<std::uint8_t>& out,std::uint32_t value) {
    for(unsigned i=0;i<4;++i) out.push_back(std::uint8_t(value>>(8*i)));
}
void xyz(std::vector<std::uint8_t>& out,Vertex v) { append(out,std::bit_cast<std::uint32_t>(v.x)); append(out,std::bit_cast<std::uint32_t>(v.y)); append(out,std::bit_cast<std::uint32_t>(v.z)); }
struct Reader {
    Bytes data; std::size_t p=0;
    Bytes take(std::size_t n) { check(p<=data.size() && n<=data.size()-p,"Truncated world package"); auto v=data.subspan(p,n); p+=n; return v; }
    std::uint32_t word() { auto b=take(4); return b[0]|(std::uint32_t(b[1])<<8)|(std::uint32_t(b[2])<<16)|(std::uint32_t(b[3])<<24); }
    Vertex vertex() { return {std::bit_cast<std::int32_t>(word()),std::bit_cast<std::int32_t>(word()),std::bit_cast<std::int32_t>(word())}; }
};
void write_new(const std::filesystem::path& path,Bytes b) {
    const auto file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    check(file!=INVALID_HANDLE_VALUE,"World output already exists or is inaccessible");
    DWORD count=0; const bool good=WriteFile(file,b.data(),DWORD(b.size()),&count,nullptr) && count==b.size(); CloseHandle(file);
    check(good,"Failed to write world assets");
}
}

WorldModel extract_world_model(const Rom& rom,std::uint16_t descriptor) {
    check(descriptor>=0x8000 && descriptor<=0xffe4,"Invalid visual descriptor address");
    const auto b=rom.bytes(); const std::size_t d=descriptor&0x7fff;
    const unsigned v=u16(b,d),bank=b[d+2],f=u16(b,d+3);
    check(v>=0x8000 && f>=0x8000 && bank>=10 && bank<=31,"Invalid visual descriptor pointers");
    WorldModel model;
    model.descriptor_address=descriptor;
    model.vertex_rom_offset=bank*0x8000+(v&0x7fff); model.face_rom_offset=bank*0x8000+(f&0x7fff);
    model.scale_exponent=b[d+7];
    auto first=vertices(b,model.vertex_rom_offset,model.scale_exponent);
    check(first.end==model.face_rom_offset,"Visual vertex stream does not meet descriptor face pointer");
    model.frames.push_back(std::move(first.values));
    for(unsigned frame=1;frame<first.frames;++frame) {
        auto next=vertices(b,model.vertex_rom_offset,model.scale_exponent,frame);
        check(next.end==first.end && next.values.size()==model.frames[0].size(),"World animation changes topology");
        model.frames.push_back(std::move(next.values));
    }
    Faces(b,model).decode();
    return model;
}

World extract_world(const Rom& rom) {
    return extract_world(rom,extract_easy_ride(rom));
}
World extract_world(const Rom& rom, const Scene& scene) {
    World world;
    const bool lake_side=scene.start_offset==course_definition(CourseId::lake_side).scene_offset;
    const bool course_signs=scene.start_offset==course_definition(CourseId::sunset_valley).scene_offset ||
                            scene.start_offset==course_definition(CourseId::night_owl).scene_offset ||
                            scene.start_offset==course_definition(CourseId::kings_forest).scene_offset ||
                            scene.start_offset==course_definition(CourseId::sea_breeze).scene_offset ||
                            scene.start_offset==course_definition(CourseId::white_land).scene_offset || lake_side ||
                            scene.start_offset==course_definition(CourseId::big_ravine).scene_offset ||
                            scene.start_offset==course_definition(CourseId::sky_ramp).scene_offset ||
                            scene.start_offset==course_definition(CourseId::harbor_city).scene_offset;
    for(const auto& o:scene.objects) {
        WorldInstance instance{o.command_offset,o.template_word,o.orientation,o.opcode,o.position};
        // These three Lake Side callbacks replace the placement's heading at spawn.
        if(lake_side && (o.callback_address==0x09c5fe || o.callback_address==0x09c605 || o.callback_address==0x09c60c))
            instance.yaw=rom.bytes()[Rom::lorom_offset(o.callback_address)+2];
        if (course_signs && (o.callback_address==0x09c5c8 || o.callback_address==0x09c5d1 ||
            o.callback_address==0x09c5ec || o.callback_address==0x09c5f5)) {
            // These sign callbacks load an immediate selector, set bit 7 and
            // store it in object+18. Material C5 indexes table 03:9BA3.
            instance.sign_phase=rom.bytes()[Rom::lorom_offset(o.callback_address)+2];
        }
        world.instances.push_back(instance);
    }
    world.route_points=scene.points;
    // The complete bank-00 descriptor inventory keeps vehicle/LOD descriptors
    // available without baking model geometry or a guessed identity into code.
    const auto b=rom.bytes();
    for(unsigned d=0;d+28<=0x8000;++d) {
        if(b[d+2]<10 || b[d+2]>31 || u16(b,d)<0x8000 || u16(b,d+3)<0x8000 || b[d+7]>8) continue;
        try { world.models.push_back(extract_world_model(rom,std::uint16_t(d|0x8000))); }
        catch(const InvalidWorld&) { }
    }
    if(std::ranges::any_of(scene.objects,[](const auto& o){return o.callback_address==0x08eddd;})) {
        // The cartridge paints the trailer liner A91C after the A938 shell.
        // Its side ribs sit at |X|=232..240, behind the |X|=224 side walls
        // under geometric depth testing. Bring just the rib vertices inside
        // the wall while retaining their authored bevels and height.
        const auto liner=std::ranges::find(world.models,0xa91c,&WorldModel::descriptor_address);
        if(liner==world.models.end())throw InvalidWorld("Missing Rock Field trailer liner");
        for(auto& frame:liner->frames)for(auto& vertex:frame) {
            if(std::abs(vertex.x)>=232)vertex.x+=vertex.x<0?17:-17;
        }
    }
    std::set<unsigned> known;
    for(const auto& model:world.models) known.insert(model.descriptor_address);
    for(const auto& instance:world.instances)
        check(instance.visual_descriptor==0xa040 || known.contains(instance.visual_descriptor),"An original course visual descriptor failed extraction");
    check(!world.models.empty(),"No native world models recovered");
    return world;
}

std::vector<std::uint8_t> encode_world(const World& world) {
    check(!world.models.empty() && world.models.size()<=4096 && world.instances.size()<=4096 && world.route_points.size()<=1024,"Invalid world dimensions");
    std::vector<std::uint8_t> out(magic.begin(),magic.end());
    const bool has_signs=std::any_of(world.instances.begin(),world.instances.end(),
        [](const auto& instance){return instance.sign_phase!=0;});
    if (has_signs) out.back()='2';
    append(out,std::uint32_t(world.models.size())); append(out,std::uint32_t(world.instances.size())); append(out,std::uint32_t(world.route_points.size()));
    for(const auto& m:world.models) {
        check(!m.frames.empty() && m.frames.size()<=64 && !m.frames[0].empty() && m.frames[0].size()<=256 && m.primitives.size()<=4096,"Invalid world model dimensions");
        append(out,m.descriptor_address); append(out,m.vertex_rom_offset); append(out,m.face_rom_offset); append(out,m.scale_exponent); append(out,m.control_opcode);
        append(out,std::uint32_t(m.frames.size())); append(out,std::uint32_t(m.frames[0].size())); append(out,std::uint32_t(m.primitives.size()));
        for(const auto& frame:m.frames) {
            check(frame.size()==m.frames[0].size(),"World animation topology mismatch");
            for(auto v:frame) xyz(out,v);
        }
        for(const auto& primitive:m.primitives) {
            check(primitive.indices.size()>=2 && primitive.indices.size()<=32,"Invalid world primitive size");
            append(out,std::uint32_t(primitive.indices.size())); out.insert(out.end(),primitive.attributes.begin(),primitive.attributes.end());
            for(auto index:primitive.indices) { check(index<m.frames[0].size(),"Invalid world primitive vertex"); append(out,index); }
        }
        check(out.size()<=package_limit,"World package exceeds 128 MiB");
    }
    for(const auto& i:world.instances) {
        append(out,i.command_offset); append(out,i.visual_descriptor); append(out,i.yaw); append(out,i.opcode); xyz(out,i.position);
        if(has_signs) append(out,i.sign_phase);
    }
    for(const auto& p:world.route_points) { append(out,p.rom_offset); xyz(out,p.position); append(out,p.unknown_word); }
    return out;
}

World decode_world(Bytes bytes) {
    check(bytes.size()<=package_limit,"World package exceeds 128 MiB"); Reader r{bytes};
    const auto prefix=r.take(magic.size());
    const bool has_signs=prefix.back()=='2';
    check(std::equal(prefix.begin(),prefix.end()-1,magic.begin()) && (prefix.back()=='1' || has_signs),"Unsupported world package");
    const auto models=r.word(),instances=r.word(),points=r.word();
    check(models && models<=4096 && instances<=4096 && points<=1024,"Invalid world package counts");
    World world; std::set<unsigned> descriptors;
    for(unsigned i=0;i<models;++i) {
        WorldModel m; const auto descriptor=r.word();
        check(descriptor>=0x8000 && descriptor<=0xffe4 && descriptors.insert(descriptor).second,"Invalid or duplicate world descriptor");
        m.descriptor_address=std::uint16_t(descriptor); m.vertex_rom_offset=r.word(); m.face_rom_offset=r.word();
        const auto scale=r.word(),control=r.word(),frames=r.word(),count=r.word(),primitives=r.word();
        check(m.vertex_rom_offset<rom_size && m.face_rom_offset<rom_size && scale<=8 &&
            (control==0x14 || control==0x3c || control==0x58) && frames && frames<=64 && count && count<=256 && primitives<=4096,"Invalid world model metadata");
        check(std::size_t(frames)*count*12<=bytes.size()-r.p,"Truncated world vertex frames");
        m.scale_exponent=std::uint8_t(scale); m.control_opcode=std::uint8_t(control);
        for(unsigned f=0;f<frames;++f) {
            std::vector<Vertex> frame;
            for(unsigned v=0;v<count;++v) {
                const auto vertex=r.vertex();
                for(auto n:{vertex.x,vertex.y,vertex.z}) check(n>=-32768 && n<=32768,"World vertex coordinate out of range");
                frame.push_back(vertex);
            }
            m.frames.push_back(std::move(frame));
        }
        for(unsigned f=0;f<primitives;++f) {
            const auto n=r.word(); check(n>=2 && n<=32,"Invalid world primitive count"); Polygon p;
            const auto attrs=r.take(6); std::copy(attrs.begin(),attrs.end(),p.attributes.begin());
            for(unsigned j=0;j<n;++j) { const auto index=r.word(); check(index<count,"World vertex index outside model"); p.indices.push_back(index); }
            m.primitives.push_back(std::move(p));
        }
        world.models.push_back(std::move(m));
    }
    for(unsigned n=0;n<instances;++n) {
        WorldInstance i; i.command_offset=r.word(); const auto descriptor=r.word(),yaw=r.word(),op=r.word();
        check(i.command_offset<rom_size && descriptor>=0x8000 && descriptor<=0xffff && yaw<=255 && op<=255 && (descriptor==0xa040 || descriptors.contains(descriptor)),"Invalid world instance");
        i.visual_descriptor=std::uint16_t(descriptor); i.yaw=std::uint8_t(yaw); i.opcode=std::uint8_t(op); i.position=r.vertex();
        if (has_signs) {
            const auto phase=r.word();
            check(phase<=7,"Unsupported sign phase");
            i.sign_phase=static_cast<std::uint8_t>(phase);
        }
        world.instances.push_back(i);
    }
    for(unsigned n=0;n<points;++n) {
        ScenePoint p; p.rom_offset=r.word(); p.position=r.vertex(); const auto metadata=r.word();
        check(p.rom_offset<rom_size && metadata<=65535,"Invalid world route point"); p.unknown_word=std::uint16_t(metadata); world.route_points.push_back(p);
    }
    check(r.p==bytes.size(),"Trailing world package bytes"); return world;
}

std::array<double,3> world_position(const Vertex& local,const WorldInstance& instance) {
    const double angle=double(instance.yaw)*2*std::numbers::pi/256;
    const auto c=std::cos(angle),s=std::sin(angle);
    return {instance.position.x+c*local.x-s*local.z,double(instance.position.y)+local.y,instance.position.z+s*local.x+c*local.z};
}

std::array<double,3> scenery_position(const Vertex& local,const SceneryPose& pose) {
    const auto angle=[](std::uint8_t value){return -double(value)*2*std::numbers::pi/256;};
    const double sy=std::sin(angle(pose.yaw)),cy=std::cos(angle(pose.yaw));
    const double sp=std::sin(angle(pose.pitch)),cp=std::cos(angle(pose.pitch));
    const double sr=std::sin(angle(pose.roll)),cr=std::cos(angle(pose.roll));
    // Same original yaw/pitch/roll convention as the existing vehicle view.
    return {pose.position.x+(sy*sr*sp+cy*cr)*local.x+(sy*cr*sp-cy*sr)*local.y+sy*cp*local.z,
            pose.position.y+sr*cp*local.x+cr*cp*local.y-sp*local.z,
            pose.position.z+(cy*sr*sp-sy*cr)*local.x+(cy*cr*sp+sy*sr)*local.y+cy*cp*local.z};
}

static WorldPalette indexed_world_palette(const Rom& rom,unsigned index,bool dusk) {
    const auto pointer = [&](unsigned table) {
        const auto p = rom.slice(Rom::lorom_offset(table) + 3 * index, 3);
        return Rom::lorom_offset(p[0] | (p[1] << 8) | (p[2] << 16));
    };
    const auto day_base = pointer(0x03f649);
    const auto dusk_base = pointer(0x03f69d);
    const auto day_bg = pointer(0x03f6f1);
    const auto dusk_bg = pointer(0x03f745);
    WorldPalette palette;
    for (unsigned i = 0; i < palette.size(); ++i) {
        const bool backdrop_slot = index>=2 && i<16;
        const auto day = rom.u16((backdrop_slot ? day_bg : day_base) + 2*i);
        palette[i] = day;
        if (dusk) {
            const auto end = rom.u16((backdrop_slot ? dusk_bg : dusk_base) + 2*i);
            const auto mask = rom.u16(Rom::lorom_offset(0x03ef1f) + 2*(i/16));
            // CPU03:EE85 protects masked slots. CPU02:C307 skips both
            // one-word sentinel differences as well as matching endpoints.
            if (!(mask & (1u << (i%16))) && std::abs(int(day)-int(end)) > 1)
                palette[i] = end;
        }
    }
    return palette;
}

WorldPalette extract_world_palette(const Rom& rom,CourseId course,bool dusk) {
    return indexed_world_palette(rom,static_cast<unsigned>(course_definition(course).id),dusk);
}

WorldPalette extract_rock_field_palette(const Rom& rom,StuntCourse course) {
    return indexed_world_palette(rom,stunt_rom_index(course),false);
}

WorldMaterials extract_world_materials(const Rom& rom, CourseId course) {
    WorldMaterials result;
    // GSU 01:8F98..8FA9 reads material words from ROM03:8F91 using
    // polygon attribute[2]. First-race SRAM004A independently equals8F91.
    // SRAM0052=A325 selects flat-color indices; SRAM004C=A1C5 selects
    // shade ramps. RGB555 source F8800 matches230/256 live CGRAM words;
    // the remaining slots include HUD and dynamic vehicle recoloring.
    result.palette_rgb555=extract_world_palette(rom,course);
    const auto b=rom.bytes();
    for(unsigned i=0;i<256;++i) {
        const auto word=rom.u16(0x18f91+2*i); result.source_words[i]=word;
        if(word&0xc000) continue; // Texture/pattern/control path: not a flat color.
        const unsigned kind=word>>8,low=word&255;
        if(kind==0x3e || kind==0x3f) {
            const unsigned index=kind==0x3e?b[0x1a325+low]:low;
            result.palette_indices[i].fill(static_cast<std::uint8_t>(index));
            result.kinds[i]=1; result.colors_rgb555[i].fill(result.palette_rgb555[index]);
        } else {
            // Original01:9063..906E resolves the ramp;01:9090..90B2
            // clamps lighting to6..15 and reads ramp[lighting-6].
            const auto pointer=rom.u16(0x1a1c5+2*kind);
            check(pointer>=0x8000,"Material shade pointer outside ROM bank3");
            const std::size_t ramp=0x18000+(pointer&0x7fff);
            rom.slice(ramp,10); result.kinds[i]=2;
            for(unsigned shade=0;shade<16;++shade) {
                const auto index = b[ramp+std::clamp(shade,6u,15u)-6];
                result.palette_indices[i][shade] = index;
                result.colors_rgb555[i][shade] = result.palette_rgb555[index];
            }
        }
    }
    return result;
}

static std::vector<WorldSprite> extract_sprites(const Rom& rom, CourseId course, bool dusk,
                                                bool include_loading_car,bool demo,std::optional<StuntCourse> stunt={}) {
    const auto b=rom.bytes(); const auto palette=stunt?extract_rock_field_palette(rom,*stunt):extract_world_palette(rom,course,dusk);
    const bool indexed=course==CourseId::kings_forest || course==CourseId::white_land ||
                       course==CourseId::night_cruise || course==CourseId::lake_side || course==CourseId::big_ravine ||
                       course==CourseId::sky_ramp || course==CourseId::harbor_city;
    std::vector<WorldSprite> result;
    // Original material words share the same bounded texture decoder as the
    // wheel/eye animation tables. Base-table entries use phase as material ID.
    const auto extract=[&](unsigned table,unsigned phase,unsigned word,unsigned selector) {
            for(unsigned depth=0;depth<8 && (word&0xc000)==0x8000;++depth) {
                const std::size_t p=0x18000+(word&0x1fff); const unsigned mask=b[p]-1;
                word=rom.u16(p+1+2*(selector&mask));
            }
            check((word&0xc000)==0x4000,"Unsupported vehicle sprite material");
            const unsigned texture=word&255,format=(word>>8)&31;
            check(format<=16,"Invalid vehicle sprite dimensions selector");
            const std::size_t pointer=0x19d59+3*texture;
            const unsigned address=u16(b,pointer),bank=b[pointer+2];
            check(bank<=31 && address>=0x8000,"Vehicle sprite source pointer outside asset banks");
            const auto metadata=rom.u16(0x1a079+format*2);
            check(metadata>=0x8000,"Invalid sprite metadata pointer");
            const auto size=rom.u16(0x18000+(metadata&0x7fff));
            const unsigned width=(size&255)+1,height=((size>>8)&127)+1;
            check(width<=128 && height<=128,"Vehicle sprite dimensions exceed supported bounds");
            const bool flip_x=(size&0x8000)!=0,high_nibble=(word&0x2000)!=0;
            const unsigned palette_base=b[0x19fb1+texture]&0xf0;
            WorldSprite sprite{std::uint16_t(table),std::uint16_t(phase),std::uint16_t(width),std::uint16_t(height),(bank<<16)|address,{}};
            for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) {
                const auto source=bank*0x8000+((address+y*256+(flip_x?width-1-x:x))&0x7fff);
                check(source<b.size(),"Vehicle sprite texel outside ROM");
                const auto index=(b[source]>>(high_nibble?4:0))&15;
                const auto color=palette[palette_base|index];
                if(indexed)
                    sprite.palette_indices.push_back(static_cast<std::uint8_t>(palette_base|index));
                const auto expand=[](unsigned n){return (n<<3)|(n>>2);};
                const std::uint32_t pixel=expand(color&31)|(expand((color>>5)&31)<<8)|(expand((color>>10)&31)<<16)|(index?0xff000000u:0u);
                sprite.pixels.push_back(pixel);
            }
            for(unsigned corner=0;corner<4;++corner) for(unsigned axis=0;axis<2;++axis)
                sprite.texcoords[corner][axis]=b[0x18000+(metadata&0x7fff)+2+corner*2+axis];
            result.push_back(std::move(sprite));
    };
    for(const auto& [table,poses]:{std::pair{0x9bbfu,32u},std::pair{0x9bc3u,32u},std::pair{0x9bc7u,32u},std::pair{0x9bcbu,32u},std::pair{0x9bcfu,32u},std::pair{0x9aafu,13u}})
        for(unsigned phase=0;phase<poses;++phase)
            extract(table,phase,rom.u16(0x18000+(table&0x7fff)),phase);
    if(include_loading_car) {
        // 08:B66D selects the regular palette's pre-rendered distant cars.
        const auto material=rom.u16(Rom::lorom_offset(0x0399a3));
        for(unsigned phase=0;phase<32;++phase)extract(0x99a3,phase,material,phase);
    }
    const bool has_grandstands=course==CourseId::night_owl || course==CourseId::kings_forest ||
                               course==CourseId::sea_breeze || course==CourseId::white_land ||
                               course==CourseId::night_cruise || course==CourseId::lake_side || course==CourseId::big_ravine ||
                               course==CourseId::sky_ramp || course==CourseId::harbor_city;
    for(unsigned material=0;material<256;++material) {
        const unsigned word=rom.u16(0x18f91+2*material);
        // The AB68/AB84 grandstands have no animation callback.
        // GSU01:8FCF resolves material D5 through table 03:9977 with
        // their original object+18 selector zero. The alternate UVs are mirrored.
        if((word&0xc000)==0x4000 || ((has_grandstands || demo) && material==0xd5))
            extract(0x8f91,material,word,0);
    }
    if(course==CourseId::kings_forest || demo) {
        // CPU09:8B3B places the trunk and canopy as two separate billboards.
        for(unsigned table:{0x9971u,0x9973u})extract(table,0,rom.u16(0x18000+(table&0x7fff)),0);
    }
    if(course==CourseId::night_cruise) {
        // CPU09:8BD4 selects the canopy alone for the four roadside trees.
        extract(0x9973,0,rom.u16(Rom::lorom_offset(0x039973)),0);
    }
    if(course==CourseId::sky_ramp) {
        // CPU09:9FF7 sets the flyover's smoke material. A04C cycles six frames.
        const auto smoke=rom.u16(Rom::lorom_offset(0x03997c));
        for(unsigned phase=0;phase<6;++phase) extract(0x997c,phase,smoke,phase);
    }
    if(course==CourseId::white_land) {
        // CPU09:A06A uses one billboard for the intact block and another for each fragment.
        for(unsigned table:{0x996du,0x996bu})extract(table,0,rom.u16(0x18000+(table&0x7fff)),0);
    }
    if(course==CourseId::sea_breeze || course==CourseId::harbor_city) {
        // Wheel spray and low-speed ripples set actor+18 to 80..83.
        // CPU08:D56F removes the particle when it reaches selector four.
        for(unsigned table:{0x9b63u,0x9b79u}) {
            const auto material=rom.u16(0x18000+(table&0x7fff));
            for(unsigned phase=0;phase<4;++phase)extract(table,phase,material,phase);
        }
    }
    if (course==CourseId::sunset_valley || (has_grandstands && course!=CourseId::night_cruise)) {
        const auto sign=rom.u16(0x18f91+2*0xc5);
        if (course==CourseId::sea_breeze) {
            extract(0x9ba3,2,sign,2); // Four B5B0 signs use CPU09:C5C8.
        } else if (course==CourseId::big_ravine) {
            for (unsigned phase : {2u,6u}) extract(0x9ba3,phase,sign,phase);
        } else if (course!=CourseId::sunset_valley) {
            for (unsigned phase : {2u,3u}) extract(0x9ba3,phase,sign,phase);
        } else {
            for (unsigned phase : {3u,6u,7u}) extract(0x9ba3,phase,sign,phase);
        }
    }
    return result;
}

std::vector<WorldSprite> extract_vehicle_sprites(const Rom& rom,CourseId course,bool dusk,bool include_loading_car) {
    return extract_sprites(rom,course,dusk,include_loading_car,false);
}

std::vector<WorldSprite> extract_easy_ride_demo_sprites(const Rom& rom) {
    return extract_sprites(rom,CourseId::easy_ride,false,false,true);
}

std::vector<WorldSprite> extract_rock_field_sprites(const Rom& rom,StuntCourse course) {
    return extract_sprites(rom,CourseId::easy_ride,false,false,false,course);
}

std::vector<std::uint8_t> encode_world_sprites(const std::vector<WorldSprite>& sprites) {
    check(!sprites.empty() && sprites.size()<=256,"Invalid sprite package count");
    std::vector<std::uint8_t> result={'S','R','F','S','P','R','T','2'}; append(result,std::uint32_t(sprites.size()));
    const bool indexed=!sprites.front().palette_indices.empty();
    if(indexed)result[7]='3';
    for(const auto& s:sprites) {
        check(s.width && s.height && s.width<=128 && s.height<=128 && s.pixels.size()==std::size_t(s.width)*s.height,"Invalid sprite dimensions");
        for(unsigned n:{s.material_table,s.phase,s.width,s.height}) append(result,n);
        append(result,s.source_address);
        for(const auto& uv:s.texcoords) for(auto coordinate:uv) result.push_back(coordinate);
        for(auto pixel:s.pixels) append(result,pixel);
        check(indexed?s.palette_indices.size()==s.pixels.size():s.palette_indices.empty(),"Invalid indexed sprite extent");
        result.insert(result.end(),s.palette_indices.begin(),s.palette_indices.end());
    }
    return result;
}

std::vector<WorldSprite> decode_world_sprites(Bytes bytes) {
    check(bytes.size()<=16*1024*1024,"Sprite package too large"); Reader reader{bytes};
    const std::array<std::uint8_t,8> prefix={'S','R','F','S','P','R','T','2'};
    const auto actual=reader.take(8);
    const bool indexed=actual[7]=='3';
    check(std::equal(actual.begin(),actual.begin()+7,prefix.begin()) && (actual[7]=='2' || indexed),"Unsupported sprite package");
    const auto count=reader.word(); check(count && count<=256,"Invalid sprite count");
    std::vector<WorldSprite> result;
    for(unsigned i=0;i<count;++i) {
        const auto table=reader.word(),phase=reader.word(),w=reader.word(),h=reader.word(),address=reader.word();
        check(table<=65535 && phase<=255 && w && w<=128 && h && h<=128 && address<0x200000,"Invalid sprite metadata");
        WorldSprite sprite{std::uint16_t(table),std::uint16_t(phase),std::uint16_t(w),std::uint16_t(h),address,{}};
        for(auto& uv:sprite.texcoords) for(unsigned axis=0;axis<2;++axis) {
            uv[axis]=reader.take(1)[0];
            check(uv[axis]<(axis?h:w),"Texture coordinate outside original tile");
        }
        for(unsigned p=0;p<w*h;++p) sprite.pixels.push_back(reader.word());
        if(indexed) {
            const auto indices=reader.take(w*h);
            sprite.palette_indices.assign(indices.begin(),indices.end());
        }
        result.push_back(std::move(sprite));
    }
    check(reader.p==bytes.size(),"Trailing sprite package bytes"); return result;
}

void generate_world_assets(const Rom& rom,const std::filesystem::path& output,CourseId course) {
    std::filesystem::create_directories(output);
    const auto world=extract_world(rom,extract_course_scene(rom,course)); const auto bytes=encode_world(world);
    const auto materials=extract_world_materials(rom,course);
    const auto sprites=encode_world_sprites(extract_vehicle_sprites(rom,course));
    // Decode before committing generated files, including all cross references.
    (void)decode_world(bytes);
    (void)decode_world_sprites(sprites);
    std::ostringstream header;
    header<<"#pragma once\n#include <array>\nnamespace srf::generated {\ninline constexpr char world_sha256[]=\""<<sha256(bytes)<<"\";\n"
          <<"inline constexpr unsigned world_model_count="<<world.models.size()<<";\n"
          <<"inline constexpr unsigned world_instance_count="<<world.instances.size()<<";\n"
          <<"inline constexpr char sprite_sha256[]=\""<<sha256(sprites)<<"\";\n"
          <<"inline constexpr unsigned sky_rgb555="<<materials.palette_rgb555[0]<<";\n"
          <<"inline constexpr std::array<unsigned char,256> world_material_kinds={{";
    for(auto kind:materials.kinds) header<<unsigned(kind)<<',';
    header<<"}};\ninline constexpr std::array<std::array<unsigned short,16>,256> world_material_rgb555={{\n";
    for(const auto& colors:materials.colors_rgb555) { header<<"{{"; for(auto color:colors) header<<color<<','; header<<"}},\n"; }
    header<<"}};\ninline constexpr std::array<std::array<unsigned char,16>,256> world_material_indices={{\n";
    for (const auto& row : materials.palette_indices) {
        header<<"{{";
        for (const auto index : row) header<<unsigned(index)<<',';
        header<<"}},\n";
    }
    header<<"}};\n}\n";
    write_new(output/"world.srfworld",bytes);
    write_new(output/"sprites.srfsprites",sprites);
    if(course==CourseId::sunset_valley || course==CourseId::night_owl)
        write_new(output/"sprites-dusk.srfsprites",encode_world_sprites(extract_vehicle_sprites(rom,course,true)));
    const auto text=header.str(); write_new(output/"generated_world.hpp",Bytes(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
}
}

