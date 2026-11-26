namespace {
// The launcher supplies one ordered grid of inputs per tick over the child's private pipes.
// Numbering includes the grid countdown. There is at most one outstanding local input.
int play_lan_race(GameHost* host,Renderer& renderer,const srf::Rom& rom,const srf::World& world,
                  srf::CourseId course,unsigned slot,std::span<const srf::PlayerCar> grid,const std::filesystem::path& controller,
                  const std::filesystem::path& capture,bool drop_in,bool capture_handoff) {
    using namespace srf;
    using Clock=std::chrono::steady_clock;
    const auto& local_course=lan_courses.at(static_cast<unsigned>(course));
    const auto own=grid[slot];
    LocalRace race(rom,local_course,grid[0],grid[1],false,grid.subspan(2));
    const bool cruise_test=!host && capture_handoff && course==CourseId::night_cruise;
    const bool lake_test=!host && capture_handoff && course==CourseId::lake_side;
    const bool ravine_test=!host && course==CourseId::big_ravine;
    const bool sky_test=!host && capture_handoff && course==CourseId::sky_ramp;
    const bool harbor_test=!host && capture_handoff && course==CourseId::harbor_city;
    const auto sky_gate=std::ranges::find(race.scene().objects,0x099bd7u,&SceneObject::callback_address);
    const auto route=extract_route(race.scene());
    CourseEnvironment environment(rom,course);
    renderer.load_world(world,0);
    CourseScenery scenery(rom,course);
    WetTireSpray spray;
    const bool wet=renderer.rain_spray_enabled();
    auto road=presentation_surfaces(world,course,race_camera.enabled);
    BootWindowInput keys;
    std::optional<GameScreen> screen;
    std::unique_ptr<Gamepad> pad;
    std::unique_ptr<AudioDevice> audio;
    RaceAudioState motor{};
    const auto tune=extract_race_audio_tuning(rom.bytes(),race.car(slot).body.word(0x66));
    if(host){
        const auto title=std::wstring(local_course.name)+L" — LAN Quick Race";
        screen.emplace(*host,title.c_str(),stunt_window_proc,&keys);
        try{pad=std::make_unique<Gamepad>(controller);}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
        try{audio=std::make_unique<AudioDevice>(extract_course_audio(rom,course));audio->play_music();}
        catch(const std::exception& e){std::cerr<<e.what()<<'\n';}
    }
    BonusDashboard dashboard(rom,own);
    BonusHudBoost boost(rom,0);
    CourseHudMap map(rom,local_course.index);
    RaceHudText notices(rom);
    const auto tiles=extract_bonus_hud_tiles(rom), signal=extract_start_light_tiles(rom);
    const auto palette=bonus_hud_palette(rom,0xf8960);
    std::vector<std::uint32_t> overlay(208*128);
    std::vector<Vehicle> previous(grid.size());
    const auto snapshot=[&]{for(unsigned i=0;i<grid.size();++i)previous[i]=race.car(i);};
    snapshot();
    constexpr unsigned countdown=42;
    unsigned tick=0;
    const auto draw_course=[&](float fraction,std::span<const SceneryPose> actors,const EnvironmentPresentation& sky,std::span<const SceneryPose> decks,
                               std::span<const SceneryPose> props,std::span<const ScenerySprite> particles,std::span<const SceneryPose> rocks={},std::span<const SceneryPose> bonus={}){
        std::vector<RenderVehicle> poses(grid.size());
        for(unsigned i=0;i<grid.size();++i){
            const auto& car=race.car(i);
            poses[i]=vehicle_pose(rom,previous[i],car,i==slot,fraction,std::uint8_t(race.car(slot).body.word(0x24)>>8));
            poses[i].remote_rally_truck=i!=slot && grid[i]==PlayerCar::four_wd;
        }
        const auto& b=race.car(slot).body;
        const auto surface=road.sample(race.bridge_poses());
        if(race_camera.enabled)seat_opponents(poses,surface);
        chase_camera(renderer,poses[slot],lerp_angle(previous[slot].body.word(0x24),b.word(0x24),fraction),surface,course);
        const float phase=float(race.phase())+fraction;
        renderer.set_environment(sky);
        renderer.set_race_phase(phase);
        renderer.load_world(world,0,poses,&rom,actors,rocks,props,decks,scenery.sprites(),nullptr,{},particles,bonus);
        if(wet)renderer.load_spray(spray,phase);
        if(race_hud_visible || tick<countdown){
            std::fill(overlay.begin(),overlay.end(),0);
            std::span<const std::uint32_t> panel;
            if(race_hud_visible){
                notices.draw(overlay,"LAP "+std::to_string(std::min(3,1+b.word(0x7a)))+"/3",6);
                map.draw_route(overlay,5);
                for(unsigned i=0;i<grid.size();++i)map.draw_marker(overlay,race.car(i),i==slot?1:6);
                boost.draw_at(overlay,0,b);
                draw_race_stopwatch(overlay,148,race.phase()*100/14,tiles,palette);
                panel=dashboard.draw(std::int16_t(b.word(0x4a)),b.word(0x82),0,race.standings()[slot]);
            }
            if(tick<countdown)draw_start_signal(overlay,72,1+tick/14,signal,palette);
            renderer.set_bonus_overlay(overlay,15,1,40,panel,0,224,tick<countdown);
        }else renderer.clear_bonus_overlay();
        renderer.render();
    };
    const auto draw_scene=[&](float fraction,std::span<const SceneryPose> actors,const EnvironmentPresentation& sky){
        draw_course(fraction,actors,sky,race.bridge_poses(),race.prop_poses(),race.ice()?race.ice()->sprites():race.spray_sprites(),race.snowballs(),race.bonus_poses());
    };
    const auto draw=[&](float fraction){draw_scene(fraction,scenery.poses(),environment.presentation());};
    struct Frame {
        std::array<std::uint16_t,max_lan_drivers> buttons{};
        std::array<std::optional<std::int16_t>,max_lan_drivers> steering{};
    };
    const auto read_controls=[&](std::istringstream& line){
        Frame frame{};
        for(unsigned i=0;i<grid.size();++i){
            unsigned buttons;int axis;
            if(!(line>>buttons>>axis) || buttons>65535 || axis< -32768 || axis>32768)
                throw std::runtime_error("Invalid LAN controls");
            frame.buttons[i]=std::uint16_t(buttons);
            if(axis!=32768)frame.steering[i]=std::int16_t(axis);
        }
        std::string extra;
        if(line>>extra)throw std::runtime_error("Oversized LAN grid");
        return frame;
    };
    const auto advance=[&](const Frame& frame){
        snapshot();
        if(tick>=countdown){
            const auto phase=race.phase();
            const auto& local=previous[slot].body;
            environment.advance_video((phase+1)*60/14-phase*60/14);
            if(course==CourseId::sky_ramp)scenery.step(previous[0].body,previous[0].body.word(0x7a),std::span(previous).subspan(1));
            else scenery.step(local,local.word(0x7a));
            environment.step(local,local.word(0x7a),(phase+1)*100/14);
            const bool gate_open=cruise_test && std::ranges::any_of(race.bonus_poses(),[](const auto& p){return p.command_offset && p.pitch;});
            race.step(frame.buttons,frame.steering);
            if(sky_test)for(auto sound:race.bonus_effects()){
                if(sound!=0x0e && sound!=0x59)continue;
                const auto& p=race.bonus_poses().front().position;
                std::cerr<<"SKY "<<(sound==0x59?"strike":"flight")<<' '<<tick+1<<' '<<p.x<<' '<<p.y<<' '<<p.z<<std::endl;
            }
            if(cruise_test){
                if(race.boost_owner() && race.boost_available())
                    throw std::runtime_error("Collected LAN boost became available again");
                if(phase==30)std::cerr<<"CRUISE driving "<<tick+1<<std::endl;
                if(!gate_open && std::ranges::any_of(race.bonus_poses(),[](const auto& p){return p.command_offset && p.pitch;}))
                    std::cerr<<"CRUISE gate "<<tick+1<<std::endl;
                for(auto sound:race.bonus_effects()){
                    if(sound==0x0e)std::cerr<<"CRUISE flight "<<tick+1<<std::endl;
                    if(sound==0x25)std::cerr<<"CRUISE award "<<tick+1<<' '<<*race.boost_owner()
                        <<' '<<previous[0].body.word(0x6a)<<' '<<race.car(0).body.word(0x6a)
                        <<' '<<previous[1].body.word(0x6a)<<' '<<race.car(1).body.word(0x6a)<<std::endl;
                }
            }
            if(wet){
                spray.begin_phase(race.phase());
                for(unsigned i=0;i<grid.size();++i)if(spray_exposed(course,environment,race.car(i).body))spray.emit(race.phase(),i,previous[i],race.car(i));
            }
        }
    };
    const auto route_input=[&](unsigned driver,int pace,bool target_snowman=false){
        const auto& b=race.car(driver).body;
        auto target=route.at(b.word(0x58)%route.size()).position;
        if(target_snowman && b.word(0x7a)==0){
            // The offscreen host takes the inside line into the first snowman using ordinary steering.
            const auto& prop=race.snowmen().front();
            const int x=std::int16_t(prop.state.word(0x1e)),z=std::int16_t(prop.state.word(0x22));
            if(!race.ice()->solid_retired(prop.command_offset) &&
               std::hypot(x-std::int16_t(b.word(0x1e)),z-std::int16_t(b.word(0x22)))<4000){
                target[0]=x;target[2]=z;
            }
        }
        if(cruise_test && driver==1){
            // Steer the test Coupe through the third folding gate, then resume its racing line.
            const auto& gate=race.bonus_poses()[2];
            if(b.word(0x7a)==0 && !gate.pitch && std::hypot(gate.position.x-std::int16_t(b.word(0x1e)),gate.position.z-std::int16_t(b.word(0x22)))<10000){
                target[0]=gate.position.x;target[2]=gate.position.z;
            }
            pace=35;
        }
        if(ravine_test)pace=driver?5:30; // Put the test Coupe on the approach when the rocks fall.
        bool sky_boost=false;
        if(sky_test || harbor_test)pace=driver?5:10;
        if(sky_test && driver==1 && b.word(0x7a)==1){
            // Drive the Coupe across the aircraft sensor with normal steering and boost.
            const auto gate=sky_gate->position;
            const int dx=gate.x-std::int16_t(b.word(0x1e)),dz=gate.z-std::int16_t(b.word(0x22));
            if(dx>0 && std::hypot(dx,dz)<14000){target[0]=gate.x;target[2]=gate.z-2500;}
            sky_boost=dx>0 && dx<14000 && std::abs(dz)<5000;
        }
        const double angle=std::atan2(double(target[0])-std::int16_t(b.word(0x1e)),double(target[2])-std::int16_t(b.word(0x22)));
        const auto error=std::int16_t(int(angle*65536/6.28318530718)+b.word(0x24));
        std::uint16_t buttons=error>600?2:error< -600?1:0;
        if(b.word(0x4a)<(std::abs(int(error))>4800?145:275)-pace)buttons|=0x10;
        if(cruise_test && driver==1 && race.phase()>80 && race.phase()<95)buttons|=1024;
        if(sky_boost)buttons|=1024;
        return buttons;
    };
    if(drop_in){
        std::cout<<"LOADED"<<std::endl;
        std::string replay;
        while(true){
            if(!std::getline(std::cin,replay))throw std::runtime_error("LAN replay interrupted");
            std::istringstream line(replay);
            unsigned seq;std::string type,extra;
            if(!(line>>type>>seq) || seq!=tick)throw std::runtime_error("Invalid replay sequence");
            if(type=="SYNC" && !(line>>extra))break;
            if(type!="FRAME" || tick>=1000000 || race.result())throw std::runtime_error("Invalid replay frame");
            advance(read_controls(line));
            ++tick;
            if(host && tick%64==0){
                MSG msg{};
                while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
                if(game_closing || keys.escape || keys.quit)return 1;
            }
        }
    }
    HANDLE pipe=GetStdHandle(STD_INPUT_HANDLE);
    std::string pending;
    auto next=Clock::now(),last_step=next,last_response=next;
    bool sent=false,last_h=false,captured=false;
    bool lake_contact=false,lake_deck=false,lake_creature=false;
    unsigned lake_water_stage=0;
    unsigned cloud_stage=0,dawn_stage=0;
    bool formation=false,smoke=false;
    std::vector<bool> folded_signs(race.prop_poses().size());
    std::array<unsigned,6> ravine_deck_frames{};
    std::array<bool,4> ravine_rock_capture{};
    bool ravine_flash=false,ravine_bridge=false;
    bool aircraft_captured=false,boost_captured=false;
    unsigned skyline_samples=0,first_sky_weight=0,dusk_route=0,dusk_lap=0;
    const auto bonus_status=[&](const char* label){
        const auto poses=race.bonus_poses();
        const bool visible=std::ranges::any_of(poses,[](const auto& p){return p.visual_descriptor==0xb7a8 && p.visible;});
        std::cerr<<"CRUISE "<<label<<' '<<tick<<' '<<race.boost_owner().value_or(2)<<' '<<visible
                 <<' '<<race.car(0).body.word(0x6a)<<' '<<race.car(1).body.word(0x6a)<<std::endl;
    };
    bool snowman_captured=false;
    unsigned snowball_tick=0,snowball_samples=0;
    bool water_captured=false;
    std::array<bool,2> barrier_captured{};
    unsigned forest_fog_stage=0;
    std::array<unsigned,2> forest_deck_stage{};
    unsigned aqua_capture_stage=0;
    unsigned sunset_capture_stage=0;
    unsigned event_pixels=0,event_tick=0,event_lap=0;
    std::vector<BYTE> event_frame,event_control;
    const auto capture_path=[&](std::string_view label){
        return capture.parent_path()/(capture.stem().string()+"-"+std::string(label)+".png");
    };
    const auto changed_pixels=[](const auto& live,const auto& control){
        unsigned pixels=0;
        for(std::size_t p=0;p<live.size();p+=4)
            if(!std::equal(live.begin()+p,live.begin()+p+3,control.begin()+p))++pixels;
        return pixels;
    };
    const auto capture_difference=[&](const std::string& label,unsigned minimum,auto draw_control,bool whole_actor=false){
        draw_control();
        auto control=renderer.readback_bgra();
        draw(1);
        auto live=renderer.readback_bgra();
        const unsigned count=changed_pixels(live,control);
        if(count<minimum)return 0u;
        if(whole_actor){
            const auto changed=[&](unsigned x,unsigned y){
                const auto i=4*(y*controls.width+x);
                return !std::equal(live.begin()+i,live.begin()+i+3,control.begin()+i);
            };
            for(unsigned x=0;x<controls.width;++x)
                if(changed(x,0) || changed(x,controls.height-1))return 0u;
            for(unsigned y=0;y<controls.height;++y)
                if(changed(0,y) || changed(controls.width-1,y))return 0u;
        }
        Renderer::write_png(capture_path(label),live,controls.width,controls.height);
        Renderer::write_png(capture_path(label+"-control"),control,controls.width,controls.height);
        return count;
    };
    while(!race.result()){
        MSG msg{};
        while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){
            if(msg.message==WM_QUIT){keys.quit=true;break;}
            TranslateMessage(&msg);DispatchMessageW(&msg);
        }
        if(game_closing || keys.escape || keys.quit){std::cout<<"QUIT"<<std::endl;return 0;}
        const auto now=Clock::now();
        if(now-last_response>std::chrono::seconds(45))throw std::runtime_error("LAN peer timed out");
        // Give the hidden launcher's real discovery scan time to finish during lap one.
        const bool pace_discovery=cruise_test && slot==0 && !captured && tick>=countdown && tick<280;
        const bool sky_scan=sky_test && !slot && !captured && race.car(0).body.word(0x7a)==1;
        if(!sent && (!host || now>=next) && (!pace_discovery || now-last_step>=std::chrono::milliseconds(32)) &&
           (!sky_scan || now-last_step>=std::chrono::milliseconds(71))){
            GamepadInput input{};
            if(host && game_focused(host->window)){
                input=combine_player_input(local_keyboard(keys,false),pad?pad->poll(true,race_camera.look):GamepadInput{});
            }else if(!host){
                // Offscreen regression driver uses the same controls and route as a human car.
                const bool snow_test=capture_handoff && course==CourseId::white_land;
                const int pace=snow_test ? (!slot && race.car(0).body.word(0x7a)>0 ? 65 : 0) : int(slot)*20;
                input.requested=route_input(slot,pace,snow_test && !slot);
            }
            // Vehicle records include the selected ROM model, pose, wheels and route progress.
            std::uint32_t digest=2166136261u;
            const auto hash=[&](const VehicleState& state){for(auto word:state.words)digest=(digest^word)*16777619u;};
            for(unsigned i=0;i<grid.size();++i){hash(race.car(i).body);for(const auto& w:race.car(i).wheels)hash(w);}
            for(const auto& pose:race.prop_poses()){
                for(auto value:{pose.position.x,pose.position.y,pose.position.z,int(pose.yaw)})
                    digest=(digest^std::uint32_t(value))*16777619u;
            }
            for(const auto& prop:race.snowmen())if(race.ice()->solid_retired(prop.command_offset))
                digest=(digest^prop.command_offset)*16777619u;
            for(const auto& ball:race.snowballs())
                for(auto value:{ball.position.x,ball.position.y,ball.position.z,int(ball.yaw)})
                    digest=(digest^std::uint32_t(value))*16777619u;
            if(course==CourseId::night_cruise){
                digest=(digest^race.boost_owner().value_or(2))*16777619u;
                for(const auto& pose:race.bonus_poses())
                    for(int v:{pose.position.x,pose.position.y,pose.position.z,int(pose.pitch),int(pose.yaw),int(pose.visible)})
                        digest=(digest^std::uint32_t(v))*16777619u;
            }
            for(const auto& deck:race.bridge_poses())digest=(digest^deck.frame)*16777619u;
            if(course==CourseId::sky_ramp || course==CourseId::harbor_city){
                const auto mix=[&](int v){digest=(digest^std::uint32_t(v))*16777619u;};
                const auto mix_pose=[&](const SceneryPose& p){
                    for(int v:{int(p.command_offset),p.position.x,p.position.y,p.position.z,int(p.frame),int(p.yaw),int(p.pitch),int(p.roll),int(p.visible)})mix(v);
                };
                for(const auto& p:race.prop_poses())mix_pose(p);
                for(const auto& p:race.bonus_poses())mix_pose(p);
                for(const auto& p:scenery.poses())if(p.visual_descriptor==0xaa34)mix_pose(p);
                for(const auto& p:race.spray_sprites())
                    for(int v:{p.position.x,p.position.y,p.position.z,int(p.table),int(p.frame)})mix(v);
            }
            std::cout<<"INPUT "<<tick<<' '<<input.requested<<' '<<(input.steering_q15?int(*input.steering_q15):32768);
            for(unsigned i=1;i<grid.size();++i)std::cout<<' '<<route_input(i,35);
            for(unsigned i=0;i<grid.size();++i)std::cout<<' '<<race.car(i).body.word(0x66)<<' '<<race.car(i).body.word(0x7a);
            std::cout<<' '<<digest<<std::endl;
            sent=true;
        }
        DWORD available=0;
        if(!PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr))throw std::runtime_error("LAN launcher disconnected");
        if(available){
            char bytes[128];DWORD n=0;
            if(available>sizeof(bytes) || !ReadFile(pipe,bytes,available,&n,nullptr))throw std::runtime_error("Invalid LAN input frame");
            pending.append(bytes,n);
            if(pending.size()>128)throw std::runtime_error("Oversized LAN input frame");
            if(pending.ends_with('\n')){
                std::istringstream line(pending);
                unsigned seq;std::string type;
                if(!sent || !(line>>type>>seq) || (type!="FRAME" && type!="TAKE") || seq!=tick)
                    throw std::runtime_error("Out-of-order LAN input");
                const auto frame=read_controls(line);
                if(!host && capture_handoff && !captured && type=="TAKE"){
                    draw(1);renderer.screenshot(capture);captured=true;
                    if(cruise_test)bonus_status("takeover");
                    for(const auto& prop:race.snowmen())if(race.ice()->solid_retired(prop.command_offset))
                        std::cerr<<"WHITE retired "<<tick<<' '<<prop.command_offset<<std::endl;
                }
                advance(frame);
                if(audio){
                    const unsigned buttons=frame.buttons[slot];
                    const bool throttle=(buttons&16)!=0 && tick>=countdown;
                    auto events=update_race_audio(motor,tune,race.car(slot),{false,throttle});
                    const auto& surroundings=environment.events();
                    if(tick>=countdown){
                        events.commands.insert(events.commands.end(),surroundings.sound_commands.begin(),surroundings.sound_commands.end());
                        events.effects.insert(events.effects.end(),surroundings.effects.begin(),surroundings.effects.end());
                        if(race.ice())for(const auto sounds:{race.ice()->effects(),race.snowball_effects()})
                            events.effects.insert(events.effects.end(),sounds.begin(),sounds.end());
                        const auto bonus_sounds=race.bonus_effects();
                        events.effects.insert(events.effects.end(),bonus_sounds.begin(),bonus_sounds.end());
                        const auto prop_sounds=race.prop_effects();
                        events.effects.insert(events.effects.end(),prop_sounds.begin(),prop_sounds.end());
                        const auto scenery_sounds=scenery.effects();
                        events.effects.insert(events.effects.end(),scenery_sounds.begin(),scenery_sounds.end());
                    }
                    audio->update(events,throttle,(buttons&1024)!=0,true,true);
                }
                ++tick;sent=false;pending.clear();last_response=last_step=Clock::now();
                next=std::max(next+std::chrono::microseconds(71429),last_step);
                if(!host && !capture_handoff && tick==countdown+50){draw(1);renderer.screenshot(capture);}
                if(captured && slot==1 && (sky_test || harbor_test)){
                    const auto sky=environment.presentation();
                    const auto lap=race.car(1).body.word(0x7a)+1;
                    const auto compare=[&](std::string name,unsigned threshold,auto control){
                        const unsigned pixels=capture_difference(name,threshold,control);
                        if(pixels)std::cerr<<(sky_test?"SKY ":"HARBOR ")<<name<<' '<<tick<<' '<<lap<<' '<<pixels<<std::endl;
                        return pixels!=0;
                    };
                    if(sky_test){
                        if(!cloud_stage && !sky.fog_weight)cloud_stage=1;
                        if((cloud_stage==1 && sky.fog_weight==15) || (cloud_stage==2 && !sky.fog_weight)){
                            auto alt=sky;alt.fog_weight=cloud_stage==1?0:15;
                            if(compare(cloud_stage==1?"cloud-entry":"cloud-exit",1800,[&]{draw_scene(1,scenery.poses(),alt);}))++cloud_stage;
                        }
                        const auto actors=scenery.poses();
                        if(!formation && lap==3 && std::ranges::count_if(actors,[](const auto& p){return p.visible && p.visual_descriptor==0xaa34;})>=2){
                            std::vector<SceneryPose> empty(actors.begin(),actors.end());
                            for(auto& p:empty)if(p.visual_descriptor==0xaa34)p.visible=false;
                            // Look up from the bonnet to frame the overhead pass in the guest capture.
                            const auto camera=race_camera;
                            race_camera.view=RaceCamera::bonnet;
                            race_camera.tilt=.42f;
                            const auto pixels=capture_difference("formation",500,[&]{draw_scene(1,empty,sky);},true);
                            race_camera=camera;
                            if(pixels){
                                formation=true;
                                std::cerr<<"SKY formation "<<tick<<' '<<lap<<' '<<pixels<<std::endl;
                            }
                        }
                        if(!smoke && !race.spray_sprites().empty())
                            smoke=compare("smoke",60,[&]{draw_course(1,actors,sky,{},{},{},{},race.bonus_poses());});
                    }else{
                        const auto props=race.prop_poses();
                        const auto spray=race.spray_sprites();
                        if(!water_captured && !spray.empty())
                            water_captured=compare("water",300,[&]{draw_course(1,scenery.poses(),sky,{},props,{});});
                        for(std::size_t i=0;i<props.size();++i){
                            const auto& p=props[i];const auto& car=race.car(1).body;
                            if(folded_signs[i] || !p.frame || p.visual_descriptor!=0xaf58 || tick%4 ||
                               std::abs(p.position.x-std::int16_t(car.word(0x1e)))>8500 || std::abs(p.position.z-std::int16_t(car.word(0x22)))>8500)continue;
                            std::vector<SceneryPose> upright(props.begin(),props.end());upright[i].frame=0;
                            folded_signs[i]=compare("sign-"+std::to_string(p.command_offset),150,[&]{draw_course(1,scenery.poses(),sky,{},upright,spray);});
                        }
                        if((!dawn_stage && sky.palette_weight==0) || (dawn_stage==1 && sky.palette_weight==31)){
                            auto alt=sky;alt.palette_weight=dawn_stage?0:31;
                            if(compare(dawn_stage?"dawn":"night",4000,[&]{draw_scene(1,scenery.poses(),alt);}))++dawn_stage;
                        }
                    }
                }
                if(ravine_test && slot==1 && !ravine_flash && !capture_handoff && environment.presentation().additive_fixed_color){
                    auto clear=environment.presentation();
                    clear.fixed_rgb555=0;clear.additive_fixed_color=false;clear.color_math_layers=0;
                    const auto pixels=capture_difference("lightning",10000,[&]{draw_scene(1,scenery.poses(),clear);});
                    if(pixels){ravine_flash=true;std::cerr<<"RAVINE lightning "<<tick<<' '<<pixels<<std::endl;}
                }
                if(ravine_test && captured){
                    const auto decks=race.bridge_poses();
                    for(std::size_t i=0;i<decks.size();++i)ravine_deck_frames.at(i)|=1u<<decks[i].frame;
                    const auto rocks=race.snowballs();
                    const unsigned lap=1+std::max(race.car(0).body.word(0x7a),race.car(1).body.word(0x7a));
                    for(auto sound:race.snowball_effects()){
                        if(sound!=0x53 && sound!=0x5a)continue;
                        std::cerr<<"RAVINE "<<(sound==0x53?"fall":"impact")<<' '<<tick<<' '<<lap;
                        for(const auto& p:rocks)std::cerr<<' '<<p.visual_descriptor<<' '<<p.position.x<<' '<<p.position.y<<' '<<p.position.z;
                        std::cerr<<std::endl;
                    }
                    if(slot==1){
                        const auto sky=environment.presentation();
                        if(!ravine_bridge && tick%4==0 && std::ranges::any_of(decks,[&](const auto& p){
                            const auto& car=race.car(1).body;
                            return p.frame!=2 && std::abs(p.position.x-std::int16_t(car.word(0x1e)))<6500 &&
                                std::abs(p.position.z-std::int16_t(car.word(0x22)))<6500;})){
                            std::vector<SceneryPose> neutral(decks.begin(),decks.end());
                            for(auto& p:neutral)p.frame=2;
                            const auto count=capture_difference("spans",400,[&]{draw_course(1,scenery.poses(),sky,neutral,{},{},rocks);});
                            if(count){ravine_bridge=true;std::cerr<<"RAVINE spans "<<tick<<' '<<count<<std::endl;}
                        }
                        if(lap<=3 && !ravine_rock_capture[lap] && std::ranges::any_of(rocks,[](const auto& p){return p.visual_descriptor==0xb4d0;})){
                            std::vector<SceneryPose> control(rocks.begin(),rocks.end());
                            std::erase_if(control,[](const auto& p){return p.visual_descriptor==0xb4d0;});
                            const auto count=capture_difference("rocks-"+std::to_string(lap),180,[&]{
                                draw_course(1,scenery.poses(),sky,decks,{},{},control);});
                            if(count){ravine_rock_capture[lap]=true;std::cerr<<"RAVINE rocks "<<tick<<' '<<lap<<' '<<count<<std::endl;}
                        }
                    }
                }
                if(!host && captured && slot==1 && course==CourseId::sea_breeze && tick%3==0){
                    const auto& car=race.car(slot);
                    const auto props=race.prop_poses();
                    const auto water=race.spray_sprites();
                    const auto compare=[&](const std::string& label,unsigned command,unsigned minimum,
                                           std::span<const SceneryPose> visible,std::span<const ScenerySprite> particles){
                        const unsigned changed=capture_difference(label,minimum,[&]{
                            draw_course(1,scenery.poses(),environment.presentation(),race.bridge_poses(),visible,particles);
                        });
                        if(!changed)return false;
                        std::cerr<<"SEA "<<label<<' '<<tick<<' '<<1+car.body.word(0x7a)<<' '<<command<<' '<<changed<<std::endl;
                        return true;
                    };
                    if(!water_captured && !water.empty() && std::ranges::any_of(car.wheels,[](const auto& w){return (w.word(0x46)&0x211)==0x11 && (w.word(0x2c)&0x80);}))
                        water_captured=compare("water",0,350,props,{});
                    for(std::size_t i=0;i<props.size();++i){
                        const auto& p=props[i];
                        if(barrier_captured.at(i) || std::abs(p.position.x-std::int16_t(car.body.word(0x1e)))>8500 ||
                           std::abs(p.position.z-std::int16_t(car.body.word(0x22)))>8500)continue;
                        std::vector<SceneryPose> visible(props.begin(),props.end());
                        visible[i].visible=false;
                        barrier_captured[i]=compare("barrier-"+std::to_string(i+1),p.command_offset,200,visible,water);
                    }
                }
                if(lake_test && captured){
                    const auto& car=race.car(1);
                    const auto& deck=race.bridge_poses().front();
                    const auto lap=1+car.body.word(0x7a);
                    // Probe the real wheel candidate at the position used to start this phase.
                    for(unsigned w=0;!lake_contact && w<car.wheels.size();++w){
                        const auto* shape=race.wheel_surface(1,w);
                        if(!shape || shape->command_offset!=deck.command_offset || shape->frame==15)continue;
                        const auto& wheel=previous[1].wheels[w];
                        const auto groups=query_collision_shape(*shape,{std::int16_t(wheel.word(0x1e)),
                            std::int16_t(wheel.word(0x20)),std::int16_t(wheel.word(0x22))});
                        for(const auto& group:groups){
                            if(!group.flags || (group.flags&0xff00) || !group.normal[1])continue;
                            std::cerr<<"LAKE contact "<<tick<<' '<<deck.command_offset<<' '<<deck.frame<<' '<<int(shape->frame)
                                     <<' '<<w<<' '<<group.distance<<' '<<group.normal[1]<<std::endl;
                            lake_contact=true;break;
                        }
                    }
                    if(slot==1){
                        const auto sky=environment.presentation();
                        if(!lake_deck && deck.frame!=15 && tick%3==0 &&
                           std::abs(deck.position.z-std::int16_t(car.body.word(0x22)))<9500){
                            auto flat=deck;flat.frame=15;
                            const auto count=capture_difference("bridge",350,[&]{
                                draw_course(1,scenery.poses(),sky,std::span(&flat,1),{},{});
                            });
                            if(count){lake_deck=true;std::cerr<<"LAKE bridge "<<tick<<' '<<deck.frame<<' '<<count<<std::endl;}
                        }
                        if(!lake_creature && lap>=2){
                            const auto poses=scenery.poses();
                            auto it=std::ranges::find(poses,0xb754,&SceneryPose::visual_descriptor);
                            if(it!=poses.end() && it->visible){
                                std::vector<SceneryPose> hidden(poses.begin(),poses.end());
                                hidden[it-poses.begin()].visible=false;
                                const auto count=capture_difference("creature",160,[&]{draw_scene(1,hidden,sky);});
                                if(count){lake_creature=true;std::cerr<<"LAKE creature "<<tick<<' '<<lap<<' '<<count<<std::endl;}
                            }
                        }
                        if(!lake_water_stage && !sky.fog_weight)++lake_water_stage;
                        if((lake_water_stage==1 && sky.fog_weight==6) ||
                           (lake_water_stage==2 && !sky.fog_weight)){
                            const bool entry=lake_water_stage==1;
                            auto control=sky;control.fog_weight=entry?0:6;control.fog_rgb555=entry?0xffff:0x0800;
                            const auto label=entry?"entry":"exit";
                            const auto count=capture_difference(label,1000,[&]{draw_scene(1,scenery.poses(),control);});
                            if(count){++lake_water_stage;std::cerr<<"LAKE "<<label<<' '<<tick<<' '<<int(sky.fog_weight)<<' '<<count<<std::endl;}
                        }
                    }
                }
                if(cruise_test && captured && slot==1){
                    const auto sky=environment.presentation();
                    const auto poses=race.bonus_poses();
                    const auto capture_actor=[&](unsigned descriptor,const char* label,unsigned minimum){
                        auto found=std::ranges::find(poses,descriptor,&SceneryPose::visual_descriptor);
                        if(found==poses.end() || !found->visible)return false;
                        std::vector<SceneryPose> control(poses.begin(),poses.end());
                        control[found-poses.begin()].visible=false;
                        const auto pixels=capture_difference(label,minimum,[&]{
                            draw_course(1,scenery.poses(),sky,{},race.prop_poses(),{},{},control);
                        },true);
                        if(pixels)std::cerr<<"CRUISE "<<label<<' '<<tick<<' '<<pixels<<std::endl;
                        return pixels>0;
                    };
                    if(!aircraft_captured)aircraft_captured=capture_actor(0xaa34,"aircraft",128);
                    if(!boost_captured && !race.boost_owner())boost_captured=capture_actor(0xb7a8,"boost",128);
                    const unsigned weight=sky.palette_weight;
                    const auto& driver=race.car(slot).body;
                    const auto route_node=driver.word(0x58),lap=driver.word(0x7a);
                    // Compare dusk and full night on successive passes through the same city block.
                    if(!skyline_samples || (skyline_samples==1 && weight>=(first_sky_weight+31)/2) ||
                       (skyline_samples==2 && weight==31 && route_node==dusk_route && lap>dusk_lap)){
                        if(!skyline_samples)first_sky_weight=weight;
                        if(skyline_samples==1){dusk_route=route_node;dusk_lap=lap;}
                        const auto label="sky-"+std::to_string(++skyline_samples);
                        draw(1);renderer.screenshot(capture_path(label));
                        std::cerr<<"CRUISE sky "<<tick<<' '<<weight<<std::endl;
                    }
                }
                if(!host && course==CourseId::white_land && capture_handoff){
                    const auto& ice=*race.ice();
                    if(slot==0 && !ice.effects().empty()){
                        const auto& car=previous[0].body;
                        for(const auto& prop:race.snowmen()){
                            bool overlap=true;
                            for(unsigned axis=0;axis<3;++axis){
                                const auto at=0x1e + 2*axis,size=0x0c + 2*axis;
                                overlap&=std::abs(std::int16_t(car.word(at))-std::int16_t(prop.state.word(at)))<=car.word(size)+prop.state.word(size);
                            }
                            if(overlap && std::ranges::any_of(ice.sprites(),[&](const auto& s){return s.table==0x996b && s.command_offset==prop.command_offset;}))
                                std::cerr<<"WHITE impact "<<tick<<' '<<prop.command_offset<<' '<<1+car.word(0x7a)<<std::endl;
                        }
                    }
                    if(captured && slot==1){
                        const auto& car=race.car(slot).body;
                        const auto& first=race.snowmen().front();
                        const auto intact=ice.intact_sprite(first);
                        if(!snowman_captured && ice.solid_retired(first.command_offset) && tick%3==0 &&
                           std::hypot(intact.position.x-std::int16_t(car.word(0x1e)),intact.position.z-std::int16_t(car.word(0x22)))<6500){
                            std::vector<ScenerySprite> restored(ice.sprites().begin(),ice.sprites().end());
                            restored.push_back(intact);
                            if(capture_difference("snowman",160,[&]{
                                draw_course(1,scenery.poses(),environment.presentation(),{},{},restored,race.snowballs());
                            })){
                                std::cerr<<"WHITE snowman "<<tick<<' '<<first.command_offset<<std::endl;
                                snowman_captured=true;
                            }
                        }
                        const auto balls=race.snowballs();
                        if(balls.size()==2 && car.word(0x7a)==2 && snowball_samples<2 && tick>=snowball_tick+3){
                            // Keep each ball visible independently in the same frame.
                            draw(1);
                            auto live=renderer.readback_bgra();
                            std::array<std::vector<BYTE>,2> controls_without;
                            std::array<unsigned,2> pixels{};
                            for(unsigned i=0;i<2;++i){
                                draw_course(1,scenery.poses(),environment.presentation(),{},{},ice.sprites(),balls.subspan(1-i,1));
                                auto& ref=controls_without[i];ref=renderer.readback_bgra();
                                pixels[i]=changed_pixels(live,ref);
                            }
                            if(std::min(pixels[0],pixels[1])>=24){
                                const auto label="snowballs-"+std::to_string(++snowball_samples);
                                Renderer::write_png(capture_path(label),live,controls.width,controls.height);
                                for(unsigned i=0;i<2;++i)Renderer::write_png(capture_path(label+"-without-"+std::to_string(i+1)),controls_without[i],controls.width,controls.height);
                                std::cerr<<"WHITE balls "<<tick<<" 3 "<<balls[0].position.z<<' '<<balls[1].position.z<<' '<<pixels[0]<<' '<<pixels[1]<<std::endl;
                                snowball_tick=tick;
                            }
                        }
                    }
                }
                if(!host && captured && slot==1 && (course==CourseId::kings_forest || course==CourseId::white_land)){
                    const auto sky=environment.presentation();
                    const auto weights=course==CourseId::white_land?std::array{10,5,0}:std::array{15,7,0};
                    if(forest_fog_stage<weights.size() && sky.fog_weight==weights[forest_fog_stage]){
                        const auto label="fog-"+std::to_string(sky.fog_weight);
                        draw(1);renderer.screenshot(capture_path(label));
                        auto control=sky;
                        control.fog_weight=sky.fog_weight?0:15;
                        draw_scene(1,scenery.poses(),control);renderer.screenshot(capture_path(label+"-control"));
                        std::cerr<<(course==CourseId::white_land?"WHITE fog ":"FOREST fog ")<<tick<<' '<<int(sky.fog_weight)<<std::endl;
                        ++forest_fog_stage;
                    }
                    const auto decks=race.bridge_poses();
                    const auto& car=race.car(slot).body;
                    for(std::size_t i=0;i<decks.size();++i){
                        auto& stage=forest_deck_stage.at(i);
                        const auto& deck=decks[i];
                        if(stage>=2 || deck.frame!=(stage==0?2:4))continue;
                        if(std::abs(deck.position.x-std::int16_t(car.word(0x1e)))>11000 ||
                           std::abs(deck.position.z-std::int16_t(car.word(0x22)))>11000)continue;
                        // Hold both cars, camera and fog fixed. Change only this deck's visible pose.
                        std::vector<SceneryPose> flat(decks.begin(),decks.end());
                        flat[i].frame=0;
                        draw_course(1,scenery.poses(),sky,flat,race.prop_poses(),race.spray_sprites());
                        auto reference=renderer.readback_bgra();
                        draw(1);
                        auto live=renderer.readback_bgra();
                        unsigned pixels=0;
                        for(std::size_t p=0;p<live.size();p+=4)
                            if(!std::equal(live.begin()+p,live.begin()+p+3,reference.begin()+p))++pixels;
                        if(pixels<2500)continue;
                        const auto label="bridge-"+std::to_string(i+1)+"-"+std::to_string(deck.frame);
                        Renderer::write_png(capture_path(label),live,controls.width,controls.height);
                        Renderer::write_png(capture_path(label+"-flat"),reference,controls.width,controls.height);
                        std::cerr<<"FOREST bridge "<<tick<<' '<<deck.command_offset<<' '<<deck.frame<<' '<<pixels<<std::endl;
                        ++stage;
                    }
                }
                if(!host && captured && slot==1 && course==CourseId::aqua_tunnel && aqua_capture_stage<3){
                    const auto view=environment.presentation();
                    // Wait for the portal fade to finish before sampling the resulting daylight or water.
                    if(!view.blank_backdrop && !(environment.state().flags&6) && view.alternate_backdrop==(aqua_capture_stage==1)){
                        if(aqua_capture_stage>0){
                            const auto label=aqua_capture_stage==1?"entry":"exit";
                            draw(1);
                            renderer.screenshot(capture_path(label));
                            std::cerr<<"AQUA "<<label<<' '<<tick<<' '<<race.car(slot).body.word(0x7a)<<std::endl;
                        }
                        ++aqua_capture_stage;
                    }
                }
                if(!host && captured && slot==1 && course==CourseId::sunset_valley){
                    const auto weight=environment.presentation().palette_weight;
                    const auto lap=race.car(slot).body.word(0x7a);
                    constexpr std::array weights{0,16,31};
                    constexpr std::array labels{"day","transition","dusk"};
                    if(sunset_capture_stage<weights.size() && weight==weights[sunset_capture_stage]){
                        const auto label=labels[sunset_capture_stage++];
                        draw(1);renderer.screenshot(capture_path(label));
                        if(weight!=0){
                            draw_scene(1,scenery.poses(),{});
                            renderer.screenshot(capture_path(std::string(label)+"-daylight"));
                        }
                        std::cerr<<"SUNSET "<<label<<' '<<tick<<' '<<lap<<' '<<int(weight)<<std::endl;
                    }
                }
                if(!host && captured && slot==1){
                    const auto lap=race.car(slot).body.word(0x7a);
                    const bool crossing=course==CourseId::sunset_valley && lap==2;
                    const bool flyover=course==CourseId::night_owl && lap==1;
                    const auto event_actor=[&](const SceneryPose& p){
                        return p.visible && (flyover ? p.visual_descriptor==0xb770 :
                            (p.visual_descriptor==0xb2d8 || p.visual_descriptor==0xb2f4));
                    };
                    if((crossing || flyover) && std::ranges::any_of(scenery.poses(),event_actor)){
                        // Isolate pixels contributed by the moving actors in the actual guest view.
                        const auto poses=scenery.poses();
                        std::vector<SceneryPose> without_event(poses.begin(),poses.end());
                        std::erase_if(without_event,event_actor);
                        draw_scene(1,without_event,environment.presentation());
                        auto empty=renderer.readback_bgra();
                        draw(1);
                        auto frame=renderer.readback_bgra();
                        unsigned pixels=0;
                        bool clipped=false;
                        for(std::size_t p=0;p<frame.size();p+=4){
                            if(std::equal(frame.begin()+p,frame.begin()+p+3,empty.begin()+p))continue;
                            ++pixels;
                            if(flyover){
                                const auto x=(p/4)%controls.width,y=(p/4)/controls.width;
                                clipped|=x==0 || y==0 || x+1==controls.width || y+1==controls.height;
                            }
                        }
                        if(!clipped && pixels>event_pixels){
                            event_pixels=pixels;
                            event_tick=tick;
                            event_lap=lap;
                            event_frame=std::move(frame);
                            event_control=std::move(empty);
                        }
                    }
                }
                if(!host && tick>12000)throw std::runtime_error("Offscreen drivers exceeded the lap budget");
            }
        }
        if(host){
            if(keys.key('H') && !last_h)race_hud_visible=!race_hud_visible;
            last_h=keys.key('H');
            RECT r{};GetClientRect(host->window,&r);
            if(r.right && r.bottom){
                draw(std::clamp(float(std::chrono::duration<double>(Clock::now()-last_step).count()*14),0.f,1.f));
            }else Sleep(4);
        }else if(!available)Sleep(1);
    }
    if(event_pixels){
        const bool flyover=course==CourseId::night_owl;
        Renderer::write_png(capture_path(flyover?"flyover":"crossing"),event_frame,controls.width,controls.height);
        Renderer::write_png(capture_path(flyover?"without-aircraft":"without-herd"),event_control,controls.width,controls.height);
        std::cerr<<(flyover?"NIGHT flyover ":"SUNSET crossing ")<<event_tick<<' '<<event_lap<<' '<<event_pixels<<std::endl;
    }
    if(cruise_test)bonus_status("finish");
    if(ravine_test && captured){
        std::cerr<<"RAVINE decks";
        for(auto mask:ravine_deck_frames)std::cerr<<' '<<mask;
        std::cerr<<std::endl;
    }
    std::cout<<"FINISH "<<tick<<' '<<*race.result();
    for(unsigned i=0;i<grid.size();++i)std::cout<<' '<<race.car(i).body.word(0x7a);
    std::cout<<std::endl;
    return 0;
}
}
