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
public:
    unsigned advance(int64_t predicted,bool driving) {
        const auto elapsed=last?predicted-last:0;
        last=driving?predicted:0;
        if(!driving || elapsed<0 || elapsed>250000000){fraction=0;return 0;}
        fraction+=elapsed*14;
        const unsigned phases=unsigned(fraction/1000000000);
        fraction%=1000000000;
        return phases;
    }
};
class Drive {
public:
    explicit Drive(std::ostream& diagnostics);
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
