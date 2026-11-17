#include "srf/presentation.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>

namespace srf {
namespace {
using Vector = std::array<std::int64_t,3>;
Vector difference(Vertex a,Vertex b) {return {std::int64_t(a.x)-b.x,std::int64_t(a.y)-b.y,std::int64_t(a.z)-b.z};}
Vector cross(Vector a,Vector b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
std::int64_t dot(Vector n,Vertex v) {return n[0]*v.x+n[1]*v.y+n[2]*v.z;}
struct Surface {
    std::size_t face;
    std::array<std::int32_t,2> low,high;
    bool textured;
    unsigned layer=0;
};

using Point2 = std::array<std::int32_t,2>;
using Triangle2 = std::array<Point2,3>;
std::int64_t side(Point2 a,Point2 b,Point2 p) {
    return (std::int64_t(b[0])-a[0])*(std::int64_t(p[1])-a[1])-
           (std::int64_t(b[1])-a[1])*(std::int64_t(p[0])-a[0]);
}
bool separated(const Triangle2& a,const Triangle2& b) {
    const auto winding=side(a[0],a[1],a[2]);
    if(!winding)return true;
    for(unsigned i=0;i<3;++i) {
        const auto outside=[&](Point2 p) {
            const auto d=side(a[i],a[(i+1)%3],p);
            return winding>0?d<=0:d>=0;
        };
        if(std::all_of(b.begin(),b.end(),outside))return true;
    }
    return false;
}
bool overlap_area(const Polygon& a,const Polygon& b,std::span<const Vertex> vertices,unsigned axis) {
    const auto corner=[&](const Polygon& face,std::size_t i) {
        const auto v=vertices[face.indices[i]];
        const std::array xyz{v.x,v.y,v.z};
        return Point2{xyz[(axis+1)%3],xyz[(axis+2)%3]};
    };
    // Use the same triangle fans as the renderer. Shared edges have zero area,
    // even when the two triangles have overlapping bounding rectangles.
    for(std::size_t i=1;i+1<a.indices.size();++i) {
        const Triangle2 first{corner(a,0),corner(a,i),corner(a,i+1)};
        for(std::size_t j=1;j+1<b.indices.size();++j) {
            const Triangle2 second{corner(b,0),corner(b,j),corner(b,j+1)};
            if(!separated(first,second) && !separated(second,first))return true;
        }
    }
    return false;
}
}

std::vector<SurfaceOffset> coplanar_surface_offsets(const WorldModel& model,
    std::size_t frame,const std::array<bool,256>& textured_materials) {
    const auto& vertices=model.frames.at(frame%model.frames.size());
    std::vector<SurfaceOffset> offsets(model.primitives.size());
    std::map<std::array<std::int64_t,4>,std::vector<Surface>> planes;
    for(std::size_t i=0;i<model.primitives.size();++i) {
        const auto& face=model.primitives[i];
        if(face.indices.size()<3)continue;
        const auto origin=vertices.at(face.indices[0]);
        Vector normal{};
        for(std::size_t k=1;k+1<face.indices.size();++k) {
            normal=cross(difference(vertices.at(face.indices[k]),origin),difference(vertices.at(face.indices[k+1]),origin));
            if(normal!=Vector{})break;
        }
        const auto divisor=std::gcd(std::gcd(normal[0],normal[1]),normal[2]);
        if(!divisor)continue;
        for(auto& component:normal)component/=divisor;
        const auto distance=dot(normal,origin);
        if(std::any_of(face.indices.begin(),face.indices.end(),[&](auto index){return dot(normal,vertices.at(index))!=distance;}))continue;
        // Keep opposite-facing sides separate. Their visibility is determined
        // by backface culling, never by a decal bias.
        const auto dominant=std::size_t(std::max_element(normal.begin(),normal.end(),
            [](auto a,auto b){return std::abs(a)<std::abs(b);})-normal.begin());
        Surface surface{i,{}, {},face.indices.size()==4 && textured_materials[face.attributes[2]],0};
        // Sunset places these chevron-only models directly on E3D8's tunnel
        // walls. Their backing is a separate instance on the same plane.
        if ((model.descriptor_address==0xb2a0 && face.attributes[2]==0xb5) ||
            (model.descriptor_address==0xb2bc && face.attributes[2]==0xb3))
            surface.layer=1;
        const unsigned axes[]{unsigned((dominant+1)%3),unsigned((dominant+2)%3)};
        for(unsigned a=0;a<2;++a) {
            const std::int32_t xyz[]{origin.x,origin.y,origin.z};
            surface.low[a]=surface.high[a]=xyz[axes[a]];
            for(auto index:face.indices) {
                const auto v=vertices.at(index);const std::int32_t p[]{v.x,v.y,v.z};
                surface.low[a]=std::min(surface.low[a],p[axes[a]]);
                surface.high[a]=std::max(surface.high[a],p[axes[a]]);
            }
        }
        planes[{normal[0],normal[1],normal[2],distance}].push_back(surface);
    }
    for(auto& [plane,surfaces]:planes) {
        // Road stripes can precede the asphalt in a flattened BSP (D8E8).
        // Keep the road on its shared edge and lift the markings instead.
        const auto layer_order=[&](const Surface& surface) {
            if(surface.textured) return 2;
            const auto material=model.primitives[surface.face].attributes[2];
            return material>=0x2a && material<=0x2e && plane[1]<0 ? 0 : 1;
        };
        std::stable_sort(surfaces.begin(),surfaces.end(),[&](const auto& a,const auto& b){return layer_order(a)<layer_order(b);});
        const double length=std::sqrt(double(plane[0])*plane[0]+double(plane[1])*plane[1]+double(plane[2])*plane[2]);
        const auto axis=static_cast<unsigned>(std::max_element(plane.begin(),plane.begin()+3,
            [](auto a,auto b){return std::abs(a)<std::abs(b);})-plane.begin());
        for(std::size_t i=0;i<surfaces.size();++i) {
            auto& surface=surfaces[i];
            for(std::size_t j=0;j<i;++j) {
                const auto& previous=surfaces[j];
                if(surface.low[0]<previous.high[0] && previous.low[0]<surface.high[0] &&
                   surface.low[1]<previous.high[1] && previous.low[1]<surface.high[1] &&
                   overlap_area(model.primitives[surface.face],model.primitives[previous.face],vertices,axis))
                    surface.layer=std::max(surface.layer,previous.layer+1);
            }
            // Move toward the source winding's front. A quarter world unit
            // per overlapping layer separates depth without a distance-varying
            // screen-space bias, which could pull far decals through geometry.
            for(unsigned a=0;a<3;++a)offsets[surface.face][a]=float(.25*surface.layer*double(plane[a])/length);
        }
    }
    // EE20 plane16/material169 is the authored windmill ground shadow. Its
    // separate BEFC road object occupies exactly the same world plane, so a
    // per-model coplanar group cannot see the overlap. Keep its tiny separation
    // local to that original shadow; the tower, blades and lines remain intact.
    if(model.descriptor_address==0xee20)for(std::size_t i=0;i<model.primitives.size();++i) {
        const auto& face=model.primitives[i];
        if(face.attributes[1]==16 && face.attributes[2]==169 && face.indices.size()==4 &&
            std::all_of(face.indices.begin(),face.indices.end(),[&](auto index){return vertices.at(index).y==0;}))
            offsets[i][1]-=.25f;
    }
    return offsets;
}

std::vector<bool> missing_barrier_backfaces(const WorldModel& model,std::size_t frame) {
    const auto& vertices=model.frames.at(frame%model.frames.size());
    std::vector<bool> result(model.primitives.size());
    const auto normal=[&](const Polygon& face) {
        Vector value{};
        if(face.indices.size()<3)return value;
        for(std::size_t k=1;k+1<face.indices.size() && value==Vector{};++k)
            value=cross(difference(vertices.at(face.indices[k]),vertices.at(face.indices[0])),
                        difference(vertices.at(face.indices[k+1]),vertices.at(face.indices[0])));
        return value;
    };
    const auto points=[&](const Polygon& face) {
        std::vector<std::array<std::int32_t,3>> value;
        for(auto index:face.indices) {const auto p=vertices.at(index);value.push_back({p.x,p.y,p.z});}
        std::sort(value.begin(),value.end());return value;
    };
    for(std::size_t i=0;i<model.primitives.size();++i) {
        const auto& face=model.primitives[i];
        // USA Rev1 materials123/124 are the plain roadside barrier surfaces.
        // F7DC omits their backs; ABBC supplies distinct reverse polygons. Fill
        // only a missing flat vertical back, preserving existing reverse art.
        if(face.attributes[2]!=123 && face.attributes[2]!=124)continue;
        const auto n=normal(face);
        if(n==Vector{} || n[1]!=0)continue;
        const auto original=points(face);
        bool reverse=false;
        for(const auto& other:model.primitives) {
            if(other.indices.size()!=face.indices.size())continue;
            const auto opposite=normal(other);
            if(double(n[0])*opposite[0]+double(n[1])*opposite[1]+double(n[2])*opposite[2]<0 && points(other)==original) {
                reverse=true;break;
            }
        }
        result[i]=!reverse;
    }
    return result;
}

std::uint8_t interpolated_angle_byte(std::uint16_t before,std::uint16_t after,float blend) {
    const auto wrapped=std::uint16_t(after-before);
    const int delta=wrapped<32768?int(wrapped):int(wrapped)-65536;
    return std::uint8_t(int(std::floor((double(before)+delta*double(blend))/256.0)));
}

int wheel_sprite_bank(std::uint8_t roll,std::uint8_t relative_yaw,std::uint8_t camera_roll) {
    // CPU08:BFBA..BFD8: byte-wrapped camera roll, signed ASR, then truncate
    // the magnitude to a multiple of four. Negative values do not round down.
    const auto combined=std::uint8_t(roll+camera_roll);
    const int value=combined<128?int(combined):int(combined)-256;
    const int half=value>=0?value/2:(value-1)/2;
    int bank=std::clamp((half/4)*4,-8,8);
    // CPU08:BFE1..BFFC includes the side-view boundary at192.
    if(relative_yaw>=64 && relative_yaw<=192)bank=-bank;
    return bank;
}

unsigned original_sprite_diameter(std::uint16_t radius,std::uint8_t scale,std::uint8_t size) {
    // GSU01:AD08..AD12 reads descriptor+10(hex), not its X extent at+0A.
    // B0F0..B105 doubles that radius and adds the scaled CPU size byte.
    // Negative size bytes instead request a fixed screen size; these are not
    // used by the recovered wheel/eye attachment definitions.
    if(scale>15 || size>=128)throw std::runtime_error("Unsupported world-sized sprite definition");
    return std::max(1u,2u*radius+(unsigned(size)<<scale));
}

bool road_support_surface(std::uint8_t scene_opcode,std::int16_t descriptor_bias,std::span<const Vertex> vertices,const Polygon& face) {
    // CPU08:8055..8060 marks opcode0C objects for background painter priority.
    // Restrict the modern depth exception to upward supporting faces: vertical
    // walls in the same original model must still occlude a vehicle behind them.
    if((scene_opcode!=0x0c && descriptor_bias<=0) || face.indices.size()<3)return false;
    const auto origin=vertices[face.indices[0]];
    for(std::size_t i=1;i+1<face.indices.size();++i) {
        const auto normal=cross(difference(vertices[face.indices[i]],origin),
                                difference(vertices[face.indices[i+1]],origin));
        if(normal!=Vector{}) {
            // A 45-degree cone is invariant under the scene's yaw rotations.
            const double up=-double(normal[1]),x=double(normal[0]),z=double(normal[2]);
            return up>0 && up*up>=x*x+z*z;
        }
    }
    return false;
}

float road_painter_depth(float camera_z,float vertical_delta,std::int16_t descriptor_bias,bool background_priority) {
    // CPU02:CD11..CD38 supplies 2*abs(Y-cameraY)+8000; GSU01:AFC0..AFD7
    // adds camera-space Z and the signed model descriptor bias at+5.
    return camera_z+2.f*std::abs(vertical_delta)+(background_priority?8000.f:0.f)+float(descriptor_bias);
}
}
