// Uses the game's native D3D11 renderer with an offscreen target and recorded inputs.
#define wmain preview_application_entry
#include "../src/preview.cpp"
#undef wmain
#include "stunt_test_inputs.hpp"

namespace {
std::vector<std::size_t> changed_pixels(const std::vector<BYTE>& a,const std::vector<BYTE>& b) {
    std::vector<std::size_t> result;
    for(std::size_t i=0;i<a.size();i+=4)
        if(a[i]!=b[i] || a[i+1]!=b[i+1] || a[i+2]!=b[i+2])result.push_back(i/4);
    return result;
}

void verify_blue_lake_water(const srf::Rom& rom,const std::filesystem::path& output) {
    const auto course=srf::StuntCourse::blue_lake;
    srf::RockFieldSession session(rom,srf::PlayerCar::four_wd,course);
    const auto inputs=read_inputs(std::filesystem::path(SRF_TEST_FIXTURES)/"blue_lake_4wd_inputs.csv");
    while(session.drive.phase()<118)session.advance(inputs.at(session.drive.phase()).buttons);
    const auto& car=session.drive.player();
    const auto world=srf::extract_world(rom,session.drive.scene());
    const auto pose=vehicle_pose(rom,car,car,true,1,static_cast<std::uint8_t>(car.body.word(0x24)>>8));
    for(const auto look:{RaceLook::classic,RaceLook::n64,RaceLook::remastered}) {
        RaceGraphics graphics{};graphics.look=look;
        Renderer renderer(nullptr,nullptr,0,graphics);
        srf::generated::CoursePackage colors{};
        colors.day_palette=colors.dusk_palette=srf::extract_rock_field_palette(rom,course);
        renderer.set_course_colors(colors);
        // Classic sorts its faces when loading the world, using the current camera.
        renderer.load_world(world,0);
        chase_camera(renderer,pose,lerp_angle(car.body.word(0x24),car.body.word(0x24),1),{},srf::CourseId::easy_ride);
        const auto render=[&](std::optional<srf::StuntCourse> selected) {
            renderer.select_course(std::nullopt,selected);
            renderer.load_textures(srf::extract_rock_field_sprites(rom,course));
            renderer.load_backdrop(srf::extract_rock_field_backdrop(rom,course));
            renderer.set_environment({});
            renderer.load_stunt_world(world,pose,rom,session);
            renderer.render();
            return renderer.readback_bgra();
        };
        const auto backdrop=render({}),water=render(course);
        if(look==RaceLook::classic) {
            if(water!=backdrop)throw std::runtime_error("Blue Lake changed the Classic course backdrop");
            continue;
        }
        unsigned visible=0,detail=0;
        for(unsigned y=controls.height/2;y<controls.height;++y)for(unsigned x=1;x<controls.width;++x) {
            const auto i=4*(y*controls.width+x);
            // Compare neighbouring pixels that used to be the same blue gradient band.
            if(backdrop[i]<180 || backdrop[i]<backdrop[i+2]+80 ||
               !std::equal(backdrop.begin()+i,backdrop.begin()+i+3,backdrop.begin()+i-4))continue;
            if(water[i]<=water[i+2]+20 || water[i]==backdrop[i])continue;
            ++visible;
            if(water[i]!=water[i-4] || water[i+1]!=water[i-3])++detail;
        }
        if(!output.empty())renderer.screenshot(output/(look==RaceLook::n64?"blue-lake-n64.png":"blue-lake-remastered.png"));
        if(visible<2000 || detail<visible/20)throw std::runtime_error("Blue Lake water lacks visible surface texture");
    }
}

void verify_stereo_crest(Renderer& renderer,const srf::World& world,const auto& load_variant,
                         unsigned phase,const std::filesystem::path& output) {
    auto no_ramp=world;
    if(std::erase_if(no_ramp.instances,[](const auto& i){return i.command_offset==0x6dea5;})!=1)
        throw std::runtime_error("Missing stereo ramp control");
    std::array eyes{SceneEye{},SceneEye{}};
    const auto lateral=renderer.normalized_distance(20);
    eyes[0].offset[0]=-lateral;
    eyes[1].offset[0]= lateral;
    // Parallel optical axes with different projection centers.
    eyes[0].left=-1.03125f;eyes[0].right=.96875f;
    eyes[1].left=-.96875f;eyes[1].right=1.03125f;
    for(auto& eye:eyes){eye.down=-.68f;eye.up=.6f;}
    constexpr UINT eye_width=768,eye_height=512;
    std::array surfaces{renderer.make_target(eye_width,eye_height),renderer.make_target(eye_width,eye_height)};
    const auto stereo=[&](const srf::World& geometry,bool show_star,bool save=false) {
        load_variant(geometry,show_star);
        // Share one uploaded scene. Read both targets after both draws.
        for(unsigned e=0;e<2;++e)renderer.render_eye(surfaces[e],eyes[e]);
        std::array images{renderer.readback_bgra(surfaces[0]),renderer.readback_bgra(surfaces[1])};
        if(save && !output.empty())for(unsigned e=0;e<2;++e)
            Renderer::write_png(output/(std::to_string(phase)+(e?"-right.png":"-left.png")),images[e],eye_width,eye_height);
        return images;
    };
    const auto desktop_before=renderer.readback_bgra();
    const auto shown=stereo(world,true,true),hidden=stereo(world,false);
    const auto uncovered=stereo(no_ramp,true),empty=stereo(no_ramp,false);
    std::array<size_t,2> covered_count{};
    std::array<double,2> center{};
    for(unsigned e=0;e<2;++e) {
        const auto visible_eye=changed_pixels(shown[e],hidden[e]);
        const auto whole_star=changed_pixels(uncovered[e],empty[e]);
        std::cout<<"Stereo "<<phase<<(e?" right ":" left ")<<visible_eye.size()<<'/'<<whole_star.size()<<" star pixels\n";
        if(visible_eye.empty() || whole_star.empty() ||
           !std::ranges::includes(whole_star,visible_eye))
            throw std::runtime_error("Invalid stereo ramp occlusion");
        for(auto pixel:whole_star)center[e]+=double(pixel%eye_width);
        center[e]/=double(whole_star.size());
        covered_count[e]=whole_star.size()-visible_eye.size();
    }
    if(phase==856) {
        // Place both eyes at the same point. The projection centers alone
        // now explain the full 24-pixel disparity, without depth parallax.
        eyes[1].offset=eyes[0].offset;
        const auto aligned=stereo(no_ramp,true),aligned_empty=stereo(no_ramp,false);
        const auto left=changed_pixels(aligned[0],aligned_empty[0]);
        const auto right=changed_pixels(aligned[1],aligned_empty[1]);
        if(!std::ranges::equal(left,right,[](auto l,auto r){return l==r+24;}) || left.empty() ||
           center[0]-center[1]<25)
            throw std::runtime_error("Eye separation and asymmetric projection were not applied independently");
        eyes[1]=eyes[0];
        eyes[1].orientation={0,std::sin(.05f),0,std::cos(.05f)};
        const auto rotated=stereo(no_ramp,true),rotated_empty=stereo(no_ramp,false);
        const auto turned=changed_pixels(rotated[1],rotated_empty[1]);
        double x=0;
        for(auto pixel:turned)x+=double(pixel%eye_width);
        if(turned.empty() || center[0]-x/turned.size()<30)
            throw std::runtime_error("Turning the eye right should move the star to the left");
        std::cout<<"Translated eye disparity "<<center[0]-center[1]<<" pixels, coincident eyes 24 pixels\n";
    }
    if(desktop_before!=renderer.readback_bgra())
        throw std::runtime_error("Rendering the eyes changed the desktop surface");
    if(phase==846 && (covered_count[0]!=0 || covered_count[1]==0))
        throw std::runtime_error("The ramp should cover the star only in the right eye");
    if(phase==844 && (!covered_count[0] || !covered_count[1] || covered_count[0]==covered_count[1]))
        throw std::runtime_error("Ramp crest did not occlude the star differently in each eye");
}
}

