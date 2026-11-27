#include "srf/custom_content.hpp"
#include <algorithm>
#include <cmath>

namespace srf {
namespace {
double horizontal_length2(Vertex a,Vertex b) {
    const double x=a.x-b.x,z=a.z-b.z;
    return x*x+z*z;
}
// Intersection of linear half-spaces over one movement step.
struct ContactInterval {
    double start=0,end=1;
    void require_nonnegative(double initial,double change) {
        if(change>0)start=std::max(start,-initial/change);
        else if(change<0)end=std::min(end,-initial/change);
        else if(initial<0)end=-1;
    }
};
}

std::optional<CourseWall> course_wall_from_face(std::span<const Vertex> points) {
    if(points.size()<3)return {};
    if(points.size()==4) {
        // Pair the two uprights, including the bevel on low practice curbs.
        const auto ends=[&](int i){return horizontal_length2(points[i],points[i+1])+
            horizontal_length2(points[(i+2)%4],points[(i+3)%4]);};
        const int i=ends(0)<=ends(1)?0:1;
        auto a=points[i],at=points[i+1],b=points[(i+2)%4],bt=points[(i+3)%4];
        if(a.y<at.y)std::swap(a,at);
        if(b.y<bt.y)std::swap(b,bt);
        if((a.y>at.y || b.y>bt.y) && horizontal_length2(a,b)>0)
            return CourseWall{a,b,at.y,bt.y};
        return {};
    }
    // Several ROM collision planes use a triangle as their supporting face.
    Vertex a=points.front(),b=a;
    double longest=0;
    int top=a.y,bottom=a.y;
    for(auto p:points) {
        top=std::min(top,p.y);bottom=std::max(bottom,p.y);
        for(auto q:points)if(const auto length=horizontal_length2(p,q);length>longest) {
            longest=length;a=p;b=q;
        }
    }
    if(longest==0 || top==bottom)return {};
    a.y=b.y=bottom;
    return CourseWall{a,b,top,top};
}

void add_visible_course_walls(std::vector<CourseWall>& walls,const Rom& rom,const Scene& scene) {
    if(scene.authored){add_custom_course_walls(walls,scene);return;}
    const auto physical_count=walls.size();
    const auto world=extract_world(rom,scene);
    for(const auto& instance:world.instances) {
        // 0C places static scenery. 02 places the road tiles. Animated
        // geometry already contributes its current ROM collision faces.
        if(instance.opcode!=0x0c && instance.opcode!=0x02)continue;
        const auto model=std::ranges::find(world.models,instance.visual_descriptor,&WorldModel::descriptor_address);
        if(model==world.models.end() || model->frames.size()!=1)continue;
        for(const auto& face:model->primitives) {
            if((face.attributes[2]!=123 && face.attributes[2]!=124) || face.indices.size()!=4)continue;
            std::array<Vertex,4> corners;
            for(unsigned i=0;i<4;++i) {
                const auto p=world_position(model->frames[0].at(face.indices[i]),instance);
                corners[i]={int(std::lround(p[0])),int(std::lround(p[1])),int(std::lround(p[2]))};
            }
            if(auto wall=course_wall_from_face(corners))walls.push_back(*wall);
        }
    }
    // A collision proxy can be much taller than its painted curb. Where a
    // visible panel covers the same edge, its endpoints and height govern.
    const auto painted=std::span<const CourseWall>(walls).subspan(physical_count);
    auto replaced=[&](const CourseWall& proxy) {
        return proxy.shape<0 && std::ranges::any_of(painted,[&](const CourseWall& panel) {
            const double dx=panel.b.x-panel.a.x,dz=panel.b.z-panel.a.z,length2=dx*dx+dz*dz;
            auto contains=[&](Vertex foot) {
                const double x=foot.x-panel.a.x,z=foot.z-panel.a.z;
                const double t=(x*dx+z*dz)/length2;
                return t>=0 && t<=1 && std::abs(x*dz-z*dx)<=std::sqrt(length2) &&
                       std::abs(foot.y-std::lerp(panel.a.y,panel.b.y,t))<=2;
            };
            return contains(proxy.a) && contains(proxy.b);
        });
    };
    const auto first_painted=walls.begin()+physical_count;
    walls.erase(std::remove_if(walls.begin(),first_painted,replaced),first_painted);
}

std::array<int,2> resolve_course_walls(Vehicle& car,RacePosition previous,const std::array<std::uint16_t,9>& orientation,
                                      std::span<const CourseWall> walls,std::span<const CollisionShape> shapes) {
    if(walls.empty())return {};
    auto& body=car.body;
    double x=previous[0],y=previous[1],z=previous[2];
    // The pose and velocity integrator uses wrapping signed world coordinates.
    double dx=std::int16_t(body.word(0x1e)-previous[0]),dz=std::int16_t(body.word(0x22)-previous[2]);
    const double end_y=y+std::int16_t(body.word(0x20)-previous[1]);
    double vx=std::int16_t(body.word(0x2e)),vz=std::int16_t(body.word(0x32));
    const double old_vx=vx,old_vz=vz;
    int half_width=0,half_length=0,axle_height=0;
    for(auto& wheel:car.wheels) {
        half_width=std::max(half_width,std::abs(int(std::int16_t(wheel.word(0x3c)))));
        half_length=std::max(half_length,std::abs(int(std::int16_t(wheel.word(0x40)))));
        axle_height=std::max(axle_height,int(std::int16_t(wheel.word(0x3e))));
    }
    // Three overlapping discs cover the tyres and bumper, including at
    // segment ends. The wheelbase and track come from this car's definition.
    const double radius=half_width+20,offset=std::max(0.,half_length+45-radius);
    const double fx=std::int16_t(orientation[6])/32768.,fz=std::int16_t(orientation[8])/32768.;
    bool touched=false;
    for(int pass=0;pass<4;++pass) {
        struct Hit { double time=2,nx=0,nz=0,push=0,axle=0; } hit;
        for(const auto& wall:walls) {
            if(wall.shape>=0 && shapes[wall.shape].frame!=wall.frame)continue;
            const auto& a=wall.a;const auto& b=wall.b;
            const double reach=radius+offset+1;
            if(std::max(x,x+dx)+reach<std::min(a.x,b.x) || std::min(x,x+dx)-reach>std::max(a.x,b.x) ||
               std::max(z,z+dz)+reach<std::min(a.z,b.z) || std::min(z,z+dz)-reach>std::max(a.z,b.z))continue;
            const double length=std::hypot(b.x-a.x,b.z-a.z);
            const double tx=(b.x-a.x)/length,tz=(b.z-a.z)/length;
            const double travel=dx*tx+dz*tz;
            auto height_interval=[&](double along,double along_change) {
                const double u=along/length,du=along_change/length,dy=end_y-y;
                ContactInterval interval;
                interval.require_nonnegative(y+axle_height-std::lerp(wall.top_a,wall.top_b,u),dy-du*(wall.top_b-wall.top_a));
                interval.require_nonnegative(std::lerp(a.y,b.y,u)-y+half_width,du*(b.y-a.y)-dy);
                return interval;
            };
            for(double axle:{-offset,0.,offset}) {
                const double ox=x+axle*fx-a.x,oz=z+axle*fz-a.z;
                const double along=ox*tx+oz*tz,dist=ox*tz-oz*tx;
                const double side=dist<0?-1.:1.;
                const double closing=-side*(dx*tz-dz*tx);
                const double gap_to_face=std::abs(dist)-radius;
                if(closing>0 || (closing==0 && gap_to_face<-.5)) {
                    auto interval=height_interval(along,travel);
                    interval.require_nonnegative(along,travel);
                    interval.require_nonnegative(length-along,-travel);
                    interval.require_nonnegative(-gap_to_face,closing);
                    interval.require_nonnegative(std::abs(dist)+radius,-closing);
                    const double t=interval.start;
                    if(t<=interval.end && t<hit.time)
                        hit={t,side*tz,-side*tx,std::max(0.,t*closing-gap_to_face),axle};
                }
                // At an exposed end the normal is radial, so a passing car
                // can steer round it without catching on an imaginary extension.
                for(double end:{0.,length}) {
                    const double ex=ox-end*tx,ez=oz-end*tz,approach=-(ex*dx+ez*dz);
                    const double gap=ex*ex+ez*ez-radius*radius,speed2=dx*dx+dz*dz;
                    if(approach<0 || (approach==0 && gap>=0))continue;
                    const double disc=approach*approach-speed2*gap;
                    if(disc<0)continue;
                    auto interval=height_interval(end,0);
                    const double root=std::sqrt(disc);
                    if(gap>0)interval.start=std::max(interval.start,gap/(approach+root));
                    // Only the approaching half of a round cap yields a normal impulse.
                    if(speed2>0)interval.end=std::min(interval.end,approach/speed2);
                    if(end==0)interval.require_nonnegative(-along,-travel);
                    else interval.require_nonnegative(along-length,travel);
                    const double t=interval.start;
                    if(t>interval.end || t>=hit.time)continue;
                    const double hx=ex+t*dx,hz=ez+t*dz,d=std::hypot(hx,hz);
                    if(d>0)hit={t,hx/d,hz/d,std::max(0.,radius-d),axle};
                }
            }
        }
        if(hit.time>1){x+=dx;z+=dz;break;}
        touched=true;
        x+=hit.time*dx+(hit.push+1)*hit.nx;
        z+=hit.time*dz+(hit.push+1)*hit.nz;
        y=std::lerp(y,end_y,hit.time);
        const double normal=dx*hit.nx+dz*hit.nz;
        dx=(dx-normal*hit.nx)*(1-hit.time);
        dz=(dz-normal*hit.nz)*(1-hit.time);
        const double inward=std::min(0.,vx*hit.nx+vz*hit.nz);
        // A bumper strike turns the chassis about its centre. At low speed
        // the contact settles; a hard hit sheds most of its normal speed.
        const double impulse=-inward+std::max(0.,-inward-240)*.2;
        vx+=impulse*hit.nx;vz+=impulse*hit.nz;
        const double lever=hit.axle*(fx*hit.nz-fz*hit.nx);
        const double turn=std::clamp(impulse*lever*400/(radius*radius+half_length*half_length),-900.,900.);
        body.word(0x34)=std::uint16_t(std::clamp(int(std::int16_t(body.word(0x34)))+int(std::lround(turn)),-3000,3000));
    }
    if(touched) {
        body.word(0x1e)=std::uint16_t(std::lround(x));body.word(0x22)=std::uint16_t(std::lround(z));
        vx=std::clamp(vx,-32768.,32767.);vz=std::clamp(vz,-32768.,32767.);
        body.word(0x2e)=std::uint16_t(std::lround(vx));body.word(0x32)=std::uint16_t(std::lround(vz));
    }
    return {int(std::lround(vx-old_vx)),int(std::lround(vz-old_vz))};
}
}
