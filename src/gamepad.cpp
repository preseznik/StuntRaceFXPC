#include "srf/gamepad.hpp"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_error.h>
#include <array>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <stdexcept>

namespace srf {
namespace {
struct Bindings {
    std::array<SDL_GamepadButton,8> buttons{SDL_GAMEPAD_BUTTON_DPAD_LEFT,
        SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST,SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_BACK,SDL_GAMEPAD_BUTTON_RIGHT_STICK};
    std::array<SDL_GamepadAxis,3> axes{SDL_GAMEPAD_AXIS_LEFTX,
        SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,SDL_GAMEPAD_AXIS_LEFT_TRIGGER};
    int deadzone=8000,trigger_threshold=4000;
    bool analog=false;
};
std::string trim(std::string text) {
    const auto first=text.find_first_not_of(" \t\r\n");
    if(first==std::string::npos)return {};
    return text.substr(first,text.find_last_not_of(" \t\r\n")-first+1);
}
Bindings read_bindings(const std::filesystem::path& path) {
    Bindings result;
    if(path.empty() || !std::filesystem::exists(path))return result;
    std::ifstream input(path);
    if(!input)throw std::runtime_error("Cannot read controller settings");
    const std::array<std::string,8> names{"steer_left","steer_right","accelerate","brake","boost","pause","restart","recenter"};
    const std::array<std::string,3> axis_names{"steering_axis","accelerate_axis","brake_axis"};
    std::map<std::string,bool> seen;
    std::string line;
    while(std::getline(input,line)) {
        line=trim(line);
        if(line.empty() || line[0]=='#' || line[0]==';')continue;
        const auto split=line.find('=');
        if(split==std::string::npos)throw std::runtime_error("Invalid controller settings line");
        const auto key=trim(line.substr(0,split));auto value=trim(line.substr(split+1));
        std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return char(std::tolower(c));});
        if(seen[key])throw std::runtime_error("Duplicate controller setting: "+key);
        seen[key]=true;
        bool found=false;
        for(std::size_t i=0;i<names.size();++i)if(key==names[i]) {
            // SDL 3.4 keeps a/b/x/y as its string API despite position enums.
            const auto token=value=="south"?"a":value=="east"?"b":value=="west"?"x":value=="north"?"y":value.c_str();
            const auto button=SDL_GetGamepadButtonFromString(token);
            if(value!="none" && button==SDL_GAMEPAD_BUTTON_INVALID)throw std::runtime_error("Invalid controller button: "+value);
            result.buttons[i]=button;found=true;break;
        }
        for(std::size_t i=0;i<axis_names.size();++i)if(key==axis_names[i]) {
            const auto axis=SDL_GetGamepadAxisFromString(value.c_str());
            if(value!="none" && axis==SDL_GAMEPAD_AXIS_INVALID)throw std::runtime_error("Invalid controller axis: "+value);
            result.axes[i]=axis;found=true;break;
        }
        if(key=="deadzone" || key=="trigger_threshold") {
            int number=0;
            const auto parsed=std::from_chars(value.data(),value.data()+value.size(),number);
            if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || number<1 || number>30000)
                throw std::runtime_error("Controller thresholds must be integers from 1 to 30000");
            (key=="deadzone"?result.deadzone:result.trigger_threshold)=number;found=true;
        }
        if(key=="steering_mode") {
            if(value!="digital" && value!="analog")throw std::runtime_error("Steering mode must be digital or analog");
            result.analog=value=="analog";found=true;
        }
        if(!found)throw std::runtime_error("Unknown controller setting: "+key);
    }
    return result;
}
}
struct Gamepad::Impl {
    Bindings bindings;
    SDL_Gamepad* pad=nullptr;
    bool armed=false,previous_pause=false,previous_restart=false,previous_recenter=false;
    bool menu_armed=false;
    std::array<bool,4> map_released{};
    std::uint16_t frontend_released{};
    std::string name,error;
    explicit Impl(const std::filesystem::path& config):bindings(read_bindings(config)) {
        // The game owns a Win32 window, not an SDL window. Poll SDL ourselves
        // and gate every result using the actual Win32 foreground state.
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
        if(!SDL_InitSubSystem(SDL_INIT_GAMEPAD))throw std::runtime_error(SDL_GetError());
        SDL_SetGamepadEventsEnabled(false);
        SDL_SetJoystickEventsEnabled(false);
    }
    ~Impl() {if(pad)SDL_CloseGamepad(pad);SDL_QuitSubSystem(SDL_INIT_GAMEPAD);}
    bool refresh() {
        SDL_UpdateGamepads();
        const bool disconnected=pad && !SDL_GamepadConnected(pad);
        if(disconnected) {
            SDL_CloseGamepad(pad);pad=nullptr;name.clear();armed=false;
            previous_pause=previous_restart=previous_recenter=menu_armed=false;
            frontend_released=0;
            map_released.fill(false);
        }
        if(!pad) {
            int count=0;auto* ids=SDL_GetGamepads(&count);
            if(!ids)error=SDL_GetError();
            for(int i=0;i<count && !pad;++i) {
                pad=SDL_OpenGamepad(ids[i]);
                if(!pad)error=SDL_GetError();
            }
            SDL_free(ids);
            if(pad) {const auto* text=SDL_GetGamepadName(pad);name=text?text:"Gamepad";error.clear();armed=false;}
        }
        return disconnected;
    }
    GamepadInput poll(bool focused) {
        GamepadInput result;
        result.disconnected=refresh();
        if(!pad)return result;
        const auto button=[&](std::size_t i) {return bindings.buttons[i]!=SDL_GAMEPAD_BUTTON_INVALID && SDL_GetGamepadButton(pad,bindings.buttons[i]);};
        const auto axis=[&](std::size_t i) {return bindings.axes[i]==SDL_GAMEPAD_AXIS_INVALID?0:int(SDL_GetGamepadAxis(pad,bindings.axes[i]));};
        const int steering=std::clamp(axis(0),-32767,32767);
        const bool left_button=button(0),right_button=button(1);
        const bool proportional=bindings.analog && bindings.axes[0]!=SDL_GAMEPAD_AXIS_INVALID && !left_button && !right_button;
        // Remove the deadzone, then use its remaining travel for the full range.
        const int magnitude=std::max(0,std::abs(steering)-bindings.deadzone)*32767/(32767-bindings.deadzone);
        const int target=steering<0?-magnitude:magnitude;
        const bool left=left_button || (proportional?target<0:(!bindings.analog && steering<=-bindings.deadzone));
        const bool right=right_button || (proportional?target>0:(!bindings.analog && steering>=bindings.deadzone));
        const bool throttle=button(2) || axis(1)>=bindings.trigger_threshold;
        const bool brake=button(3) || axis(2)>=bindings.trigger_threshold;
        const bool boost=button(4),pause=button(5),restart=button(6),recenter=button(7);
        const bool neutral=!(left || right || throttle || brake || boost || pause || restart || recenter);
        if(!focused) {armed=false;previous_pause=previous_restart=previous_recenter=false;return result;}
        // Attach/focus changes must not turn held Start/Back into actions or
        // resume with a stale throttle. Require one neutral observation first.
        if(!armed) {armed=neutral;return result;}
        if(proportional)result.steering_q15=static_cast<std::int16_t>(target);
        if(left)result.requested|=1;
        if(right)result.requested|=2;
        if(throttle)result.requested|=0x10;
        if(brake)result.requested|=0x40;
        if(boost)result.requested|=0x400;
        result.pause=pause && !previous_pause;result.restart=restart && !previous_restart;
        result.recenter=recenter && !previous_recenter;
        previous_pause=pause;previous_restart=restart;previous_recenter=recenter;
        return result;
    }
};
Gamepad::Gamepad(const std::filesystem::path& config):impl_(std::make_unique<Impl>(config)) {}
Gamepad::~Gamepad()=default;
GamepadInput Gamepad::poll(bool focused) {return impl_->poll(focused);}
std::uint16_t Gamepad::poll_frontend(bool focused) {
    impl_->refresh();
    if(!impl_->pad || !focused) {impl_->frontend_released=0;return 0;}
    constexpr std::array keys{SDL_GAMEPAD_BUTTON_SOUTH,SDL_GAMEPAD_BUTTON_WEST,
        SDL_GAMEPAD_BUTTON_BACK,SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_DPAD_UP,
        SDL_GAMEPAD_BUTTON_DPAD_DOWN,SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_NORTH,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
        SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
    std::uint16_t held=0;
    for(unsigned i=0;i<keys.size();++i)
        if(SDL_GetGamepadButton(impl_->pad,keys[i]))held|=0x8000u>>i;
    impl_->frontend_released|=static_cast<std::uint16_t>(~held);
    return held&impl_->frontend_released;
}
CourseMapButtons Gamepad::poll_course_map(bool focused) {
    impl_->refresh();
    if(!focused || !impl_->pad) {impl_->map_released.fill(false);return {};}
    constexpr std::array keys{SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_SOUTH,
                              SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
    std::array<bool,4> levels{};
    for(unsigned i=0;i<keys.size();++i) {
        const bool down=SDL_GetGamepadButton(impl_->pad,keys[i]);
        if(!down)impl_->map_released[i]=true;
        levels[i]=down && impl_->map_released[i];
    }
    return {levels[0],levels[1],levels[2],levels[3]};
}
MenuButton Gamepad::poll_menu(bool focused) {
    impl_->refresh();
    if(!impl_->pad || !focused) {impl_->menu_armed=false;return MenuButton::none;}
    constexpr std::array buttons{SDL_GAMEPAD_BUTTON_DPAD_UP,SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT,SDL_GAMEPAD_BUTTON_DPAD_RIGHT,SDL_GAMEPAD_BUTTON_SOUTH,
        SDL_GAMEPAD_BUTTON_EAST,SDL_GAMEPAD_BUTTON_START,SDL_GAMEPAD_BUTTON_BACK};
    constexpr std::array actions{MenuButton::up,MenuButton::down,MenuButton::left,MenuButton::right,
        MenuButton::accept,MenuButton::back,MenuButton::accept,MenuButton::back};
    for(std::size_t i=0;i<buttons.size();++i) if(SDL_GetGamepadButton(impl_->pad,buttons[i])) {
        const bool ready=impl_->menu_armed;
        impl_->menu_armed=false;
        return ready?actions[i]:MenuButton::none;
    }
    impl_->menu_armed=true;
    return MenuButton::none;
}
GamepadInput combine_player_input(std::uint16_t keyboard,GamepadInput gamepad) {
    if(keyboard&3) {gamepad.requested&=std::uint16_t(~3U);gamepad.steering_q15.reset();}
    gamepad.requested|=keyboard;
    return gamepad;
}
std::string Gamepad::status() const {
    if(!impl_->error.empty())return "Controller: "+impl_->error;
    if(!impl_->pad)return "Controller: not connected (keyboard available)";
    return "Controller: "+impl_->name+(impl_->bindings.analog?" | proportional steering":" | digital steering")+
        (impl_->armed?"":" - release controls to enable");
}
}
