#pragma once
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

// Wait after presenting so the next input sample is taken close to the draw.
class DesktopFrameLimit {
    using Clock=std::chrono::steady_clock;
    Clock::duration interval{};
    Clock::time_point deadline{};
    HANDLE timer{};
public:
    explicit DesktopFrameLimit(unsigned fps) {
        if(!fps)return;
        interval=std::chrono::nanoseconds(1000000000/fps);
        timer=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
    }
    DesktopFrameLimit(const DesktopFrameLimit&)=delete;
    ~DesktopFrameLimit(){if(timer)CloseHandle(timer);}
    bool capped() const{return interval!=Clock::duration::zero();}
    void wait(){
        if(!capped())return;
        const auto now=Clock::now();
        if(deadline==Clock::time_point{})deadline=now+interval;
        if(now>=deadline){deadline=now+interval;return;}
        LARGE_INTEGER due{};
        due.QuadPart=-std::max<int64_t>(1,std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-now).count()/100);
        if(timer && SetWaitableTimer(timer,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(timer,INFINITE);
        else std::this_thread::sleep_until(deadline);
        deadline+=interval;
    }
};
