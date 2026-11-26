// Render the production headset shader and bonnet camera without a window or XR runtime.
#define wmain preview_application_entry
#include "../src/preview.cpp"
#undef wmain

namespace {
constexpr unsigned width=512,height=512;
using Stereo=std::array<std::vector<BYTE>,2>;

Stereo render_eyes(Renderer& renderer,const SceneTarget& target) {
    Stereo images;
    for(unsigned i=0;i<2;++i) {
        srf::vr::Eye eye{{i?.032f:-.032f,0,0},{0,0,0,1},i?-.84f:-1.38f,i?1.38f:.84f,-1.43f,.97f};
        renderer.render_eye(target,bonnet_eye(renderer,eye));
        images[i]=renderer.readback_bgra(target);
    }
    if(images[0]==images[1])throw std::runtime_error("Identical stereo images");
    return images;
}

void save(Stereo& images,const std::filesystem::path& output,const std::string& name,std::string tag,unsigned phase) {
    if(!output.empty())for(unsigned i=0;i<2;++i)
        Renderer::write_png(output/(name+"-"+tag+(i?"-right.png":"-left.png")),images[i],width,height);
    std::cout<<name<<" "<<tag<<" phase "<<phase<<'\n';
}

void check_sunset(Renderer& renderer,const SceneTarget& target,const srf::World& world,const srf::Rom& rom,srf::PlayerCar car,
               std::string name,std::string fixture,const std::filesystem::path& output) {
    constexpr auto course=srf::CourseId::sunset_valley;
    srf::NativeRace race(rom,car,course);
    srf::CourseScenery scenery(rom,course),empty(rom,course);
    srf::CourseEnvironment environment(rom,course);
    srf::RaceAudioState audio;
    const auto inputs=read_inputs(std::filesystem::path(SRF_TEST_FIXTURES)/("sunset_valley_"+fixture+"_inputs.csv"));
    const auto render=[&](const srf::CourseScenery& actors) {
        load_bonnet_pose(renderer,world,rom,race,actors,race.racers());
        return render_eyes(renderer,target);
    };
    bool day=false,dusk=false;
    Stereo crossing;
    unsigned crossing_phase=0;
    std::array<unsigned,2> best_pixels{};
    for(const auto input:inputs) {
        const auto previous=race.racers()[race.player_index()].body;
        const auto lap=race.completion().acknowledged_laps;
        srf::RaceAudioOutput events;
        race.step(srf::prepare_player_input(audio,previous,input.buttons,race.countdown_active(),events),input.ticks);
        environment.advance_video(input.ticks);
        environment.step(previous,lap,race.elapsed_centiseconds());
        scenery.step(previous,lap);
        const auto state=environment.presentation();
        renderer.set_environment(state);
        if(!day && race.phase()==100 && state.palette_weight==0) {
            auto images=render(scenery);save(images,output,name,"daylight",race.phase());day=true;
        }
        if(!dusk && state.palette_weight==31) {
            auto images=render(scenery);
            // Same viewpoint and actors with daylight as the control for each eye.
            renderer.set_environment({});
            const auto daylight=render(scenery);
            for(unsigned i=0;i<2;++i) {
                unsigned darkened=0;
                for(size_t p=0;p<width*height*2;p+=4)
                    if(daylight[i][p]>images[i][p]+20)++darkened;
                if(darkened<width*height/50)throw std::runtime_error("Missing dusk palette for "+name);
            }
            renderer.set_environment(state);
            save(images,output,name,"dusk",race.phase());dusk=true;
        }
        if(day && dusk && lap==2 && std::ranges::any_of(scenery.poses(),&srf::SceneryPose::visible)) {
            auto images=render(scenery);
            const auto clear=render(empty);
            const auto animal_pixels=[&](unsigned i) {
                unsigned pixels=0;
                for(size_t p=0;p<images[i].size();p+=4)
                    if(!std::equal(images[i].begin()+p,images[i].begin()+p+3,clear[i].begin()+p))++pixels;
                return pixels;
            };
            // Keep the clearest paired frame as the herd moves across the road.
            const std::array pixels{animal_pixels(0),animal_pixels(1)};
            if(std::ranges::min(pixels)>std::ranges::min(best_pixels)) {
                best_pixels=pixels;crossing_phase=race.phase();crossing=std::move(images);
            }
        }else if(crossing_phase)break;
    }
    if(std::ranges::min(best_pixels)>=32) {
        std::cout<<"Animal pixels left/right "<<best_pixels[0]<<'/'<<best_pixels[1]<<'\n';
        save(crossing,output,name,"crossing",crossing_phase);return;
    }
    throw std::runtime_error("Missing stereo daylight, dusk or crossing for "+name);
}
void check_night_owl(Renderer& renderer,const SceneTarget& target,const srf::World& world,const srf::Rom& rom,
                     srf::PlayerCar car,std::string name,std::string fixture,const std::filesystem::path& output) {
    constexpr auto course=srf::CourseId::night_owl;
    srf::NativeRace race(rom,car,course);
    srf::CourseScenery scenery(rom,course);
    srf::CourseEnvironment environment(rom,course);
    srf::RaceAudioState audio;
    const auto tape=read_inputs(std::filesystem::path(SRF_TEST_FIXTURES)/("night_owl_"+fixture+"_inputs.csv"));
    // First clear view of a rival and the middle of the first flyover on each recorded drive.
    const unsigned traffic_phase=car==srf::PlayerCar::four_wd?120:car==srf::PlayerCar::coupe?400:395;
    const unsigned flyover_phase=car==srf::PlayerCar::four_wd?418:car==srf::PlayerCar::coupe?349:344;
    for(const auto input:tape) {
        const auto previous=race.racers()[race.player_index()].body;
        const auto lap=race.completion().acknowledged_laps;
        srf::RaceAudioOutput events;
        race.step(srf::prepare_player_input(audio,previous,input.buttons,race.countdown_active(),events),input.ticks);
        environment.advance_video(input.ticks);
        environment.step(previous,lap,race.elapsed_centiseconds());
        scenery.step(previous,lap);
        if(lap)break;
        const auto phase=race.phase();
        const bool grid=phase==20;
        const bool flyover=phase==flyover_phase;
        if(!grid && !flyover && phase!=traffic_phase)continue;
        renderer.set_environment(environment.presentation());
        const auto& cars=race.racers();
        load_bonnet_pose(renderer,world,rom,race,scenery,cars);
        auto images=render_eyes(renderer,target);
        if(grid) {
            for(const auto& eye:images) {
                unsigned dark_sky=0,asphalt=0;
                for(unsigned x=100;x<400;++x) {
                    for(unsigned y=40;y<100;++y) {
                        const auto* pixel=&eye[4*(y*width+x)];
                        if(std::max({pixel[0],pixel[1],pixel[2]})<20)++dark_sky;
                    }
                    // The road just below the horizon, above the painted grid and bonnet.
                    for(unsigned y=235;y<245;++y) {
                        const auto* pixel=&eye[4*(y*width+x)];
                        if(pixel[1]>=40 && pixel[1]<120 && std::abs(int(pixel[1])-pixel[0])<20)++asphalt;
                    }
                }
                if(dark_sky<9000 || asphalt<1200)throw std::runtime_error("Night sky or visible asphalt missing for "+name);
            }
            save(images,output,name,"grid",phase);
            continue;
        }
        auto vehicles=vehicle_poses(rom,cars,cars,race.player_index(),1,false);
        if(!flyover) {
            renderer.load_race_world(world,std::span(&vehicles[race.player_index()],1),rom,race,scenery);
        }else{
            const auto actors=scenery.poses();
            std::vector<srf::SceneryPose> without_plane(actors.begin(),actors.end());
            auto plane=std::ranges::find(without_plane,0xb770,&srf::SceneryPose::visual_descriptor);
            if(plane==without_plane.end() || !plane->visible)throw std::runtime_error("Missing plane on "+name+" replay");
            plane->visible=false;
            renderer.load_world(world,0,vehicles,&rom,without_plane,race.rock_poses(),race.cone_poses(),
                                race.bridge_poses(),scenery.sprites(),&race);
        }
        const auto control=render_eyes(renderer,target);
        std::array<unsigned,2> contrast{};
        for(unsigned eye=0;eye<2;++eye) {
            const auto& frame=images[eye];
            for(size_t p=0;p<frame.size();p+=4) {
                for(unsigned channel=0;channel<3;++channel) {
                    if(std::abs(int(frame[p+channel])-control[eye][p+channel])<32)continue;
                    ++contrast[eye];break;
                }
            }
        }
        if(std::ranges::min(contrast)<300)throw std::runtime_error("Insufficient stereo contrast for "+name+(flyover?" flyover":" rivals"));
        save(images,output,name,flyover?"flyover":"traffic",phase);
        std::cout<<"Contrasting pixels left/right "<<contrast[0]<<'/'<<contrast[1]<<'\n';
        if(phase==std::max(traffic_phase,flyover_phase))return;
    }
    throw std::runtime_error("Missing Night Owl traffic or flyover for "+name);
}

// Shift the look-at point 60 m along the bonnet camera axis but leave the
// physical eyes fixed. A camera-relative lighting grid changes the image.
void check_shadow_distance(Renderer& renderer,const SceneTarget& target,const std::filesystem::path& output,
                           const std::string& course) {
    auto before=render_eyes(renderer,target);
    const auto camera=controls;
    const float shift=renderer.normalized_distance(60*90);
    controls.target_x-=std::sin(controls.yaw)*shift;
    controls.target_z+=std::cos(controls.yaw)*shift;
    controls.distance+=shift;
    auto after=render_eyes(renderer,target);
    controls=camera;
    for(unsigned eye=0;eye<2;++eye) {
        if(!output.empty())Renderer::write_png(output/(course+(eye?"-right.png":"-left.png")),before[eye],width,height);
        // Compare regional brightness. Equivalent float transforms can still
        // rasterize a distant edge on opposite sides of a pixel boundary.
        constexpr unsigned patch=64;
        int largest_shift=0;
        for(unsigned y=0;y<height;y+=patch)for(unsigned x=0;x<width;x+=patch) {
            std::array<int,3> delta{};
            for(unsigned dy=0;dy<patch;++dy)for(unsigned dx=0;dx<patch;++dx) {
                const auto p=4*((y+dy)*width+x+dx);
                for(unsigned c=0;c<3;++c)delta[c]+=int(before[eye][p+c])-after[eye][p+c];
            }
            for(int sum:delta)largest_shift=std::max(largest_shift,std::abs(sum));
        }
        const double levels=double(largest_shift)/(patch*patch);
        std::cout<<course<<" eye "<<eye<<": peak patch change "<<levels<<'\n';
        if(levels>2) {
            if(!output.empty())Renderer::write_png(output/(course+"-shifted.png"),after[eye],width,height);
            throw std::runtime_error(course+" lighting follows the camera target");
        }
    }
}
void check_course_lighting(Renderer& renderer,const SceneTarget& target,const srf::Rom& rom,
                           const std::filesystem::path& output) {
    using namespace srf;
    for(const auto& course:courses) {
        renderer.select_course(course.id);
        renderer.set_course_colors(generated::course_packages[unsigned(course.id)]);
        renderer.load_textures(extract_vehicle_sprites(rom,course.id),extract_vehicle_sprites(rom,course.id,true));
        renderer.load_backdrop(extract_course_backdrop(rom,course.id));
        NativeRace race(rom,PlayerCar::four_wd,course.id);
        CourseScenery scenery(rom,course.id);
        const auto world=extract_world(rom,extract_course_scene(rom,course.id));
        renderer.load_world(world,0);
        std::string fixture(course.slug);
        std::replace(fixture.begin(),fixture.end(),'-','_');
        auto inputs=read_inputs(std::filesystem::path(SRF_TEST_FIXTURES)/(fixture+"_clean_inputs.csv"));
        RaceAudioState audio;
        for(unsigned i=0;i<320;++i) {
            const auto previous=race.racers()[race.player_index()].body;
            RaceAudioOutput events;
            race.step(prepare_player_input(audio,previous,inputs.at(i).buttons,race.countdown_active(),events),inputs[i].ticks);
            scenery.step(previous,0);
        }
        for(unsigned dusk:{0u,31u}) {
            EnvironmentPresentation state{};
            state.palette_weight=dusk;
            renderer.set_environment(state);
            load_bonnet_pose(renderer,world,rom,race,scenery,race.racers());
            check_shadow_distance(renderer,target,output,std::string(course.slug)+(dusk?"-dusk":""));
            if(course.id!=CourseId::sunset_valley && course.id!=CourseId::night_cruise && course.id!=CourseId::harbor_city)break;
        }
    }
    const auto check_grid=[&](const Scene& scene,const Vehicle& car,const std::string& name) {
        const auto world=extract_world(rom,scene);
        renderer.set_environment({});
        renderer.load_world(world,0);
        const auto pose=vehicle_pose(rom,car,car,true,1,static_cast<std::uint8_t>(car.body.word(0x24)>>8));
        bonnet_camera(renderer,pose,pose.heading);
        renderer.load_world(world,0,std::span(&pose,1),&rom);
        check_shadow_distance(renderer,target,output,name);
    };
    for(auto course:playable_stunt_courses) {
        renderer.select_course(std::nullopt,course);
        generated::CoursePackage colors{};
        colors.day_palette=colors.dusk_palette=extract_rock_field_palette(rom,course);
        renderer.set_course_colors(colors);
        renderer.load_textures(extract_rock_field_sprites(rom,course));
        renderer.load_backdrop(extract_rock_field_backdrop(rom,course));
        RockFieldSession session(rom,PlayerCar::four_wd,course);
        check_grid(session.drive.scene(),session.drive.player(),std::string(stunt_course_id(course)));
    }
    const auto check_local=[&](LocalCourse course) {
        renderer.select_course(std::nullopt);
        generated::CoursePackage colors{};
        colors.day_palette=colors.dusk_palette=extract_local_palette(rom,course.index);
        renderer.set_course_colors(colors);
        renderer.load_textures(extract_local_sprites(rom,course.index));
        renderer.load_backdrop(extract_local_backdrop(rom,course.index));
        LocalRace race(rom,course,PlayerCar::four_wd,PlayerCar::coupe,true);
        check_grid(race.scene(),race.car(0),std::string(course.name.begin(),course.name.end()));
    };
    for(auto course:battle_courses)check_local(course);
    check_local(practice_course);
    for(auto league:{SpeedClass::novice,SpeedClass::expert,SpeedClass::master}) {
        renderer.select_course(std::nullopt);
        renderer.set_course_colors(generated::course_packages.front());
        renderer.load_textures(extract_vehicle_sprites(rom,CourseId::easy_ride,false,true));
        renderer.load_backdrop(extract_speed_bonus_backdrop(rom));
        SpeedBonus bonus(rom,league);
        check_grid(bonus.scene(),bonus.truck(),"bonus-"+std::to_string(unsigned(league)));
    }
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        const bool night=argc>1 && std::wstring_view(argv[1])==L"--night-owl";
        const bool shadows=argc>1 && std::wstring_view(argv[1])==L"--remastered-shadows";
        if(night || shadows){--argc;++argv;}
        const auto* path=argc>1?argv[1]:_wgetenv(L"SRF_ROM");if(!path)return 77;
        const auto rom=srf::Rom::load(path);
        const std::filesystem::path output=argc>2?argv[2]:L"";
        if(!output.empty())std::filesystem::create_directories(output);
        check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
        ComPtr<ID3D11Device> device;
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,
                                &device,nullptr,nullptr),"Offscreen headset device");
        const auto course=night?srf::CourseId::night_owl:srf::CourseId::sunset_valley;
        controls.world=controls.race=true;
        RaceGraphics graphics;
        if(shadows){graphics.look=RaceLook::remastered;graphics.hour=16;graphics.ray_shadows=graphics.indirect_light=true;}
        Renderer renderer(nullptr,device.Get(),0,graphics,course);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width=width;desc.Height=height;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc,nullptr,&texture),"Offscreen sRGB eye");
        const auto target=renderer.make_target(width,height,texture,desc.Format);
        if(shadows){check_course_lighting(renderer,target,rom,output);return 0;}
        renderer.set_course_colors(srf::generated::course_packages[unsigned(course)]);
        renderer.load_textures(srf::extract_vehicle_sprites(rom,course),srf::extract_vehicle_sprites(rom,course,true));
        renderer.load_backdrop(srf::extract_course_backdrop(rom,course));
        const auto world=srf::extract_world(rom,srf::extract_course_scene(rom,course));
        renderer.load_world(world,0);
        const auto check_car=night?check_night_owl:check_sunset;
        check_car(renderer,target,world,rom,srf::PlayerCar::four_wd,"4wd","clean",output);
        check_car(renderer,target,world,rom,srf::PlayerCar::coupe,"coupe","coupe",output);
        check_car(renderer,target,world,rom,srf::PlayerCar::f_type,"f-type","f_type",output);
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
