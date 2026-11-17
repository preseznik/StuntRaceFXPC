#include "srf/vehicle_math.hpp"

namespace srf {
std::int16_t project_speed(const std::array<std::int16_t,3>& velocity,
                          const std::array<std::int16_t,3>& coefficients) {
    std::uint32_t sum=0;
    for(unsigned axis=0;axis<3;++axis) {
        const std::int32_t product=std::int32_t(velocity[axis])*coefficients[axis];
        // The original FMULT followed by ROL keeps product bits 15..30.
        // Use unsigned shifts and an explicit mask to avoid host signed-shift
        // and out-of-range signed-conversion assumptions.
        sum+=(static_cast<std::uint32_t>(product)>>15)&0xffffu;
    }
    sum&=0xffffu;
    int result=sum>=0x8000 ? int(sum)-0x10000 : int(sum);
    for(unsigned i=0;i<3;++i) {
        // GSU DIV2 rounds negative odd values downward, except -1 becomes 0.
        // Three instructions therefore cannot be replaced with C++ / 8.
        result=result==-1 ? 0 : result<0 ? (result-1)/2 : result/2;
    }
    return static_cast<std::int16_t>(result);
}
}
