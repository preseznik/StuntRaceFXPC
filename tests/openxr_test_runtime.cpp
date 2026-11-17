// Link-time OpenXR test double. This executable has no Khronos loader or HMD dependency.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace {
void ensure(bool test){if(!test)throw std::runtime_error("Invalid OpenXR frame or resource lifetime in test runtime");}
struct Chain {ComPtr<ID3D11Texture2D> color;int state{},released{};};
std::array<Chain,3> chains;
struct Space {XrPosef origin;};
ComPtr<ID3D11Device> gpu;
int chain_count{},events{},frame{},submissions{},recenter_count{};
bool waited{},begun{},session_running{};
constexpr XrPosef identity{{0,0,0,1},{0,0,0}};
template<class T> XrResult one(T value,uint32_t capacity,uint32_t* count,T* out){
    *count=1;if(capacity)out[0]=value;return XR_SUCCESS;
}
XrTime display_time(){return 1000000000LL+frame*11111111LL;}
XrPosef physical_head(){
    const float yaw=frame<=1?.4f:.7f;
    return {{0,std::sin(yaw/2),0,std::cos(yaw/2)},{.25f+(frame<=1?0.f:.12f),1.2f,-.1f}};
}
XrResult XRAPI_CALL requirements(XrInstance,XrSystemId,XrGraphicsRequirementsD3D11KHR* out){
    ComPtr<IDXGIFactory1> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
    ensure(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0,&adapter)) && SUCCEEDED(adapter->GetDesc1(&desc)));
    out->adapterLuid=desc.AdapterLuid;out->minFeatureLevel=D3D_FEATURE_LEVEL_11_0;return XR_SUCCESS;
}
}
extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* info,XrInstance* out){
    ensure(info->enabledExtensionCount==1 && !strcmp(info->enabledExtensionNames[0],XR_KHR_D3D11_ENABLE_EXTENSION_NAME));
    *out=reinterpret_cast<XrInstance>(1);return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProperties(XrInstance,XrInstanceProperties* p){strcpy(p->runtimeName,"Windowless test runtime");return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystem(XrInstance,const XrSystemGetInfo*,XrSystemId* p){*p=1;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance,const char* name,PFN_xrVoidFunction* p){
    ensure(!strcmp(name,"xrGetD3D11GraphicsRequirementsKHR"));*p=reinterpret_cast<PFN_xrVoidFunction>(requirements);return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateEnvironmentBlendModes(XrInstance,XrSystemId,XrViewConfigurationType,uint32_t n,uint32_t* c,XrEnvironmentBlendMode* p){return one(XR_ENVIRONMENT_BLEND_MODE_OPAQUE,n,c,p);}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSession(XrInstance,const XrSessionCreateInfo* info,XrSession* out){
    gpu=static_cast<const XrGraphicsBindingD3D11KHR*>(info->next)->device;*out=reinterpret_cast<XrSession>(1);return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession,const XrReferenceSpaceCreateInfo* info,XrSpace* p){
    *p=reinterpret_cast<XrSpace>(new Space{info->poseInReferenceSpace});
    if(info->referenceSpaceType==XR_REFERENCE_SPACE_TYPE_LOCAL && info->poseInReferenceSpace.position.y!=0){
        ensure(std::abs(info->poseInReferenceSpace.position.y-1.2f)<.001f);++recenter_count;
    }return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace s){delete reinterpret_cast<Space*>(s);return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateViewConfigurationViews(XrInstance,XrSystemId,XrViewConfigurationType,uint32_t n,uint32_t* c,XrViewConfigurationView* p){
    *c=2;if(n)for(int i=0;i<2;++i){p[i].recommendedImageRectWidth=768;p[i].recommendedImageRectHeight=512;}return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession,uint32_t n,uint32_t* c,int64_t* p){return one<int64_t>(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,n,c,p);}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession,const XrSwapchainCreateInfo* info,XrSwapchain* out){
    auto& c=chains.at(chain_count++);D3D11_TEXTURE2D_DESC d{};d.Width=info->width;d.Height=info->height;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
    d.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    ensure(SUCCEEDED(gpu->CreateTexture2D(&d,nullptr,&c.color)));*out=reinterpret_cast<XrSwapchain>(&c);return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain s,uint32_t n,uint32_t* c,XrSwapchainImageBaseHeader* p){
    *c=1;if(n)reinterpret_cast<XrSwapchainImageD3D11KHR*>(p)->texture=reinterpret_cast<Chain*>(s)->color.Get();return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain s,const XrSwapchainImageAcquireInfo*,uint32_t* p){
    auto& c=*reinterpret_cast<Chain*>(s);ensure(begun && c.state==0);c.state=1;*p=0;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain s,const XrSwapchainImageWaitInfo*){auto& c=*reinterpret_cast<Chain*>(s);ensure(c.state==1);c.state=2;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain s,const XrSwapchainImageReleaseInfo*){auto& c=*reinterpret_cast<Chain*>(s);ensure(c.state==2);c.state=0;++c.released;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrPollEvent(XrInstance,XrEventDataBuffer* b){
    XrSessionState state;
    if(events==0)state=XR_SESSION_STATE_READY;
    else if(events==1)state=XR_SESSION_STATE_FOCUSED;
    else if(frame==6 && events==2)state=XR_SESSION_STATE_STOPPING;
    else if(frame==6 && events==3)state=XR_SESSION_STATE_EXITING;
    else return XR_EVENT_UNAVAILABLE;
    auto* event=reinterpret_cast<XrEventDataSessionStateChanged*>(b);event->type=XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;event->state=state;++events;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrBeginSession(XrSession,const XrSessionBeginInfo*){session_running=true;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrEndSession(XrSession){ensure(session_running && !begun);session_running=false;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitFrame(XrSession,const XrFrameWaitInfo*,XrFrameState* f){
    ensure(session_running && !waited && !begun && frame<6);++frame;waited=true;
    f->predictedDisplayTime=display_time();f->predictedDisplayPeriod=11111111;f->shouldRender=frame!=4;return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrBeginFrame(XrSession,const XrFrameBeginInfo*){ensure(waited && !begun);waited=false;begun=true;return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace,XrSpace,XrTime time,XrSpaceLocation* l){
    ensure(time==display_time());l->locationFlags=XR_SPACE_LOCATION_POSITION_VALID_BIT|XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;l->pose=physical_head();return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession,const XrViewLocateInfo* info,XrViewState* state,uint32_t n,uint32_t* count,XrView* views){
    ensure(info->displayTime==display_time() && n==2);*count=2;
    state->viewStateFlags=XR_VIEW_STATE_ORIENTATION_VALID_BIT;if(frame!=5)state->viewStateFlags|=XR_VIEW_STATE_POSITION_VALID_BIT;
    const auto& origin=reinterpret_cast<Space*>(info->space)->origin;
    auto head=physical_head();
    const float yaw=2*(std::atan2(head.orientation.y,head.orientation.w)-std::atan2(origin.orientation.y,origin.orientation.w));
    for(int i=0;i<2;++i){
        const float side=i?.032f:-.032f;
        views[i].pose={{0,std::sin(yaw/2),0,std::cos(yaw/2)},{head.position.x-origin.position.x+side*std::cos(yaw),0,-side*std::sin(yaw)}};
        views[i].fov={-.76f,.76f,.70f,-.65f};
    }return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession,const XrFrameEndInfo* end){
    ensure(begun && end->displayTime==display_time());begun=false;
    if(frame==4 || frame==5){ensure(end->layerCount==0);return XR_SUCCESS;}
    ensure(end->layerCount==2);++submissions;
    auto* p=reinterpret_cast<const XrCompositionLayerProjection*>(end->layers[0]);
    auto* h=reinterpret_cast<const XrCompositionLayerQuad*>(end->layers[1]);
    ensure(p->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION && p->viewCount==2 && h->type==XR_TYPE_COMPOSITION_LAYER_QUAD && h->space==p->space);
    ensure(h->pose.position.y<0 && h->pose.position.z<-1 && h->eyeVisibility==XR_EYE_VISIBILITY_BOTH);
    ensure(p->views[0].pose.position.x<p->views[1].pose.position.x && h->subImage.swapchain==reinterpret_cast<XrSwapchain>(&chains[2]));
    for(int i=0;i<2;++i)ensure(p->views[i].subImage.swapchain==reinterpret_cast<XrSwapchain>(&chains[i]) && chains[i].released==submissions);
    ensure(chains[2].released>0);return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain s){auto c=reinterpret_cast<Chain*>(s);ensure(!c->state);c->color.Reset();return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySession(XrSession){gpu.Reset();return XR_SUCCESS;}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance){return XR_SUCCESS;}
}
void verify_test_runtime(){ensure(!session_running && submissions==4 && recenter_count==2 && !waited && !begun);}
ID3D11Texture2D* test_hud_texture(){return chains[2].color.Get();}
