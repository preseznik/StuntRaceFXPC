#include "openxr_drive.hpp"
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <wrl/client.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace srf::vr {
using Microsoft::WRL::ComPtr;
namespace {
void require(XrResult result,const char* where) {
    if(XR_FAILED(result))throw std::runtime_error(std::string(where)+" (OpenXR "+std::to_string(result)+")");
}
constexpr XrPosef identity{{0,0,0,1},{0,0,0}};
constexpr auto stereo=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
template<class T,class F> auto enumerate(T initial,F f,const char* where) {
    uint32_t count{};
    require(f(0,&count,nullptr),where);
    std::vector<T> list(count,initial);
    if(count)require(f(count,&count,list.data()),where);
    list.resize(count);
    return list;
}
Eye scene_eye(const XrView& view) {
    const auto& [q,p]=view.pose;
    const auto& f=view.fov;
    return {{p.x,p.y,-p.z},{-q.x,-q.y,q.z,q.w},
            std::tan(f.angleLeft),std::tan(f.angleRight),std::tan(f.angleDown),std::tan(f.angleUp)};
}
XrPosef seat_origin(XrPosef head) {
    const auto q=head.orientation;
    const float yaw=std::atan2(2*(q.x*q.z+q.w*q.y),1-2*(q.x*q.x+q.y*q.y));
    head.orientation={0,std::sin(yaw/2),0,std::cos(yaw/2)};
    return head;
}
struct Images {
    XrSwapchain chain{};
    XrSwapchainSubImage area{};
    std::vector<XrSwapchainImageD3D11KHR> images;
    void create(XrSession session,int w,int h,DXGI_FORMAT format,bool hud) {
        XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        create.usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | (hud?XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT:0);
        create.format=format;create.sampleCount=create.faceCount=create.arraySize=create.mipCount=1;
        create.width=w;create.height=h;
        require(xrCreateSwapchain(session,&create,&chain),"Creating headset image");
        area={chain,{{0,0},{w,h}},0};
        images=enumerate(XrSwapchainImageD3D11KHR{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR},
            [&](auto n,auto* count,XrSwapchainImageD3D11KHR* out){return xrEnumerateSwapchainImages(chain,n,count,
                reinterpret_cast<XrSwapchainImageBaseHeader*>(out));},"Enumerating D3D11 headset images");
    }
    void write(const auto& draw) {
        XrSwapchainImageAcquireInfo a{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,nullptr,XR_INFINITE_DURATION};
        XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};uint32_t index{};
        require(xrAcquireSwapchainImage(chain,&a,&index),"Acquiring headset image");
        require(xrWaitSwapchainImage(chain,&w),"Waiting for headset image");
        try {draw(images.at(index).texture);} catch(...){xrReleaseSwapchainImage(chain,&r);throw;}
        require(xrReleaseSwapchainImage(chain,&r),"Releasing headset image");
    }
};
// Sample the two eye draws on the GPU timeline. Poll completed queries on
// later frames, without flushing the command queue or blocking the headset.
struct GpuFrameTime {
    ComPtr<ID3D11Query> disjoint,start,finish;
    bool pending{};
    uint64_t samples{};
    double total_ms{},worst_ms{};
    void initialize(ID3D11Device* device){
        D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP,0};
        if(FAILED(device->CreateQuery(&desc,&start)) || FAILED(device->CreateQuery(&desc,&finish)))return;
        desc.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;
        device->CreateQuery(&desc,&disjoint);
    }
    bool begin(ID3D11DeviceContext* ctx,uint64_t frame){
        if(!disjoint)return false;
        if(pending){
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};
            uint64_t a{},b{};
            constexpr UINT flags=D3D11_ASYNC_GETDATA_DONOTFLUSH;
            if(ctx->GetData(disjoint.Get(),&result,sizeof(result),flags)!=S_OK ||
               ctx->GetData(start.Get(),&a,sizeof(a),flags)!=S_OK || ctx->GetData(finish.Get(),&b,sizeof(b),flags)!=S_OK)return false;
            pending=false;
            if(!result.Disjoint && result.Frequency && b>=a){
                const double ms=1000.*double(b-a)/result.Frequency;
                ++samples;total_ms+=ms;worst_ms=std::max(worst_ms,ms);
            }
        }
        if(frame%16!=0)return false;
        ctx->Begin(disjoint.Get());ctx->End(start.Get());return true;
    }
    void end(ID3D11DeviceContext* ctx){
        ctx->End(finish.Get());ctx->End(disjoint.Get());pending=true;
    }
};
}
struct Drive::Impl {
    std::ostream& log;
    XrInstance instance{};XrSystemId system{};XrSession session{};
    XrSpace local{},seat{},head{};
    XrSessionState state=XR_SESSION_STATE_UNKNOWN;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    DXGI_FORMAT format{};
    Images eyes[2],panel;
    std::vector<uint32_t> panel_bgra;
    uint64_t panel_frames{};
    std::vector<uint32_t> last_hud;
    bool running=false,quit=false,recenter=true,seat_ready=false,was_panel=false;
    XrTime space_change{};
    uint64_t frames{},projections{},recenters{},missed_periods{};
    GpuFrameTime gpu_time;
    std::chrono::steady_clock::time_point last_submission{};
    double submission_seconds{};
    uint64_t submission_intervals{};
    uint64_t draw_ns{},long_draws{},peak_draw_ns{};
    XrTime last_time{};XrDuration period{};
    unsigned preferred_hz{},eye_scale_percent{};
    bool refresh_extension{};
    Impl(std::ostream& stream,unsigned hz,unsigned scale):log(stream),preferred_hz(hz),eye_scale_percent(scale){}
    ~Impl(){
        if(context)context->ClearState();
        for(auto* item:{&panel,&eyes[1],&eyes[0]})if(item->chain)xrDestroySwapchain(item->chain);
        for(auto space:{head,seat,local})if(space)xrDestroySpace(space);
        if(session)xrDestroySession(session);
        if(instance)xrDestroyInstance(instance);
        log<<"Frames "<<frames<<", stereo submissions "<<projections<<", recenters "<<recenters
           <<", skipped predicted display periods "<<missed_periods<<std::endl;
        if(panel_frames)log<<"Menu panel submissions "<<panel_frames<<std::endl;
        if(projections)log<<"App work before submission (includes image waits): mean "<<draw_ns/1e6/projections
            <<" ms, worst "<<peak_draw_ns/1e6<<" ms, over predicted period "<<long_draws<<std::endl;
        if(gpu_time.samples)log<<"Stereo GPU time ("<<gpu_time.samples<<" samples): mean "<<gpu_time.total_ms/gpu_time.samples
            <<" ms, worst "<<gpu_time.worst_ms<<" ms"<<std::endl;
        if(submission_seconds>0)log<<"Focused stereo submissions per wall-clock second "<<submission_intervals/submission_seconds<<std::endl;
    }
    XrSpace space(XrReferenceSpaceType type,XrPosef pose) {
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO,nullptr,type,pose};XrSpace result{};
        require(xrCreateReferenceSpace(session,&info,&result),"Creating seated reference space");return result;
    }
    void initialize() {
        if(eye_scale_percent<50 || eye_scale_percent>100 || preferred_hz>240)
            throw std::invalid_argument("Invalid headset refresh rate or eye resolution");
        const char* extensions[]={XR_KHR_D3D11_ENABLE_EXTENSION_NAME,XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME};
        if(preferred_hz){
            auto supported=enumerate(XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES},[](auto n,auto* c,XrExtensionProperties* p){
                return xrEnumerateInstanceExtensionProperties(nullptr,n,c,p);},"Reading OpenXR extensions");
            refresh_extension=std::ranges::any_of(supported,[&](const auto& p){return !std::strcmp(p.extensionName,extensions[1]);});
            if(!refresh_extension)log<<"Runtime controls refresh; display refresh rate extension unavailable (requested "<<preferred_hz<<" Hz)"<<std::endl;
        }
        XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
        std::strcpy(ci.applicationInfo.applicationName,"Stunt Race FX PC");
        ci.applicationInfo.apiVersion=XR_MAKE_VERSION(1,0,0);
        ci.enabledExtensionCount=refresh_extension?2:1;ci.enabledExtensionNames=extensions;
        require(xrCreateInstance(&ci,&instance),"Starting OpenXR. Connect the headset and enable a Windows D3D11 OpenXR runtime");
        XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
        require(xrGetInstanceProperties(instance,&props),"Reading runtime");log<<"Runtime "<<props.runtimeName<<std::endl;
        XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO,nullptr,XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
        require(xrGetSystem(instance,&si,&system),"Finding connected headset");
        auto modes=enumerate(XrEnvironmentBlendMode{},[&](auto n,auto* c,XrEnvironmentBlendMode* p){
            return xrEnumerateEnvironmentBlendModes(instance,system,stereo,n,c,p);},"Checking stereo display mode");
        if(std::find(modes.begin(),modes.end(),XR_ENVIRONMENT_BLEND_MODE_OPAQUE)==modes.end())
            throw std::runtime_error("A stereo headset with opaque display mode is required");
        PFN_xrGetD3D11GraphicsRequirementsKHR get{};
        require(xrGetInstanceProcAddr(instance,"xrGetD3D11GraphicsRequirementsKHR",reinterpret_cast<PFN_xrVoidFunction*>(&get)),"Getting D3D11 requirements");
        XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};require(get(instance,system,&req),"Reading headset GPU");
        ComPtr<IDXGIFactory1> factory;
        if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))throw std::runtime_error("DXGI factory unavailable");
        for(UINT i=0;;++i){
            ComPtr<IDXGIAdapter1> adapter;if(FAILED(factory->EnumAdapters1(i,&adapter)))break;
            DXGI_ADAPTER_DESC1 d{};if(FAILED(adapter->GetDesc1(&d)))continue;
            if(d.AdapterLuid.LowPart!=req.adapterLuid.LowPart || d.AdapterLuid.HighPart!=req.adapterLuid.HighPart)continue;
            const auto level=std::max(D3D_FEATURE_LEVEL_11_0,req.minFeatureLevel);
            if(SUCCEEDED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,&level,1,D3D11_SDK_VERSION,&device,nullptr,&context)))break;
        }
        if(!device)throw std::runtime_error("Cannot create D3D11 device on the headset runtime's GPU");
        gpu_time.initialize(device.Get());
        XrGraphicsBindingD3D11KHR bind{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR,nullptr,device.Get()};
        XrSessionCreateInfo sc{XR_TYPE_SESSION_CREATE_INFO,&bind,0,system};require(xrCreateSession(instance,&sc,&session),"Opening headset session");
        local=space(XR_REFERENCE_SPACE_TYPE_LOCAL,identity);seat=space(XR_REFERENCE_SPACE_TYPE_LOCAL,identity);head=space(XR_REFERENCE_SPACE_TYPE_VIEW,identity);
        auto config=enumerate(XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW},[&](auto n,auto* c,XrViewConfigurationView* p){
            return xrEnumerateViewConfigurationViews(instance,system,stereo,n,c,p);},"Reading stereo target sizes");
        if(config.size()!=2)throw std::runtime_error("The runtime did not provide two stereo views");
        auto formats=enumerate(int64_t{},[&](auto n,auto* c,int64_t* p){return xrEnumerateSwapchainFormats(session,n,c,p);},"Reading headset image formats");
        for(auto f:{DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM})
            if(std::find(formats.begin(),formats.end(),f)!=formats.end()){format=f;break;}
        if(!format)throw std::runtime_error("No supported headset color format");
        const auto scaled=[&](uint32_t size){return std::max(1u,(size*eye_scale_percent+50)/100);};
        for(int i=0;i<2;++i)eyes[i].create(session,scaled(config[i].recommendedImageRectWidth),scaled(config[i].recommendedImageRectHeight),format,false);
        log<<"Eye targets "<<eyes[0].area.imageRect.extent.width<<'x'<<eyes[0].area.imageRect.extent.height
           <<" and "<<eyes[1].area.imageRect.extent.width<<'x'<<eyes[1].area.imageRect.extent.height<<" at "<<eye_scale_percent<<"%"<<std::endl;
    }
    void request_refresh(){
        PFN_xrEnumerateDisplayRefreshRatesFB list{};
        PFN_xrRequestDisplayRefreshRateFB request{};
        auto r=xrGetInstanceProcAddr(instance,"xrEnumerateDisplayRefreshRatesFB",reinterpret_cast<PFN_xrVoidFunction*>(&list));
        if(XR_SUCCEEDED(r))r=xrGetInstanceProcAddr(instance,"xrRequestDisplayRefreshRateFB",reinterpret_cast<PFN_xrVoidFunction*>(&request));
        uint32_t count{};
        if(XR_SUCCEEDED(r))r=list(session,0,&count,nullptr);
        std::vector<float> rates(count);
        if(XR_SUCCEEDED(r) && count)r=list(session,count,&count,rates.data());
        if(XR_SUCCEEDED(r)){
            log<<"Runtime refresh choices (Hz):";for(auto rate:rates)log<<' '<<rate;log<<std::endl;
            const auto choice=std::ranges::find_if(rates,[&](float hz){return std::abs(hz-preferred_hz)<.1f;});
            if(choice==rates.end()){log<<"Requested "<<preferred_hz<<" Hz unavailable; using runtime rate"<<std::endl;return;}
            r=request(session,*choice);
        }
        log<<"Display refresh request "<<preferred_hz<<" Hz, XR result "<<int(r)<<std::endl;
    }
    void events(){
        for(;;){
            XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
            auto r=xrPollEvent(instance,&ev);if(r==XR_EVENT_UNAVAILABLE)break;
            require(r,"Reading OpenXR events");
            if(ev.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING){quit=true;return;}
            if(ev.type==XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING){
                const auto& changed=reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(ev);
                if(changed.referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL)space_change=changed.changeTime;
            }
            if(ev.type!=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)continue;
            state=reinterpret_cast<const XrEventDataSessionStateChanged&>(ev).state;
            log<<"Session state "<<int(state)<<std::endl;
            if(state==XR_SESSION_STATE_READY){
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO,nullptr,stereo};
                require(xrBeginSession(session,&begin),"Beginning stereo session");running=true;last_time=0;recenter=true;seat_ready=false;
                if(refresh_extension)request_refresh();
            }else if(state==XR_SESSION_STATE_STOPPING){
                require(xrEndSession(session),"Ending stereo session");running=false;
            }else if(state==XR_SESSION_STATE_EXITING || state==XR_SESSION_STATE_LOSS_PENDING)quit=true;
        }
    }
    void frame(const Update& update,const Draw& draw,std::span<const uint32_t> pixels={}){
        XrFrameState f{XR_TYPE_FRAME_STATE};XrFrameWaitInfo w{XR_TYPE_FRAME_WAIT_INFO};XrFrameBeginInfo b{XR_TYPE_FRAME_BEGIN_INFO};
        require(xrWaitFrame(session,&w,&f),"Waiting for headset frame");
        require(xrBeginFrame(session,&b),"Beginning headset frame");++frames;
        const auto started=std::chrono::steady_clock::now();
        XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,nullptr,f.predictedDisplayTime,XR_ENVIRONMENT_BLEND_MODE_OPAQUE};
        try {
            if(period!=f.predictedDisplayPeriod){period=f.predictedDisplayPeriod;log<<"Predicted display period "<<period<<" ns"<<std::endl;}
            if(last_time && period>0 && f.predictedDisplayTime-last_time>period*3/2)
                missed_periods+=(f.predictedDisplayTime-last_time+period/2)/period-1;
            last_time=f.predictedDisplayTime;
            if(space_change && f.predictedDisplayTime>=space_change){recenter=true;space_change=0;}
            // A runtime may expose a valid estimated pose before the headset is worn.
            // Seat the driver from a focused, tracked pose, including when leaving menus.
            if(was_panel && pixels.empty())recenter=true;
            was_panel=!pixels.empty();
            if(recenter && f.shouldRender && state==XR_SESSION_STATE_FOCUSED){
                XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};require(xrLocateSpace(head,local,f.predictedDisplayTime,&loc),"Locating seated head");
                const auto tracked=XR_SPACE_LOCATION_ORIENTATION_VALID_BIT|XR_SPACE_LOCATION_POSITION_VALID_BIT|
                                   XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT|XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
                if((loc.locationFlags&tracked)==tracked){
                    auto next=space(XR_REFERENCE_SPACE_TYPE_LOCAL,seat_origin(loc.pose));xrDestroySpace(seat);seat=next;seat_ready=true;recenter=false;++recenters;
                }
            }
            XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO,nullptr,stereo,f.predictedDisplayTime,seat};
            XrView views[2]={{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
            XrViewState vs{XR_TYPE_VIEW_STATE};uint32_t count{};
            if(f.shouldRender)require(xrLocateViews(session,&locate,&vs,2,&count,views),"Locating eyes at display time");
            const auto valid=XR_VIEW_STATE_POSITION_VALID_BIT|XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            const bool visible=seat_ready && f.shouldRender && count==2 && (vs.viewStateFlags&valid)==valid;
            auto display=update(f.predictedDisplayTime,visible && state==XR_SESSION_STATE_FOCUSED);
            recenter|=display.recenter;
            XrCompositionLayerProjectionView pv[2]={{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
            XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION,nullptr,0,seat,2,pv};
            XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD,nullptr,0,seat,XR_EYE_VISIBILITY_BOTH,{},
                                       {{0,0,0,1},{0,-.06f,-1.45f}},{1.6f,1.2f}};
            const XrCompositionLayerBaseHeader* layers[]={reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
                                                         reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
            const auto upload_panel=[&](std::span<const uint32_t> image) {
                if(image.size()!=256*224)throw std::runtime_error("Invalid headset panel size");
                if(!panel.chain)panel.create(session,256,224,format,true);
                if(format==DXGI_FORMAT_B8G8R8A8_UNORM || format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB){
                    panel_bgra.resize(image.size());
                    std::transform(image.begin(),image.end(),panel_bgra.begin(),[](uint32_t c){
                        return (c&0xff00ff00u)|((c&255)<<16)|((c>>16)&255);
                    });
                    image=panel_bgra;
                }
                panel.write([&](auto target){context->UpdateSubresource(target,0,nullptr,image.data(),256*4,0);});
                quad.subImage=panel.area;
            };
            if(visible && !pixels.empty()){
                upload_panel(pixels);last_hud.clear();
                quad.pose.position={0,0,-1.6f};
                end.layerCount=1;end.layers=layers+1;++panel_frames;
            }else if(visible){
                const bool timing=gpu_time.begin(context.Get(),projections);
                for(int i=0;i<2;++i){
                    pv[i].pose=views[i].pose;pv[i].fov=views[i].fov;pv[i].subImage=eyes[i].area;
                    auto eye=scene_eye(views[i]);eye.second=(i==1);
                    eyes[i].write([&](auto texture){draw(texture,format,eye);context->OMSetRenderTargets(0,nullptr,nullptr);});
                }
                if(timing)gpu_time.end(context.Get());
                end.layerCount=1;end.layers=layers;++projections;
                if(!display.race_hud.empty()){
                    if(!std::equal(last_hud.begin(),last_hud.end(),display.race_hud.begin(),display.race_hud.end())){
                        upload_panel(display.race_hud);
                        last_hud.assign(display.race_hud.begin(),display.race_hud.end());
                    }
                    quad.subImage=panel.area;
                    quad.layerFlags=XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
                    end.layerCount=2;
                }
                const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
                draw_ns+=ns;peak_draw_ns=std::max(peak_draw_ns,uint64_t(ns));long_draws+=ns>period;
            }
            // Layer structures must remain alive through xrEndFrame.
            auto result=xrEndFrame(session,&end);end.type=XR_TYPE_UNKNOWN;require(result,"Submitting stereo frame");
            if(visible && pixels.empty() && state==XR_SESSION_STATE_FOCUSED){
                const auto now=std::chrono::steady_clock::now();
                if(last_submission!=std::chrono::steady_clock::time_point{}){
                    submission_seconds+=std::chrono::duration<double>(now-last_submission).count();++submission_intervals;
                }
                last_submission=now;
            }else last_submission={};
        }catch(...){if(end.type!=XR_TYPE_UNKNOWN){end.layerCount=0;end.layers=nullptr;xrEndFrame(session,&end);}throw;}
    }
};
Drive::Drive(std::ostream& log,unsigned hz,unsigned scale):impl(std::make_unique<Impl>(log,hz,scale)){impl->initialize();}
Drive::~Drive()=default;
ID3D11Device* Drive::device() const{return impl->device.Get();}
bool Drive::focused() const {return impl->state==XR_SESSION_STATE_FOCUSED;}
bool Drive::step(const Update& update,const Draw& draw,std::span<const uint32_t> panel){
    impl->events();
    if(impl->quit)return false;
    if(impl->running)impl->frame(update,draw,panel);else Sleep(10);
    return true;
}
void Drive::run(const std::function<bool()>& pump,const Update& update,const Draw& draw){
    while(pump() && step(update,draw)){}
}
}
