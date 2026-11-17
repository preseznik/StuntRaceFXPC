#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace srf {
struct DemoFrame {
    unsigned tick{}, brightness{};
};

// The Easy Ride title demonstration, timed from the first black field after
// the title. The CPU's pad poll and bank-load callbacks follow the GSU work.
class DemoFlow {
    DemoFrame frame_{};
    unsigned fade_request_=1660, handoff_=1705; // natural fade and last pad dispatch
    int light_{}; // 8.8 brightness presented this field
    bool menu_{};

    // First USA Rev 1 Easy Ride tape. Each character describes one VBlank
    // from field 65 to 1659. A zero is an unsampled pad field. The remaining
    // codes store the delay (0..6) to the CPU fade request and the number of
    // extra fields (0..6) consumed loading Game Selection after black.
    // Fields outside this full-brightness portion use the startup/exit states below.
    static constexpr std::string_view polls_=
        "jcV0HA3jcV0HA3cV0HA3jcV0HA3jc0OHA3cV0HA3jc0OHA3dW0IB4jc0OHA3aT0F81G90NG9NG9NG9OH"
        "AOHA0NG9OHAOHAPIBMF80PIBMF8MF8NG90MF8MF8NG90MF8MF8MF80PIBMF80PIBMF8WPI04F8WPI04I"
        "BMF8MF80PIBPIBMF80OH03IBVOH0PIBMF80PIBMF80PI04F8MF8NG90MF8MF8NG90MF01G9NG9TMF01G"
        "90MF8NG90NG9NG90NG9NG90NG02G9OHA0NG9NG9OHA0NG9NG90NG9TMF01F8TMF01G9UNG02HAOHA0NG"
        "9OHAUNG02G9UNG02G9TMF01G90MF8MF80OHAOHA0OHAOHA0OHAOHAPIB0OHAPIBMF8MF80OHAPIBPIBM"
        "F8MF80OHAPIBVOH03HA0NG9NG90MF8MF8NG90G90MF8MF8MF8NG9MF80MF8NG9NG90NG9NG9OHA0OHAO"
        "HAPIB0OHAPIB0PIBPIB0PIBWPI04F80PIBMF80OHASLE0OHAXQJ05G90PIBZSL07IB0RKDUNG02HAUNG"
        "02fY0KD6le0QJC5eX0JC5fY0KD6leX0JC5eX0JC5MF01G9TMF01cV0HA3aT0F81bU0G92ibU0G92cV0H"
        "A3aT0F81PIBMF80PIBVOH03OH03HA0NG9TMF01G90MF8MF80PIBMF80PIBPIBVOH03IBVOH03IBPIBMF"
        "80PIBMF8MF8NG90MF8MF8TMF01F8WPI04IBVOH03IBMF80OHAVOH03HAUNG02G9OHAOHAOHAOHA0OHAO"
        "HAOHA0NG9OHANG90NG9NG9NG9NG90NG9NG9NG9NG90MF8NG9NG9NG90MF8MF8MF8MF8MF8MF80MF8MF8"
        "NG9NG90G9UNG9NG9NG9NG9OHA0NG9OHAOHAPIBPIB0OHAOHAOHAOHA0NG9OHAOHAUNG02HAPIB0PIBPI"
        "B0PIBMF8NG9PIBQJC0G9UNG9OHAPIBMF80MF8TMF01MF01G90MF80MF8TMF01WPI04PI04PI04PI04PI"
        "04PI04PI04PI04F80MF01MF01MF01MF01MF01MF01MF01MF01MF01MF01MF01MF01PI04PI04PI04OHA"
        "0OHA0TMF01PI04IB0OHAVOH03NG02G90NG90MF01MF01F8TMF01G9NG90MF8NG9NG9TMF01G9NG90MF8"
        "NG90OHAMF8WPI04F8MF8MF80OHAOHAOHAPIBMF8MF8NG9NG90HAUNG9NG9NG9NG9NG9OHAOHAOHAOHA0"
        "NG9OHA0NG9NG90MF8NG9NG9NG9OHA0NG9OHAOHAOHAPIBPIBMF80PIBMF8MF8MF80MF8MF80PIBMF8MF"
        "80PIBPIB0MF8WPI04PI04PI04OH03OH03OHA0OH03NG02NG02NG02NG02HA0NG90NG9UNG02HA0OHAVO"
        "H03NG02HA0OHAVOH03NG02HA0OHAVOH03OH03OH03IB0OHAVOH03OH03OH03HA0OHA0OHAVOH03NG02N"
        "G02NG02HA0OHA0OHAVOH03OH03HA0OHAVOH03NG02HA0OHAUNG02MF01MF01PI04OH03OH03NG0";
    static_assert(polls_.size()==1595);
    void interrupt(unsigned tick) {
        if(tick<31)return;
        if(tick<65) {
            struct Gate {unsigned from,to,fade,menu;};
            // Loader callbacks may execute before the first visible frame.
            static constexpr Gate startup[]{
                {31,36,36,66},{37,38,39,69},{39,41,42,73},{42,44,46,76},
                {46,48,49,79},{49,51,52,82},{52,54,55,86},{55,57,58,92},
                {59,61,62,115},{62,64,65,137},
            };
            for(const auto gate:startup)if(tick>=gate.from && tick<=gate.to){
                fade_request_=gate.fade;handoff_=gate.menu;menu_=true;return;
            }
            return;
        }
        if(tick<1660) {
            const char c=polls_[tick-65];
            if(c=='0')return;
            const unsigned code=(c<='9'?c-'0':c<='Z'?c-'A'+10:c-'a'+36)-1;
            fade_request_=tick+code/7;
            handoff_=fade_request_+42+29+code%7;
        } else {
            // The last camera holds for seven fields per GSU update. The
            // unpolled NMI is still visible during the 0x58 brightness ramp.
            if(tick>=1705 || (tick>=1663 && (tick-1663)%7==0))return;
            handoff_=1732+(tick==1679 || tick==1680);
        }
        menu_=true;
    }
public:
    const DemoFrame& frame() const{return frame_;}
    bool finished() const{return frame_.tick>=handoff_;}
    bool to_menu() const{return menu_;}
    void advance(std::uint16_t held_snes) {
        if(finished())return;
        const auto tick=++frame_.tick;
        // Only B, Y, Select, Start and the D-pad feed the original high-byte test.
        if(!menu_ && (held_snes&0xff00))interrupt(tick);
        if(tick>=fade_request_+2)light_=std::max(0,light_-0x58);
        else if(tick>=26) {
            // 02:89A7 indexes the fade-in table by the current integer level.
            constexpr std::array<unsigned,16> add{
                8,0x80,0x100,0x180,0x180,0x180,0x280,0x280,
                0x280,0x280,0x380,0x380,0x380,0x380,0x380,0x200};
            light_=std::min(0xf00,light_+static_cast<int>(add[light_>>8]));
        }
        frame_.brightness=static_cast<unsigned>(light_>>8);
    }
};
}

