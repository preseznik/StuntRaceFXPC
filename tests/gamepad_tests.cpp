#include "srf/gamepad.hpp"
#include "srf/boot_title.hpp"
#include "srf/boot_menu.hpp"
#include "srf/demo_flow.hpp"
#include "srf/class_board.hpp"
#include "srf/native_race.hpp"
#include "srf/race_audio.hpp"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_error.h>
#include <windows.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool condition,const char* text) {if(!condition)throw std::runtime_error(std::string(text)+": "+SDL_GetError());}
struct VirtualPad {
    SDL_JoystickID id=0;SDL_Joystick* joystick=nullptr;
    VirtualPad() {
        SDL_VirtualJoystickDesc descriptor{};SDL_INIT_INTERFACE(&descriptor);
        descriptor.type=SDL_JOYSTICK_TYPE_GAMEPAD;
        descriptor.naxes=SDL_GAMEPAD_AXIS_COUNT;descriptor.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
        descriptor.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
        descriptor.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
        descriptor.name="Stunt Race FX input regression";
        id=SDL_AttachVirtualJoystick(&descriptor);require(id!=0,"Attach virtual pad");
        joystick=SDL_OpenJoystick(id);require(joystick!=nullptr,"Open virtual pad");neutral();
    }
    void button(SDL_GamepadButton button,bool down) {require(SDL_SetJoystickVirtualButton(joystick,button,down),"Virtual button");}
    void axis(SDL_GamepadAxis axis,Sint16 value) {require(SDL_SetJoystickVirtualAxis(joystick,axis,value),"Virtual axis");}
    void neutral() {
        for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;++i)button(SDL_GamepadButton(i),false);
        for(int i=0;i<SDL_GAMEPAD_AXIS_COUNT;++i)axis(SDL_GamepadAxis(i),i>=SDL_GAMEPAD_AXIS_LEFT_TRIGGER?-32768:0);
    }
    void detach() {if(joystick) {SDL_CloseJoystick(joystick);joystick=nullptr;}if(id) {require(SDL_DetachVirtualJoystick(id),"Detach virtual pad");id=0;}}
    ~VirtualPad() {if(joystick)SDL_CloseJoystick(joystick);if(id)SDL_DetachVirtualJoystick(id);}
};
void full_race(srf::Gamepad& input,VirtualPad& pad,const std::filesystem::path& rom_path,const std::filesystem::path& fixture,bool analog=false) {
    const auto rom=srf::Rom::load(rom_path);
    srf::NativeRace direct(rom),controller(rom);
    srf::RaceAudioState direct_audio,controller_audio;
    std::ifstream file(fixture);std::string line;std::getline(file,line);unsigned phases=0;
    while(std::getline(file,line)) {
        std::istringstream row(line);unsigned phase,wishes,ticks;char comma;
        require(bool(row>>phase>>comma>>wishes>>comma>>ticks),"Input fixture row");
        pad.neutral();
        if(analog)pad.axis(SDL_GAMEPAD_AXIS_LEFTX,(wishes&1)?-32768:(wishes&2)?32767:0);
        else {
            pad.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT,(wishes&1)!=0);
            pad.button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT,(wishes&2)!=0);
        }
        pad.button(SDL_GAMEPAD_BUTTON_SOUTH,(wishes&0x10)!=0);
        pad.button(SDL_GAMEPAD_BUTTON_EAST,(wishes&0x60)!=0);
        pad.button(SDL_GAMEPAD_BUTTON_WEST,(wishes&0x400)!=0);
        const auto sampled=input.poll(true);
        // The keyboard's X alias (20) and brake (40) are the same original A
        // button; preserve the raw fixture for the independent adapter below.
        const auto canonical=(wishes&~0x20u)|((wishes&0x20u)?0x40u:0u);
        require(sampled.requested==canonical,"Controller wishes differ from keyboard fixture");
        srf::RaceAudioOutput events_a,events_b;
        direct.step(srf::prepare_player_input(direct_audio,direct.racers()[3].body,std::uint16_t(wishes),direct.countdown_active(),events_a),std::uint8_t(ticks));
        controller.step(srf::prepare_player_input(controller_audio,controller.racers()[3].body,sampled.requested,controller.countdown_active(),events_b),std::uint8_t(ticks),sampled.steering_q15);
        for(unsigned car=0;car<4;++car) {
            require(direct.racers()[car].body.words==controller.racers()[car].body.words,"Controller changed body state");
            for(unsigned wheel=0;wheel<4;++wheel)require(direct.racers()[car].wheels[wheel].words==controller.racers()[car].wheels[wheel].words,"Controller changed wheel state");
        }
        require(direct.remaining_seconds()==controller.remaining_seconds() && direct.elapsed_centiseconds()==controller.elapsed_centiseconds(),"Controller changed clock");
        ++phases;
    }
    require(phases==1637 && controller.completion().finished && controller.failure()==srf::RaceFailure::none,"Controller fixture must finish clean race");
    std::cout<<"Full "<<(analog?"analog endpoints":"digital controller")<<"/keyboard comparison: "<<phases<<" phases, identical bodies/wheels/timing, finished\n";
}

