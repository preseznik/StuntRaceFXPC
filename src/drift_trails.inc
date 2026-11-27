// Contact history advances on physics ticks. Rendering only interpolates and fades it.
class DriftTrails {
    using Vec=n64::Vec;
    struct Anchor {
        Vec point;
        const RoadTriangle* deck{};
        float u{},v{};
        Vec position() const {
            if(!deck)return point;
            const auto& [a,b,c]=*deck;
            Vec p{};
            for(unsigned i=0;i<3;++i)p[i]=float(c[i]+u*(a[i]-c[i])+v*(b[i]-c[i]));
            p[1]-=2;
            return p;
        }
    };
    struct Segment {
        std::array<Anchor,4> edges;
        Vec smoke,velocity;
        unsigned phase,seed;
        float strength,radius;
        std::vector<GpuVertex> paint;
    };
    struct SparkWheel {
        Vec start,end;
        float yaw,side,radius,seed;
        bool full;
    };
    PresentationRoad road;
    std::vector<RoadTriangle> paint_faces;
    std::deque<Segment> segments;
    std::vector<SparkWheel> spark_wheels;
    unsigned spark_phase{};
    static constexpr unsigned mark_life=84;
    // Some course stripes are separate raised polygons. Lay the same strip on
    // their surface, clipped to the tire footprint by the pixel shader.
    void mark_paint(Segment& segment,const Vec& start,const Vec& delta) const {
        const float cx=start[0]+delta[0]*.5f,cz=start[2]+delta[2]*.5f;
        std::array<Vec,4> planes;
        for(unsigned i=0;i<4;++i) {
            const auto& a=segment.edges[i].point;const auto& b=segment.edges[(i+1)%4].point;
            const float nx=b[2]-a[2],nz=a[0]-b[0];
            const float scale=std::copysign(1.f,nx*(cx-a[0])+nz*(cz-a[2]))/
                              (std::max(1.f,std::hypot(nx,nz))*segment.radius);
            planes[i]={nx*scale,nz*scale,-(nx*a[0]+nz*a[2])*scale};
        }
        for(const auto& face:paint_faces) {
            std::array<GpuVertex,3> mapped{};
            for(unsigned i=0;i<3;++i) {
                const auto& p=face[i];
                const auto side=[&](unsigned edge) {
                    const auto& n=planes[edge];return n[0]*float(p[0])+n[1]*float(p[2])+n[2];
                };
                mapped[i]={float(p[0]),float(p[1])-2,float(p[2]),0,side(0),side(2),side(1),side(3),2};
            }
            const auto outside=[&](auto test){return std::ranges::all_of(mapped,test);};
            if(outside([](auto& p){return p.u<0;}) || outside([](auto& p){return p.v<0;}) ||
               outside([](auto& p){return p.g<0;}) || outside([](auto& p){return p.b<0;}))continue;
            const auto& [a,b,c]=face;
            const double ax=a[0]-c[0],az=a[2]-c[2],bx=b[0]-c[0],bz=b[2]-c[2],det=ax*bz-bx*az;
            if(std::abs(det)<1.e-6)continue;
            const double x=cx-c[0],z=cz-c[2];
            const double paint_y=c[1]+((x*bz-z*bx)*(a[1]-c[1])+(ax*z-az*x)*(b[1]-c[1]))/det;
            const float road_y=(segment.edges[0].point[1]+segment.edges[2].point[1])*.5f;
            if(road_y-paint_y<0 || road_y-paint_y>48)continue;
            segment.paint.insert(segment.paint.end(),mapped.begin(),mapped.end());
        }
    }
    Anchor anchor(Vec p) const {
        Anchor result{p};
        if(auto surface=road_at(road.triangles,p[0],p[2],p[1]);surface && std::abs(surface->y-p[1])<128) {
            result.point[1]=surface->y;
            // Deck triangles update in place as bridges move.
            if(!road.decks.empty()) {
                result.deck=surface->triangle;result.u=surface->u;result.v=surface->v;
            }
        } else if(std::abs(p[1])<24)result.point[1]=0;
        result.point[1]-=2;
        return result;
    }
public:
    DriftTrails(const srf::World& world,srf::CourseId course):road(presentation_surfaces(world,course,true)) {
        for(const auto& instance:world.instances) {
            if(instance.opcode!=2)continue;
            const auto model=std::ranges::find(world.models,instance.visual_descriptor,&srf::WorldModel::descriptor_address);
            if(model==world.models.end() || model->frames.empty())continue;
            for(const auto& face:model->primitives) {
                const auto material=face.attributes[2];
                if(std::int8_t(face.attributes[4])<64 || (material!=0x1e && material!=0x27 && material!=0x32))continue;
                const auto point=[&](unsigned i){return srf::world_position(model->frames[0][face.indices[i]],instance);};
                for(unsigned i=2;i<face.indices.size();++i)paint_faces.push_back({point(0),point(i-1),point(i)});
            }
        }
    }
    DriftTrails(const DriftTrails&)=delete;
    void reset(){segments.clear();spark_wheels.clear();}
    void begin_phase(unsigned phase,std::span<const srf::SceneryPose> bridges={}) {
        if(phase<=1)reset();
        spark_wheels.clear();
        spark_phase=phase;
        while(!segments.empty() && phase-segments.front().phase>=mark_life)segments.pop_front();
        road.sample(bridges);
    }
    void emit(unsigned phase,unsigned actor,const srf::Vehicle& previous,const srf::Vehicle& car) {
        if(!car.drift_ticks || ((previous.body.word(0x16)|car.body.word(0x16))&0x400))return;
        const float speed=std::abs(signed_word(car.body.word(0x4a)));
        const float yaw=lerp_angle(car.body.word(0x24),car.body.word(0x24),1);
        for(unsigned i=0;i<car.wheels.size();++i) {
            const auto& old=previous.wheels[i];const auto& wheel=car.wheels[i];
            if(signed_word(wheel.word(0x40))>=0 || !WetTireSpray::grounded(wheel))continue;
            Vec start{},end{},delta{};
            for(unsigned a=0;a<3;++a) {
                start[a]=WetTireSpray::contact(old,a);end[a]=WetTireSpray::contact(wheel,a);
                delta[a]=end[a]-start[a];
            }
            const float distance=std::hypot(delta[0],delta[2]);
            const bool continuous=WetTireSpray::grounded(old) && distance<=speed*2+256 && std::abs(delta[1])<=speed+128;
            if(car.drift_charge_ticks>=srf::drift_boost_min_ticks) {
                spark_wheels.push_back({continuous?start:end,end,yaw,signed_word(wheel.word(0x3c))<0?-1.f:1.f,
                    car.tire_diameters[i]*.5f,float((actor*11+i*7)%13)/13,car.drift_charge_ticks>=srf::drift_boost_full_ticks});
            }
            if(!continuous || distance<3)continue;
            const float slip=std::abs(delta[0]*std::cos(yaw)+delta[2]*std::sin(yaw))/distance;
            if(!car.drift_direction && slip<.06f)continue;
            const float strength=std::clamp(.32f+slip,.32f,1.f)*car.drift_ticks/4.f;
            const float half=std::max(6.f,car.tire_diameters[i]*.13f);
            const auto edge=[&](Vec p,const srf::VehicleState& body,float side) {
                // Keep the strip tire-width even when the car is sliding sideways.
                const float vx=float(signed_word(body.word(0x2e))),vz=float(signed_word(body.word(0x32)));
                const float v=std::hypot(vx,vz);
                p[0]+=side*half*(v>1?vz/v:delta[2]/distance);
                p[2]-=side*half*(v>1?vx/v:delta[0]/distance);
                return anchor(p);
            };
            const std::array edges{edge(start,previous.body,-1),edge(start,previous.body,1),
                                   edge(end,car.body,1),edge(end,car.body,-1)};
            auto velocity=delta;for(auto& d:velocity)d*=.09f;
            segments.push_back({edges,end,velocity,phase,(phase*17+actor*37+i*11)%31,strength,half,{}});
            mark_paint(segments.back(),start,delta);
        }
    }
    void mesh(std::vector<GpuVertex>& out,const Renderer& renderer,float phase) const {
        const auto quad=[&](const std::array<GpuVertex,4>& q){for(auto i:{0,1,2,0,2,3})out.push_back(q[i]);};
        for(const auto& s:segments) {
            const float age=phase-s.phase;
            if(age<=-1 || age>=mark_life)continue;
            const float opacity=(.45f+.35f*s.strength)*std::clamp((mark_life-age)/22.f,0.f,1.f);
            std::array<GpuVertex,4> q;
            for(unsigned i=0;i<4;++i) {
                auto p=s.edges[i].position();
                if(age<0 && i>=2) {
                    const auto start=s.edges[3-i].position();
                    for(unsigned a=0;a<3;++a)p[a]=std::lerp(start[a],p[a],age+1);
                }
                p=renderer.normalize_world(p);
                q[i]={p[0],p[1],p[2],opacity,0,0,(i==1 || i==2)?1.f:-1.f,0,0};
            }
            quad(q);
            for(auto vertex:s.paint) {
                const auto p=renderer.normalize_world({vertex.x,vertex.y,vertex.z});
                vertex.x=p[0];vertex.y=p[1];vertex.z=p[2];vertex.r=opacity;
                if(age<0)vertex.b=std::lerp(-vertex.g,vertex.b,age+1);
                out.push_back(vertex);
            }
        }
        const float cy=std::cos(controls.yaw),sy=std::sin(controls.yaw),cp=std::cos(controls.pitch),sp=std::sin(controls.pitch);
        for(const auto& s:segments) {
            const float age=phase-s.phase,t=age/8.f;
            if(t<0 || t>=1)continue;
            auto p=s.smoke;
            for(unsigned a=0;a<3;++a)p[a]+=s.velocity[a]*age;
            p[1]-=s.radius*(.85f+4.5f*t);
            const auto c=renderer.normalize_world(p);
            const float radius=renderer.normalized_distance(s.radius*(1.4f+3.2f*t));
            std::array<GpuVertex,4> q;
            for(unsigned i=0;i<4;++i) {
                const float u=(i==1 || i==2)?1.f:-1.f,v=i>=2?1.f:-1.f;
                const float x=u*radius,y=v*radius*.83f;
                q[i]={c[0]+cy*x+sy*sp*y,c[1]+cp*y,c[2]+sy*x-cy*sp*y,
                      (.24f+.16f*s.strength)*(1-t)*(1-t),float(s.seed),t,u,v,1};
            }
            quad(q);
        }
        if(phase<float(spark_phase)-1 || phase>float(spark_phase))return;
        const float blend=phase-float(spark_phase)+1;
        for(const auto& wheel:spark_wheels) {
            const float c=std::cos(wheel.yaw),s=std::sin(wheel.yaw);
            Vec contact;
            for(unsigned a=0;a<3;++a)contact[a]=std::lerp(wheel.start[a],wheel.end[a],blend);
            const auto point=[&](float outward,float back,float up,const Vec& color) {
                const auto p=renderer.normalize_world({contact[0]+wheel.radius*(c*wheel.side*outward+s*back),
                    contact[1]-wheel.radius*up,contact[2]+wheel.radius*(s*wheel.side*outward-c*back)});
                return GpuVertex{p[0],p[1],p[2],color[0],color[1],color[2],0,0,3};
            };
            const Vec color=wheel.full?Vec{1.f,.47f,.025f}:Vec{.025f,.69f,1.f};
            const Vec hot=wheel.full?Vec{1.f,.94f,.48f}:Vec{.52f,.97f,1.f};
            // Stagger short tapered streaks around each tire's outside contact patch.
            for(int j=0;j<(wheel.full?5:3);++j) {
                const float t=std::fmod(phase*(.31f+.043f*j)+wheel.seed+.382f*j,1.f);
                const float fan=.50f+.15f*j,back=.90f-.13f*j,rise=.35f+.12f*(j%3);
                const float head=.48f+t*(wheel.full?1.18f:.80f);
                const float tail=head-(wheel.full?.46f:.32f),mid=head-.09f;
                const float width=(wheel.full?.075f:.06f)*(1-.45f*t);
                const auto streak=[&](float reach,float edge,const Vec& tint) {
                    return point(.22f+fan*reach,.27f+back*reach,.07f+rise*reach+edge,tint);
                };
                quad({streak(tail,0,color),streak(mid,width,color),streak(head,0,hot),streak(mid,-width,hot)});
            }
        }
    }
};

void Renderer::load_drift(const DriftTrails& trails,float phase) {
    std::vector<GpuVertex> vertices;
    trails.mesh(vertices,*this,phase);
    drift_count=static_cast<UINT>(vertices.size());
    if(drift_count)upload_vertices(vertices,drift_vertices);
}
