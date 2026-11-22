#pragma once
#include <d3d11.h>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>

namespace srf::vr {
struct Eye {
    std::array<float,3> meters;
    std::array<float,4> orientation;
    float left,right,down,up;
};
struct Display {
    std::wstring instruments,message;
    bool recenter=false;
};
// Accumulate integer nanoseconds at the playable game's existing phase rate.
class RaceClock {
    int64_t last{},fraction{};
    unsigned rate;
public:
    explicit RaceClock(unsigned ticks_per_second=14):rate(ticks_per_second){}
    unsigned advance(int64_t predicted,bool driving) {
        const auto elapsed=last?predicted-last:0;
        last=driving?predicted:0;
        if(!driving || elapsed<0 || elapsed>250000000){fraction=0;return 0;}
        fraction+=elapsed*rate;
        const unsigned phases=unsigned(fraction/1000000000);
        fraction%=1000000000;
        return phases;
    }
};
// The native driver executes paired vehicle callbacks in actor order. Display
// the first callback on the half-phase and the second on the original boundary.
class BonnetMotionClock {
    RaceClock clock{28};
    bool halfway{};
public:
    bool advance(int64_t predicted,bool driving,const auto& step) {
        const auto count=clock.advance(predicted,driving);
        for(unsigned n=0;n<count;++n){
            if(!halfway)step();
            halfway=!halfway;
        }
        return halfway;
    }
};
class Drive {
public:
    explicit Drive(std::ostream& diagnostics,unsigned preferred_hz=0,unsigned eye_scale_percent=100);
    ~Drive();
    ID3D11Device* device() const;
    using Update=std::function<Display(int64_t predicted,bool focused)>;
    using Draw=std::function<void(ID3D11Texture2D*,DXGI_FORMAT,const Eye&)>;
    // pump returns false on window close. Only the runtime paces active frames.
    void run(const std::function<bool()>& pump,const Update&,const Draw&);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