void partial_steering(srf::Gamepad& input,VirtualPad& pad,const std::filesystem::path& rom_path) {
    const auto rom=srf::Rom::load(rom_path);
    srf::NativeRace direct(rom),controller(rom),digital(rom);
    srf::RaceAudioState direct_audio,controller_audio,digital_audio;
    std::vector<std::array<srf::Vehicle,4>> states;
    unsigned different=0;
    // After the countdown, exercise a small S bend with changing target and a
    // release. This is an input-path/reset check, not an original analog oracle.
    const auto target=[](unsigned phase){return phase<100?0:phase<108?-16382:phase<116?-8192:
        phase<124?0:phase<132?16382:phase<140?8192:0;};
    for(unsigned run=0;run<2;++run) {
        direct.reset();controller.reset();digital.reset();
        direct_audio={};controller_audio={};digital_audio={};
        for(unsigned phase=0;phase<160;++phase) {
            const int steering=target(phase);
            // These exact raw inputs map to quarter/half travel after deadzone.
            const int raw=steering==0?0:steering<0?(steering==-16382?-20383:-14192):steering==16382?20383:14192;
            pad.neutral();pad.axis(SDL_GAMEPAD_AXIS_LEFTX,static_cast<Sint16>(raw));
            pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true);
            const auto sampled=input.poll(true);
            require(sampled.steering_q15==steering,"Partial stick target normalization");
            const auto requested=std::uint16_t(0x10|(steering<0?1:steering>0?2:0));
            srf::RaceAudioOutput a,b,c;
            direct.step(srf::prepare_player_input(direct_audio,direct.racers()[3].body,requested,direct.countdown_active(),a),4,static_cast<std::int16_t>(steering));
            controller.step(srf::prepare_player_input(controller_audio,controller.racers()[3].body,sampled.requested,controller.countdown_active(),b),4,sampled.steering_q15);
            digital.step(srf::prepare_player_input(digital_audio,digital.racers()[3].body,requested,digital.countdown_active(),c),4);
            for(unsigned car=0;car<4;++car) {
                const auto& actual=controller.racers()[car];
                require(direct.racers()[car].body.words==actual.body.words,"Partial analog body input path");
                for(unsigned wheel=0;wheel<4;++wheel)require(direct.racers()[car].wheels[wheel].words==actual.wheels[wheel].words,"Partial analog wheel input path");
                if(run) {
                    require(states[phase][car].body.words==actual.body.words,"Partial analog body reset");
                    for(unsigned wheel=0;wheel<4;++wheel)require(states[phase][car].wheels[wheel].words==actual.wheels[wheel].words,"Partial analog wheel reset");
                }
            }
            if(!run) {states.push_back(controller.racers());different+=digital.racers()[3].body.word(0x3a)!=controller.racers()[3].body.word(0x3a);}
            require(direct.elapsed_centiseconds()==controller.elapsed_centiseconds() && direct.remaining_seconds()==controller.remaining_seconds(),"Partial analog clock parity");
        }
        require(controller.failure()==srf::RaceFailure::none,"Partial analog scenario must remain driveable");
    }
    require(different>=16,"Partial analog must change steering versus digital input");
    std::cout<<"Partial analog SDL/direct comparison and reset: 160 phases, "<<different<<" steering samples differ from digital\n";
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        // Process-local test isolation: never read or change a physical pad.
        for(const char* hint:{SDL_HINT_JOYSTICK_HIDAPI,SDL_HINT_JOYSTICK_RAWINPUT,SDL_HINT_JOYSTICK_DIRECTINPUT,
            SDL_HINT_JOYSTICK_WGI,SDL_HINT_JOYSTICK_GAMEINPUT,SDL_HINT_XINPUT_ENABLED})SDL_SetHint(hint,"0");
        require(SDL_InitSubSystem(SDL_INIT_GAMEPAD),"Initialize SDL gamepad");
        {
            srf::Gamepad input;require(input.poll(true).requested==0,"No controller input");
            VirtualPad pad;
            srf::BootTitle title;
            pad.button(SDL_GAMEPAD_BUTTON_START,true);
            const auto title_tick=[&] {
                const auto buttons=input.poll_frontend(true);
                title.advance((buttons&0x1000)!=0,(buttons&0x8000)!=0);
            };
            for(unsigned i=0;i<610;++i)title_tick();
            require(!title.finished(),"Held Start skipped the title after attachment");
            pad.neutral();title_tick();
            pad.button(SDL_GAMEPAD_BUTTON_START,true);title_tick();pad.neutral();
            for(unsigned i=0;i<165;++i)title_tick();
            require(title.finished(),"Virtual controller could not leave title");
            for(const auto interrupt:{SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_BACK,
                    SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_DPAD_LEFT,
                    SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_RIGHT}) {
                srf::DemoFlow demo;
                pad.neutral();input.poll_frontend(true);
                for(const auto ignored:{SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_NORTH,
                        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER})
                    pad.button(ignored,true);
                for(unsigned i=0;i<310;++i)demo.advance(input.poll_frontend(true));
                require(!demo.finished(),"SNES A/X/L/R interrupted the idle demo");
                pad.button(interrupt,true);
                for(unsigned i=0;i<120 && !demo.finished();++i)demo.advance(input.poll_frontend(true));
                require(demo.finished() && demo.to_menu(),"Controller failed to interrupt the idle demo");
            }
            pad.neutral();input.poll_frontend(true);
            srf::BootMenu boot;
            const auto tick=[&]{boot.advance(input.poll_frontend(true));};
            const auto tap=[&](SDL_GamepadButton key){
                pad.button(key,true);tick();pad.neutral();tick();
            };
            for(unsigned i=0;i<100;++i)tick();
            tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);tap(SDL_GAMEPAD_BUTTON_DPAD_UP);
            tap(SDL_GAMEPAD_BUTTON_SOUTH);
            for(unsigned i=0;i<80;++i)tick();
            require(boot.frame().screen==srf::BootMenuScreen::car,"Controller could not enter Car Selection");
            tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            require(boot.car()==srf::PlayerCar::coupe,"Controller selected the wrong car");
            tap(SDL_GAMEPAD_BUTTON_SOUTH);
            tap(SDL_GAMEPAD_BUTTON_WEST);
            require(boot.frame().screen==srf::BootMenuScreen::car,"SNES Y failed to return to Car Selection");
            tap(SDL_GAMEPAD_BUTTON_SOUTH);
            tap(SDL_GAMEPAD_BUTTON_DPAD_UP);tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
            require(boot.initials()=="BAZ","Controller could not edit all initials");
            pad.button(SDL_GAMEPAD_BUTTON_START,true);
            require(input.poll_frontend(false)==0 && input.poll_frontend(true)==0,"Refocus accepted held Start");
            pad.neutral();tick();tap(SDL_GAMEPAD_BUTTON_START);
            for(unsigned i=0;i<120 && boot.event()!=srf::BootMenuEvent::board;++i)tick();
            require(boot.event()==srf::BootMenuEvent::board,"Controller could not confirm initials");
            for(const bool unlocked:{false,true}) {
                srf::ClassBoard board(unlocked);
                const auto board_tick=[&]{board.advance(input.poll_frontend(true));};
                const auto board_tap=[&](SDL_GamepadButton button){
                    pad.button(button,true);board_tick();pad.neutral();board_tick();
                };
                for(unsigned i=0;i<125;++i)board_tick();
                board_tap(SDL_GAMEPAD_BUTTON_DPAD_UP);board_tap(SDL_GAMEPAD_BUTTON_DPAD_UP);
                require(board.frame().speed_class==(unlocked?2u:1u),"Class board ignored the Master unlock");
                for(unsigned i=0;i<24;++i)board_tick();
                board_tap(SDL_GAMEPAD_BUTTON_START);
                for(unsigned i=0;i<125;++i)board_tick();
                require(board.choosing_play(),"Class records did not open from the controller");
                board_tap(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);board_tick();
                require(board.frame().record_page==1,"Controller could not browse course records");
                board_tap(SDL_GAMEPAD_BUTTON_DPAD_DOWN);board_tick();
                require(board.frame().cancel_selected,"Controller could not select Cancel");
                board_tap(SDL_GAMEPAD_BUTTON_SOUTH);
                for(unsigned i=0;i<60;++i)board_tick();
                require(!board.choosing_play() && !board.finished(),"Cancel failed to return to the class board");
                board_tap(SDL_GAMEPAD_BUTTON_START);
                for(unsigned i=0;i<125;++i)board_tick();
                board_tap(SDL_GAMEPAD_BUTTON_SOUTH);
                for(unsigned i=0;i<195;++i)board_tick();
                require(board.finished(),"Controller did not release the selected class to its first grid");
            }
            pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true);
            require(input.poll_menu(true)==srf::MenuButton::none,"New menu ignores held throttle");
            pad.neutral();input.poll_menu(true);pad.button(SDL_GAMEPAD_BUTTON_DPAD_DOWN,true);
            require(input.poll_menu(true)==srf::MenuButton::down,"Menu moves focus once");
            require(input.poll_menu(true)==srf::MenuButton::none,"Held menu action does not repeat");
            pad.neutral();input.poll_menu(true);pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true);
            require(input.poll_menu(true)==srf::MenuButton::accept,"Menu confirms from gamepad");
            require(!input.poll_course_map(false).accept && !input.poll_course_map(true).accept,"Board ignores held confirm on focus");
            pad.neutral();input.poll_course_map(true);
            pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true);
            require(input.poll_course_map(true).accept && input.poll_course_map(true).accept,"Board reads held B through the intro");
            pad.button(SDL_GAMEPAD_BUTTON_START,true);
            const auto both=input.poll_course_map(true);
            require(both.start && both.accept,"Board needs independent Start and B levels");
            pad.neutral();
            input.poll(true);
            require(input.status().find("Stunt Race FX input regression")!=std::string::npos,"Hotplug selection");
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,7999);require(input.poll(true).requested==0,"Steering deadzone");
            pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,7999);
            require(input.poll(true,true).look_x==0,"Right stick camera deadzone");
            pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,32767); pad.axis(SDL_GAMEPAD_AXIS_RIGHTY,-32768);
            require(input.poll(true).look_x==0,"Camera look must be opt-in");
            const auto look=input.poll(true,true);
            require(look.look_x==1 && look.look_y==-1 && look.requested==0,"Camera stick independent of driving");
            require(input.poll(false,true).look_y==0,"Camera look ignores unfocused pad");
            pad.neutral();input.poll(true);
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,8000);require(input.poll(true).requested==2,"Positive steering threshold");
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,-8000);require(input.poll(true).requested==1,"Negative steering threshold");
            pad.neutral();pad.axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767);require(input.poll(true).requested==0x10,"Right trigger throttle");
            pad.neutral();pad.axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,32767);require(input.poll(true).requested==0x40,"Left trigger brake");
            pad.neutral();pad.button(SDL_GAMEPAD_BUTTON_WEST,true);require(input.poll(true).requested==0x400,"Boost button");
            pad.neutral();pad.button(SDL_GAMEPAD_BUTTON_START,true);require(input.poll(true).pause,"Pause rising edge");
            require(!input.poll(true).pause,"Held pause must not toggle repeatedly");
            pad.neutral();input.poll(true);pad.button(SDL_GAMEPAD_BUTTON_BACK,true);require(input.poll(true).restart,"Restart rising edge");
            require(!input.poll(true).restart,"Held restart must not repeat");
            pad.neutral();input.poll(true);pad.button(SDL_GAMEPAD_BUTTON_RIGHT_STICK,true);
            require(input.poll(true).recenter && !input.poll(true).recenter,"VR recenter fires once per click");
            pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true);require(input.poll(false).requested==0,"Unfocused input must be zero");
            require(input.poll(true).requested==0 && !input.poll(true).restart,"Held inputs after focus must remain disarmed");
            pad.neutral();input.poll(true);pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true);require(input.poll(true).requested==0x10,"Neutral rearm");
            pad.detach();const auto lost=input.poll(true);require(lost.disconnected && lost.requested==0,"Disconnect clears input and requests pause");
            require(!input.poll(true).disconnected,"Disconnect event occurs once");
            VirtualPad replacement;replacement.button(SDL_GAMEPAD_BUTTON_BACK,true);require(!input.poll(true).restart,"Held restart on reconnect");
            replacement.neutral();input.poll(true);
            if(argc==3)full_race(input,replacement,argv[1],argv[2]);
        }
        const auto config=std::filesystem::temp_directory_path()/("srf-controller-regression-"+std::to_string(GetCurrentProcessId())+".ini");
        // Exclusive evidence/test path avoids overwriting user files.
        require(!std::filesystem::exists(config),"Test config already exists");
        {std::ofstream file(config);file<<"# remap\naccelerate=NORTH\nbrake=south\nboost=east\naccelerate_axis=none\nbrake_axis=none\nsteering_axis=rightx\ndeadzone=12000\n";}
        {
            srf::Gamepad input(config);VirtualPad pad;input.poll(true);
            pad.button(SDL_GAMEPAD_BUTTON_NORTH,true);require(input.poll(true).requested==0x10,"Remapped north acceleration");
            pad.neutral();pad.button(SDL_GAMEPAD_BUTTON_SOUTH,true);require(input.poll(true).requested==0x40,"Remapped south brake");
            pad.neutral();pad.axis(SDL_GAMEPAD_AXIS_LEFTX,32767);require(input.poll(true).requested==0,"Disabled old steering axis");
            pad.axis(SDL_GAMEPAD_AXIS_RIGHTX,12000);require(input.poll(true).requested==2,"Remapped steering axis and threshold");
            require(input.poll(true,true).look_x==0,"Remapped steering axis must not rotate camera");
        }
        {std::ofstream file(config);file<<"steering_mode=ANALOG\n";}
        {
            srf::Gamepad input(config);VirtualPad pad;input.poll(true);
            require(input.poll(true).steering_q15==0,"Analog neutral target");
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,8000);require(input.poll(true).steering_q15==0,"Analog deadzone edge");
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,8001);require(input.poll(true).steering_q15==1,"Continuous deadzone exit");
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,20383);const auto half=input.poll(true);
            require(half.requested==2 && half.steering_q15==16382,"Half remaining stick travel");
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,-20383);require(input.poll(true).steering_q15==-16382,"Symmetric partial steering");
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,-32768);require(input.poll(true).steering_q15==-32767,"Full left endpoint");
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,32767);require(input.poll(true).steering_q15==32767,"Full right endpoint");
            pad.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT,true);const auto digital=input.poll(true);
            require(digital.requested==1 && !digital.steering_q15,"D-pad overrides opposite stick with digital steering");
            pad.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT,false);
            const auto combined=srf::combine_player_input(0x11,input.poll(true));
            require(combined.requested==0x11 && !combined.steering_q15,"Keyboard steering overrides opposite analog target");
            require(srf::combine_player_input(0x10,input.poll(true)).steering_q15==32767,"Keyboard throttle retains analog steering");
            const auto unfocused=input.poll(false);
            require(!unfocused.steering_q15 && !unfocused.requested,"Focus loss clears analog target");
            require(!input.poll(true).steering_q15,"Held analog must not rearm on focus");
            pad.neutral();input.poll(true);
            if(argc==3) {full_race(input,pad,argv[1],argv[2],true);partial_steering(input,pad,argv[1]);}
            pad.detach();const auto lost=input.poll(true);
            require(lost.disconnected && !lost.steering_q15,"Disconnect clears proportional target");
        }
        {std::ofstream file(config);file<<"steering_mode=analog\nsteering_axis=none\n";}
        {
            srf::Gamepad input(config);VirtualPad pad;input.poll(true);
            pad.axis(SDL_GAMEPAD_AXIS_LEFTX,32767);const auto disabled=input.poll(true);
            require(!disabled.requested && !disabled.steering_q15,"Unassigned axis disables proportional target");
            pad.button(SDL_GAMEPAD_BUTTON_DPAD_LEFT,true);require(input.poll(true).requested==1,"D-pad works with unassigned analog axis");
        }
        for(const auto* invalid:{"steering_mode=linear\n","steering_mode=analog\nsteering_mode=digital\n","deadzone=0\n","deadzone=8000junk\n","boost=nope\n","boost=west\nboost=east\n","unknown=1\n"}) {
            {std::ofstream file(config);file<<invalid;}
            bool rejected=false;try {srf::Gamepad input(config);}catch(const std::exception&){rejected=true;}
            require(rejected,"Invalid config must be rejected");
        }
        std::filesystem::remove(config);SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        std::cout<<"Controller hotplug, thresholds, remapping, focus, pause/restart and disconnect checks passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