int wmain(int argc,wchar_t** argv) {
    try {
        const auto* path=argc>1?argv[1]:_wgetenv(L"SRF_ROM");
        if(!path)return 77;
        const std::filesystem::path output=argc>2?argv[2]:L"";
        if(!output.empty())std::filesystem::create_directories(output);
        const auto rom=srf::Rom::load(path);
        const auto course=srf::StuntCourse::up_n_down;
        srf::RockFieldSession session(rom,srf::PlayerCar::four_wd,course);
        const auto& drive=session.drive;
        const auto world=srf::extract_world(rom,drive.scene());
        const auto route=read_stunt_controls(std::filesystem::path(SRF_TEST_FIXTURES)/"up_down_native_inputs.csv","4wd");
        check(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
        controls=Controls{};controls.world=controls.race=true;controls.width=960;controls.height=540;
        verify_blue_lake_water(rom,output);
        controls=Controls{};controls.world=controls.race=true;controls.width=960;controls.height=540;
        Renderer renderer(nullptr);
        srf::generated::CoursePackage palette{};
        palette.day_palette=palette.dusk_palette=srf::extract_rock_field_palette(rom,course);
        renderer.set_course_colors(palette);renderer.load_textures(srf::extract_rock_field_sprites(rom,course));
        renderer.load_backdrop(srf::extract_rock_field_backdrop(rom,course));renderer.set_environment({});
        renderer.load_world(world,0);
        srf::StuntHud hud(rom,srf::PlayerCar::four_wd,course,srf::StuntAttempt::first);
        struct Sample { unsigned phase,command; bool exposed; };
        // Moving deck approach, sloped road and final crest on the same drive.
        constexpr Sample samples[]{{262,0x6ce06,true},{460,0x6cef8,false},{462,0x6cef8,false},{464,0x6cef8,true},
            {664,0x6cf7c,false},{670,0x6cf7c,false},{836,0x6d02c,false},{838,0x6d02c,false},{840,0x6d02c,false},
            {844,0x6d02c,true},{846,0x6d02c,true},{852,0x6d02c,true},{856,0x6d02c,true}};
        for(const auto& [phase,command,exposed]:samples) {
            while(drive.phase()<phase)session.advance(route.at(drive.phase()));
            const auto& car=drive.player();
            const auto pose=vehicle_pose(rom,car,car,true,1,static_cast<std::uint8_t>(car.body.word(0x24)>>8));
            chase_camera(renderer,pose,lerp_angle(car.body.word(0x24),car.body.word(0x24),1),{},srf::CourseId::easy_ride);
            hud.draw(renderer,car,session.rules,session.tick,session.released,true);
            renderer.load_stunt_world(world,pose,rom,session);renderer.render();
            const auto normal=renderer.readback_bgra();
            if(!output.empty())renderer.screenshot(output/(std::to_string(phase)+"-game.png"));
            auto stars=std::vector<srf::SceneryPose>(session.rules.star_poses().begin(),session.rules.star_poses().end());
            auto target=std::ranges::find(stars,command,&srf::SceneryPose::command_offset);
            if(target==stars.end())throw std::runtime_error("Missing star at last ramp");
            // Compare the production frame with precisely one star suppressed.
            const auto load_variant=[&](const srf::World& geometry,bool show_star) {
                target->visible=show_star;
                renderer.load_world(geometry,0,std::span(&pose,1),&rom,drive.poses(),{},session.rules.gate_poses(),
                                    drive.bridge_poses(),{},nullptr,stars);
            };
            const auto render_variant=[&](const srf::World& geometry,bool show_star) {
                load_variant(geometry,show_star);
                renderer.render();return renderer.readback_bgra();
            };
            const auto visible=changed_pixels(normal,render_variant(world,false));
            // Draw a second pair with the ordinary geometric depth test for every
            // object. The production star should cover the same pixels even when
            // the production vehicle uses its special road-sorting pass.
            auto reference=[&](bool show_star) {
                target->visible=show_star;
                auto actors=stars;
                const auto gates=session.rules.gate_poses();
                actors.insert(actors.end(),gates.begin(),gates.end());
                renderer.load_world(world,0,std::span(&pose,1),nullptr,drive.poses(),{},actors,drive.bridge_poses());
                renderer.render();return renderer.readback_bgra();
            };
            const auto geometric=reference(true);
            const auto expected=changed_pixels(geometric,reference(false));
            std::cout<<phase<<": "<<visible.size()<<" visible star pixels, geometric reference "<<expected.size()<<'\n';
            if(visible!=expected)throw std::runtime_error("Stunt star visibility differs from actual ramp depth");
            if(!exposed && !visible.empty())throw std::runtime_error("Star visible through the approach ramp");
            if(exposed && visible.empty())throw std::runtime_error("Exposed star disappeared on the ramp crest");
            if(phase==664 || phase==838) {
                const auto ramp_command=phase==664?0x6dcf9u:0x6dea5u;
                auto without_ramp=world;
                if(std::erase_if(without_ramp.instances,[&](const auto& i){return i.command_offset==ramp_command;})!=1)
                    throw std::runtime_error("Missing Up'n Down ramp");
                const auto uncovered=render_variant(without_ramp,true);
                const auto clear=changed_pixels(uncovered,render_variant(without_ramp,false));
                std::cout<<"Ramp removed: "<<clear.size()<<" visible star pixels\n";
                if(clear.empty())throw std::runtime_error("Hidden-star fixture has no visible control");
            }
            if(phase==844 || phase==846 || phase==856) {
                verify_stereo_crest(renderer,world,load_variant,phase,output);
                if(drive.phase()!=phase)throw std::runtime_error("Rendering advanced the recorded drive");
            }
        }
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
