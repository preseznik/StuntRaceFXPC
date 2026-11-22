// Native D3D11 scene rendering and development race presentation.
#include "srf/rom.hpp"
#include "srf/mesh.hpp"
#include "srf/world.hpp"
#include "srf/native_race.hpp"
#include "srf/audio_device.hpp"
#include "srf/gamepad.hpp"
#include "openxr_drive.hpp"
#include "desktop_frame_limit.hpp"
#include "srf/game_frontend.hpp"
#include "srf/speed_bonus.hpp"
#include "srf/presentation.hpp"
#include "srf/scenery.hpp"
#include "srf/environment.hpp"
#include "srf/backdrop.hpp"
#include "srf/rain.hpp"
#include "srf/rock_field_session.hpp"
#include "generated_manifest.hpp"
#include "generated_courses.hpp"
#include "generated_world.hpp"
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(hr) + ")");
}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot read generated asset file");
    const auto length = stream.tellg();
    if (length < 0 || length > 128 * 1024 * 1024) throw std::runtime_error("Invalid generated asset size");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    stream.seekg(0);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()), length)) throw std::runtime_error("Incomplete package read");
    return bytes;
}
struct GpuVertex {
    float x,y,z,r,g,b,u=0,v=0,texture=0,painter_depth=0;
    std::array<float,4> road_origin_bias{};
};
struct RenderSprite {
    unsigned table=0,phase=0;
    std::array<float,3> position{};
    float diameter=0;
};
struct RenderVehicle {
    unsigned descriptor=0;
    std::array<float,3> position{};
    std::array<float,9> orientation{1,0,0,0,1,0,0,0,1};
    std::vector<RenderSprite> sprites;
};
struct SceneTarget {
    UINT width{},height{};
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
};
struct SceneEye {
    // Relative to the game camera: +X right, +Y up, +Z forward. Offsets use
    // normalized scene units. Frustum edges are signed tangents at unit depth.
    std::array<float,3> offset{};
    std::array<float,4> orientation{0,0,0,1}; // eye-to-camera quaternion, xyzw
    float left=-1,right=1,down=-1,up=1;
};
struct Constants {
    float cy,sy,cp,sp,aspect,zoom,distance,road_pass;
    float tx,ty,tz,eye_enabled;
    std::array<float,4> eye_offset{},eye_right{},eye_up{},eye_forward{},projection{};
    void set_eye(const SceneEye& eye) {
        const auto [x,y,z,w]=eye.orientation;
        if(!(std::abs(x*x+y*y+z*z+w*w-1)<.001f) ||
           !std::isfinite(eye.left) || !std::isfinite(eye.right) || !(eye.left<eye.right) ||
           !std::isfinite(eye.down) || !std::isfinite(eye.up) || !(eye.down<eye.up) ||
           !std::ranges::all_of(eye.offset,[](float v){return std::isfinite(v);}))
            throw std::invalid_argument("Invalid eye pose or projection");
        eye_enabled=1;
        std::copy(eye.offset.begin(),eye.offset.end(),eye_offset.begin());
        eye_right={1-2*(y*y+z*z),2*(x*y+w*z),2*(x*z-w*y),0};
        eye_up={2*(x*y-w*z),1-2*(x*x+z*z),2*(y*z+w*x),0};
        eye_forward={2*(x*z+w*y),2*(y*z-w*x),1-2*(x*x+y*y),0};
        projection={2/(eye.right-eye.left),2/(eye.up-eye.down),
                    -(eye.right+eye.left)/(eye.right-eye.left),-(eye.up+eye.down)/(eye.up-eye.down)};
    }
};
struct BackdropConstants {
    float left{},top{},span_x{},span_y{};
    unsigned layer{},blank{},fixed_rgb555{},color_math_layers{};
    unsigned gradient_y{},sky_rgb555{},texture_weight{};
    float rows_per_pixel{};
    std::array<unsigned,128> row_subtract{};
    unsigned indexed_textures{},add_fixed{},padding[2]{};
};
struct Controls {
    std::size_t model=2, frame=0;
    float yaw=.65f, pitch=-.45f, zoom=2.1f;
    bool wire=false, spin=false, dirty=true, resize=false, dragging=false, panning=false, world=false;
    float target_x=0,target_y=0,target_z=0,distance=3.f;
    int mouse_x=0, mouse_y=0;
    unsigned width=1280, height=720;
    std::size_t count=0;
    bool race=false,restart=false,paused=false,recenter=false;
};
Controls controls;
unsigned desktop_fps=0;
HWND race_hud=nullptr;
bool race_hud_visible=true,grid_instruments=false;
HBRUSH hud_background=nullptr;
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM key, LPARAM data) {
    switch(message) {
    case WM_CTLCOLORSTATIC:
        if(reinterpret_cast<HWND>(data)==race_hud) {
            SetTextColor(reinterpret_cast<HDC>(key),RGB(235,241,245));
            SetBkColor(reinterpret_cast<HDC>(key),RGB(12,20,27));
            return reinterpret_cast<LRESULT>(hud_background);
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE:
        if (LOWORD(data) && HIWORD(data)) { controls.width=LOWORD(data); controls.height=HIWORD(data); controls.resize=true;
            if(race_hud) MoveWindow(race_hud,12,12,std::max(200,int(controls.width)-24),112,TRUE);
        }
        return 0;
    case WM_KEYDOWN:
        if (key==VK_ESCAPE) { DestroyWindow(window); return 0; }
        if(key==VK_F11 && !(data&(1L<<30))) {
            static WINDOWPLACEMENT placement{};placement.length=sizeof(WINDOWPLACEMENT);
            const auto style=GetWindowLongPtrW(window,GWL_STYLE);
            if(style&WS_OVERLAPPEDWINDOW) {
                MONITORINFO monitor{};monitor.cbSize=sizeof(MONITORINFO);
                if(GetWindowPlacement(window,&placement) && GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor)) {
                    SetWindowLongPtrW(window,GWL_STYLE,style&~WS_OVERLAPPEDWINDOW);
                    SetWindowPos(window,HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,
                        monitor.rcMonitor.right-monitor.rcMonitor.left,monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_FRAMECHANGED);
                }
            } else {
                SetWindowLongPtrW(window,GWL_STYLE,style|WS_OVERLAPPEDWINDOW);SetWindowPlacement(window,&placement);
                SetWindowPos(window,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_FRAMECHANGED);
            }
            return 0;
        }
        if(controls.race) {
            if(key=='H' && race_hud && !(data&(1L<<30))) {
                race_hud_visible=!race_hud_visible;
                ShowWindow(race_hud,race_hud_visible && !grid_instruments?SW_SHOWNA:SW_HIDE);
            }
            if(key==VK_F2 && !(data&(1L<<30))) controls.recenter=true;
            if(key=='R' && !(data&(1L<<30))) controls.restart=true;
            if(key=='P' && !(data&(1L<<30))) controls.paused=!controls.paused;
            if(key==VK_TAB && !(data&(1L<<30))) controls.wire=!controls.wire;
            return 0;
        }
        if (!controls.world && key==VK_RIGHT && controls.count) { controls.model=(controls.model+1)%controls.count; controls.frame=0; }
        if (!controls.world && key==VK_LEFT && controls.count) { controls.model=(controls.model+controls.count-1)%controls.count; controls.frame=0; }
        if (key=='F') ++controls.frame;
        if ((key=='W' && !controls.world) || key==VK_TAB) controls.wire=!controls.wire;
        if (key==VK_SPACE) controls.spin=!controls.spin;
        if (key==VK_HOME) { controls.yaw=controls.world?0.f:.65f; controls.pitch=controls.world?-1.1f:-.45f; controls.zoom=2.1f; controls.target_x=controls.target_y=controls.target_z=0; }
        controls.dirty=true; return 0;
    case WM_LBUTTONDOWN:
        if(controls.race) return 0;
        controls.dragging=true; controls.mouse_x=GET_X_LPARAM(data); controls.mouse_y=GET_Y_LPARAM(data); SetCapture(window); return 0;
    case WM_LBUTTONUP: controls.dragging=false; ReleaseCapture(); return 0;
    case WM_RBUTTONDOWN:
        if(controls.race) return 0;
        controls.panning=true; controls.mouse_x=GET_X_LPARAM(data); controls.mouse_y=GET_Y_LPARAM(data); SetCapture(window); return 0;
    case WM_RBUTTONUP: controls.panning=false; ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: controls.dragging=controls.panning=false; return 0;
    case WM_MOUSEMOVE:
        if (controls.dragging) {
            controls.yaw+=(GET_X_LPARAM(data)-controls.mouse_x)*.008f;
            controls.pitch+=(GET_Y_LPARAM(data)-controls.mouse_y)*.008f;
            controls.mouse_x=GET_X_LPARAM(data); controls.mouse_y=GET_Y_LPARAM(data);
        }
        if (controls.panning) {
            const float dx=(GET_X_LPARAM(data)-controls.mouse_x)*.006f/controls.zoom;
            const float dy=(GET_Y_LPARAM(data)-controls.mouse_y)*.006f/controls.zoom;
            controls.target_x-=dx*std::cos(controls.yaw)+dy*std::sin(controls.yaw);
            controls.target_z-=dx*std::sin(controls.yaw)-dy*std::cos(controls.yaw);
            controls.mouse_x=GET_X_LPARAM(data); controls.mouse_y=GET_Y_LPARAM(data);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if(controls.race) return 0;
        controls.zoom=std::clamp(controls.zoom*std::pow(1.15f,GET_WHEEL_DELTA_WPARAM(key)/120.f),.3f,controls.world?80.f:8.f); return 0;
    }
    return DefWindowProcW(window,message,key,data);
}

class Renderer {
    DesktopFrameLimit frame_limit;
    void present() {
        if(!swapchain)return;
        check(swapchain->Present(frame_limit.capped()?0:1,0),"Present");
        frame_limit.wait();
    }
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapchain;
    SceneTarget desktop;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> vertices, constants;
    ComPtr<ID3D11RasterizerState> solid, wire, sprite_solid;
    ComPtr<ID3D11DepthStencilState> reverse_depth;
    ComPtr<ID3D11BlendState> depth_only;
    ComPtr<ID3D11ShaderResourceView> texture_view, dusk_texture_view, palette_indices_view;
    const srf::generated::CoursePackage* course_colors = &srf::generated::course_packages.front();
    srf::WorldPalette world_palette = course_colors->day_palette;
    ComPtr<ID3D11ShaderResourceView> backdrop_indices,backdrop_colors;
    ComPtr<ID3D11Texture2D> backdrop_color_texture;
    ComPtr<ID3D11VertexShader> backdrop_vs;
    ComPtr<ID3D11PixelShader> backdrop_ps;
    ComPtr<ID3D11PixelShader> rain_ps;
    ComPtr<ID3D11Texture2D> rain_mask;
    ComPtr<ID3D11ShaderResourceView> rain_view;
    bool show_rain=false;
    bool headset_output=false;
    ComPtr<ID3D11PixelShader> bonus_overlay_ps;
    ComPtr<ID3D11Buffer> bonus_overlay_constants;
    ComPtr<ID3D11BlendState> bonus_overlay_blend;
    ComPtr<ID3D11Texture2D> bonus_overlay_texture;
    ComPtr<ID3D11ShaderResourceView> bonus_overlay_view;
    ComPtr<ID3D11Texture2D> course_map_texture;
    ComPtr<ID3D11ShaderResourceView> course_map_view;
    ComPtr<ID3D11PixelShader> course_map_ps;
    bool bonus_has_overlay=false,bonus_has_dashboard=false;
    unsigned bonus_brightness=15;
    float bonus_scene_weight=1.f,bonus_corner_rows=40.f;
    unsigned bonus_clip_top=0,bonus_clip_bottom=224;
    ComPtr<ID3D11Buffer> backdrop_constants;
    srf::CourseBackdrop backdrop;
    srf::EnvironmentPresentation environment;
    unsigned backdrop_weight=UINT_MAX, backdrop_fog=UINT_MAX, backdrop_fog_rgb555=UINT_MAX, skyline_phase=UINT_MAX, skyline_blend_phase=UINT_MAX;
    ComPtr<ID3D11SamplerState> texture_sampler;
    struct TextureLocation { unsigned layer,width,height; std::array<std::array<std::uint8_t,2>,4> uv; };
    std::map<std::pair<unsigned,unsigned>,TextureLocation> texture_locations;
    std::array<bool,256> textured_materials{};
    std::map<std::pair<unsigned,std::size_t>,std::vector<srf::SurfaceOffset>> surface_offsets;
    std::map<std::pair<unsigned,std::size_t>,std::vector<bool>> barrier_backs;
    UINT vertex_count=0,surface_vertex_count=0,world_vertex_count=0,line_count=0,world_line_count=0;
    bool vehicle_depth_pass=false;
    std::array<double,3> world_center{};
    double world_radius=1;
    void upload(const std::vector<GpuVertex>& triangles,const std::vector<GpuVertex>& lines={},UINT surface_count=UINT_MAX) {
        std::vector<GpuVertex> data=triangles; data.insert(data.end(),lines.begin(),lines.end());
        if(data.empty()) throw std::runtime_error("Model has no renderable primitives");
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=static_cast<UINT>(data.size()*sizeof(GpuVertex));
        desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{data.data(),0,0};
        check(device->CreateBuffer(&desc,&initial,vertices.ReleaseAndGetAddressOf()),"Geometry buffer");
        vertex_count=static_cast<UINT>(triangles.size()); line_count=static_cast<UINT>(lines.size());
        surface_vertex_count=std::min(surface_count,vertex_count);
    }
public:
    Renderer(HWND window,ID3D11Device* headset_device=nullptr,unsigned fps=0):frame_limit(fps) {
        headset_output=headset_device!=nullptr;
        D3D_FEATURE_LEVEL level;
        const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_0};
        HRESULT hr=E_FAIL;
        if(headset_device){device=headset_device;device->GetImmediateContext(&context);hr=S_OK;}
        else for (auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            if (window) {
                DXGI_SWAP_CHAIN_DESC desc{};
                desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
                desc.BufferCount=2; desc.OutputWindow=window; desc.Windowed=TRUE;
                desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
                hr=D3D11CreateDeviceAndSwapChain(nullptr,driver,nullptr,0,requested,1,D3D11_SDK_VERSION,
                    &desc,swapchain.ReleaseAndGetAddressOf(),device.ReleaseAndGetAddressOf(),&level,context.ReleaseAndGetAddressOf());
            } else {
                hr=D3D11CreateDevice(nullptr,driver,nullptr,0,requested,1,D3D11_SDK_VERSION,
                    device.ReleaseAndGetAddressOf(),&level,context.ReleaseAndGetAddressOf());
            }
            if (SUCCEEDED(hr)) break;
        }
        check(hr,"D3D11 device");
        constexpr char shader[]=R"(
cbuffer View : register(b0) { float cy,sy,cp,sp,aspect,zoom,distance,road_pass; float3 target; float eye_enabled;
    float4 eye_offset,eye_right,eye_up,eye_forward,projection; };
cbuffer Environment : register(b1) { float4 crop; uint layer,blank,fixed_color,math_layers; uint gradient_y,sky_color,texture_weight; float rows_per_pixel; uint4 row_subtract[32]; uint indexed_textures,add_fixed; };
Texture2DArray<float4> tiles:register(t0);
Texture2DArray<float4> dusk_tiles:register(t3);
Texture2DArray<uint> palette_indices:register(t4);
Texture2D<uint> colors:register(t2);
SamplerState tile_sampler:register(s0);
struct Vertex { float3 position:POSITION; float3 color:COLOR; float3 uv:TEXCOORD; float painter_depth:TEXCOORD1; float4 road:TEXCOORD2; };
struct Pixel { float4 position:SV_POSITION; float3 color:COLOR; float3 uv:TEXCOORD; };
float3 camera_position(float3 position) {
    float3 p=position-target;
    p=float3(cy*p.x+sy*p.z,p.y,-sy*p.x+cy*p.z);
    p=float3(p.x,cp*p.y-sp*p.z,sp*p.y+cp*p.z);
    p.z+=distance;
    return p;
}
Pixel vertex_main(Vertex v) {
    float3 p=camera_position(v.position);
    // Infinite-far reversed Z retains precision on the original, very large
    // course coordinates. Geometry and physics remain unchanged.
    Pixel o; o.position=float4(p.x*zoom/aspect,p.y*zoom,.0005,p.z);
    // Only the depth rebuild uses original road-object priority. Screen position
    // is identical; walls and every dynamic vertex retain ordinary geometric Z.
    float painter=v.painter_depth;
    if(eye_enabled) {
        float3 q=p-eye_offset.xyz;
        p=float3(dot(q,eye_right.xyz),dot(q,eye_up.xyz),dot(q,eye_forward.xyz));
        o.position=float4(p.xy*projection.xy+p.z*projection.zw,.0005,p.z);
        if(road_pass && painter!=0) {
            float3 origin=camera_position(v.road.xyz)-eye_offset.xyz;
            float eye_y=target.y-distance*sp+eye_offset.y*cp+eye_offset.z*sp;
            painter=dot(origin,eye_forward.xyz)+2*abs(v.road.y-eye_y)+v.road.w;
        }
    }
    if(road_pass!=0 && painter>0) o.position.z=.0005*p.z/painter;
    o.color=v.color; o.uv=v.uv; return o;
}
float4 pixel_main(Pixel p):SV_TARGET {
    float4 color=tiles.Sample(tile_sampler,p.uv);
    clip(color.a-.5);
    if(indexed_textures && p.uv.z != 0) {
        // Match Texture2DArray.Sample's slice rounding even at oblique angles.
        uint index=palette_indices.Load(int4(int2(p.uv.xy*128),int(round(p.uv.z)),0));
        uint packed=colors.Load(int3(528+index,0,0));
        color.rgb=float3(packed&31,(packed>>5)&31,(packed>>10)&31)/31;
    } else if(texture_weight && p.uv.z != 0) {
        float3 day=round(color.rgb*31);
        float3 dusk=round(dusk_tiles.Sample(tile_sampler,p.uv).rgb*31);
        color.rgb=(day+floor((dusk-day)*texture_weight/32.0))/31.0;
    }
    float3 rgb=color.rgb*p.color;
    if((math_layers&1) || rows_per_pixel>0) {
        uint color_word=fixed_color;
        if(rows_per_pixel>0) {
            uint row=min(127u,(uint)(p.position.y*rows_per_pixel));
            color_word=row_subtract[row/4][row%4];
        }
        float3 fixed_rgb=float3(color_word&31,(color_word>>5)&31,(color_word>>10)&31);
        rgb=clamp(round(rgb*31)+(add_fixed?fixed_rgb:-fixed_rgb),0,31)/31;
    }
#ifdef XR_COLOR
    // Original palette values are display-encoded. The compositor expects linear scene color.
    rgb=lerp(rgb/12.92,pow((rgb+.055)/1.055,2.4),step(.04045,rgb));
#endif
    return float4(rgb,1);
}
)";
        const D3D_SHADER_MACRO xr_color[]={{"XR_COLOR","1"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> vertex_blob,pixel_blob,errors;
        check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"vertex_main","vs_5_0",0,0,&vertex_blob,&errors),"Vertex shader");
        check(D3DCompile(shader,sizeof(shader),nullptr,headset_output?xr_color:nullptr,nullptr,"pixel_main","ps_5_0",0,0,&pixel_blob,&errors),"Pixel shader");
        check(device->CreateVertexShader(vertex_blob->GetBufferPointer(),vertex_blob->GetBufferSize(),nullptr,&vs),"Vertex shader creation");
        check(device->CreatePixelShader(pixel_blob->GetBufferPointer(),pixel_blob->GetBufferSize(),nullptr,&ps),"Pixel shader creation");
        constexpr char background_shader[]=R"(
cbuffer View : register(b0) { float cy,sy,cp,sp,aspect,zoom,distance,road_pass; float3 target; float eye_enabled;
    float4 eye_offset,eye_right,eye_up,eye_forward,projection; };
cbuffer Environment : register(b1) { float4 crop; uint layer,blank,fixed_color,math_layers; uint gradient_y,sky_color,pad2a,pad2b; uint4 row_subtract[32]; uint indexed_textures,add_fixed; };
Texture2DArray<uint> indices:register(t1);
Texture2D<uint> colors:register(t2);
Texture2D<uint> rain_mask:register(t5);
struct Pixel {float4 position:SV_POSITION;float2 uv:TEXCOORD;};
Pixel background_vertex(uint id:SV_VertexID) {
    Pixel p;p.uv=float2((id<<1)&2,id&2);
    p.position=float4(p.uv*float2(2,-2)+float2(-1,1),0,1);return p;
}
float4 background_pixel(Pixel p):SV_TARGET {
    int x=(int)floor(crop.x+p.uv.x*crop.z);
    int y=(int)floor(crop.y+p.uv.y*crop.w);
    int ramp=min(511,gradient_y+(int)floor(p.uv.y*crop.w));
    if(eye_enabled) {
        // The backdrop is at infinity. Both eyes sample the same direction
        // regardless of translation, including for asymmetric eye frusta.
        float2 ndc=p.uv*float2(2,-2)+float2(-1,1);
        float2 ray=(ndc-projection.zw)/projection.xy;
        float3 dir=normalize(eye_right.xyz*ray.x+eye_up.xyz*ray.y+eye_forward.xyz);
        float flat_z=cp*dir.z-sp*dir.y;
        float yaw=atan2(cy*dir.x-sy*flat_z,sy*dir.x+cy*flat_z);
        float pitch=asin(clamp(cp*dir.y+sp*dir.z,-1,1));
        x=104+(int)floor(yaw*(1024/6.28318530718));
        y=217-(int)floor(pitch*(1024/6.28318530718));
        ramp=clamp(y+32,0,511);
    }
    uint index=(y<0 || y>=256)?1:indices.Load(int4(x&1023,y,layer,0));
    uint value=index==0?sky_color:blank?0:colors.Load(int3(index==1?16+ramp:index,0,0));
    float3 rgb=float3(value&31,(value>>5)&31,(value>>10)&31);
    if(index!=0 && (math_layers&2)) {
        float3 fixed_rgb=float3(fixed_color&31,(fixed_color>>5)&31,(fixed_color>>10)&31);
        rgb=clamp(rgb+(add_fixed?fixed_rgb:-fixed_rgb),0,31);
    }
    rgb/=31;
#ifdef XR_COLOR
    rgb=lerp(rgb/12.92,pow((rgb+.055)/1.055,2.4),step(.04045,rgb));
#endif
    return float4(rgb,1);
}
float4 rain_pixel(Pixel p):SV_TARGET {
    // Keep original pixels square on widescreen and extend the pattern to
    // the edges without stretching the 208 x 128 viewport.
    int x=(int)floor(104+(p.uv.x-.5)*crop.z);
    int y=(int)floor(p.uv.y*128);
    if(!rain_mask.Load(int3((x%208+208)%208,y,0)))discard;
    uint value=colors.Load(int3(528+42,0,0));
    float3 rgb=float3(value&31,(value>>5)&31,(value>>10)&31);
    if(math_layers&1) {
        float3 fixed_rgb=float3(fixed_color&31,(fixed_color>>5)&31,(fixed_color>>10)&31);
        rgb=clamp(rgb+(add_fixed?fixed_rgb:-fixed_rgb),0,31);
    }
    return float4(rgb/31,1);
}
)";
        ComPtr<ID3DBlob> backdrop_vertex_blob,backdrop_pixel_blob;
        check(D3DCompile(background_shader,sizeof(background_shader),nullptr,nullptr,nullptr,"background_vertex","vs_5_0",0,0,&backdrop_vertex_blob,&errors),"Backdrop vertex shader");
        check(device->CreateVertexShader(backdrop_vertex_blob->GetBufferPointer(),backdrop_vertex_blob->GetBufferSize(),nullptr,&backdrop_vs),"Backdrop vertex shader creation");
        check(D3DCompile(background_shader,sizeof(background_shader),nullptr,headset_output?xr_color:nullptr,nullptr,"background_pixel","ps_5_0",0,0,&backdrop_pixel_blob,&errors),"Backdrop pixel shader");
        check(device->CreatePixelShader(backdrop_pixel_blob->GetBufferPointer(),backdrop_pixel_blob->GetBufferSize(),nullptr,&backdrop_ps),"Backdrop pixel shader creation");
        ComPtr<ID3DBlob> rain_blob;
        check(D3DCompile(background_shader,sizeof(background_shader),nullptr,nullptr,nullptr,"rain_pixel","ps_5_0",0,0,&rain_blob,&errors),"Rain shader");
        check(device->CreatePixelShader(rain_blob->GetBufferPointer(),rain_blob->GetBufferSize(),nullptr,&rain_ps),"Rain shader creation");
        const D3D11_INPUT_ELEMENT_DESC elements[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32B32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32_FLOAT,0,36,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,40,D3D11_INPUT_PER_VERTEX_DATA,0}};
        check(device->CreateInputLayout(elements,std::size(elements),vertex_blob->GetBufferPointer(),vertex_blob->GetBufferSize(),&layout),"Vertex layout");
        D3D11_BUFFER_DESC buffer{}; buffer.ByteWidth=sizeof(Constants);
        buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER; buffer.Usage=D3D11_USAGE_DEFAULT;
        check(device->CreateBuffer(&buffer,nullptr,&constants),"View constants");
        buffer.ByteWidth=sizeof(BackdropConstants);
        check(device->CreateBuffer(&buffer,nullptr,&backdrop_constants),"Environment constants");
        // Original plane tests hide polygon backs. Signboards contain distinct
        // coplanar front/back faces; drawing both causes competing textures.
        D3D11_RASTERIZER_DESC raster{}; raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_BACK; raster.DepthClipEnable=TRUE;
        raster.FrontCounterClockwise=TRUE;
        check(device->CreateRasterizerState(&raster,&solid),"Solid rasterizer");
        raster.CullMode=D3D11_CULL_NONE;
        check(device->CreateRasterizerState(&raster,&sprite_solid),"Sprite rasterizer");
        raster.FillMode=D3D11_FILL_WIREFRAME;
        check(device->CreateRasterizerState(&raster,&wire),"Wire rasterizer");
        D3D11_DEPTH_STENCIL_DESC depth_desc{};
        depth_desc.DepthEnable=TRUE; depth_desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        depth_desc.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
        check(device->CreateDepthStencilState(&depth_desc,&reverse_depth),"Reverse depth state");
        D3D11_BLEND_DESC no_color{};
        no_color.RenderTarget[0].RenderTargetWriteMask=0;
        check(device->CreateBlendState(&no_color,&depth_only),"Depth-only world pass");
        D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD=D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler,&texture_sampler),"Original texture sampler");
        load_textures({});
        if(!headset_device)resize(controls.width,controls.height);
    }
    void set_course_colors(const srf::generated::CoursePackage& course) {
        course_colors = &course;
        world_palette = course.day_palette;
    }
    void clear_bonus_overlay() {
        bonus_has_dashboard=bonus_has_overlay=false;
        bonus_brightness=15;
        bonus_scene_weight=1;
    }
    void composite_overlay() {
        if(!bonus_overlay_blend) {
            D3D11_BLEND_DESC blend{};
            auto& rt=blend.RenderTarget[0];rt.BlendEnable=TRUE;
            rt.SrcBlend=rt.SrcBlendAlpha=D3D11_BLEND_ONE;
            rt.DestBlend=rt.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp=rt.BlendOpAlpha=D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
            check(device->CreateBlendState(&blend,&bonus_overlay_blend),"Overlay blending");
        }
        context->OMSetBlendState(bonus_overlay_blend.Get(),nullptr,UINT_MAX);
    }
    // Original OBJ graphics occupy a 208 x 128 playfield. Composite after the 3D
    // pass so the cartridge brightness register affects the scenery and title together.
    void set_bonus_overlay(std::span<const std::uint32_t> rgba,unsigned brightness,float scene_weight,float corner_rows,
                           std::span<const std::uint32_t> dashboard,unsigned clip_top=0,unsigned clip_bottom=224,bool grid_banner=false) {
        if(brightness>15 || rgba.size()!=208*128 || (!dashboard.empty() && dashboard.size()!=256*40))
            throw std::invalid_argument("Invalid Bonus Trax overlay");
        bonus_brightness=brightness;
        bonus_scene_weight=scene_weight;
        bonus_corner_rows=grid_banner?-corner_rows:corner_rows;
        bonus_clip_top=clip_top;
        bonus_clip_bottom=clip_bottom;
        bonus_has_overlay=true;
        bonus_has_dashboard=!dashboard.empty();
        if(!bonus_overlay_ps) {
            constexpr char shader[]=R"(
cbuffer Overlay : register(b0) { float scale, center_left, brightness, play_height; float panel_scale, scene_weight, panel_left, corner_rows; float clip_top, clip_bottom; };
Texture2D<float4> art : register(t0);
float4 main(float4 position : SV_Position) : SV_Target {
    bool panel=panel_scale>0 && position.y>=play_height;
    float scanline=panel ? 168+(position.y-play_height)/panel_scale : 32+128*position.y/play_height;
    if(scanline<clip_top || scanline>=clip_bottom)return float4((brightness*8./255.).xxx,1);
    if(panel) {
        int2 p=int2(floor((position.xy-float2(panel_left,play_height))/panel_scale));
        return float4(brightness*art.Load(int3(clamp(p,int2(0,0),int2(255,39))+int2(0,128),0)).rgb,1);
    }
    // Driving instruments anchor to the edges. The result tally is one centered playfield.
    bool centered=corner_rows==0;
    float y=(position.y-(centered?(play_height-128*scale)*.5:0))/scale;
    bool bottom=!centered && position.y>=play_height-56*scale;
    bool hud=!centered && (y<abs(corner_rows) || bottom);
    bool left_half=position.x<center_left+104*scale;
    float left=hud ? (left_half?0:center_left*2) : center_left;
    int2 pixel=int2(floor((position.x-left)/scale),floor(y));
    if(bottom)pixel.y=128+int(floor((position.y-play_height)/scale));
    float4 ink=all(pixel>=0) && pixel.x<208 && pixel.y<128 ? art.Load(int3(pixel,0)) : 0;
    if(!centered && !hud && pixel.y>=72)ink=0;
    if(hud && left_half!=(pixel.x<104))ink=0;
    if(corner_rows<0 && y>=0 && y<abs(corner_rows)) {
        if(pixel.x>=38 && pixel.x<159)ink=0;
        int center_x=int(floor((position.x-center_left)/scale));
        if(center_x>=38 && center_x<159) {
            float4 banner=art.Load(int3(center_x,pixel.y,0));
            if(banner.a>0)ink=banner;
        }
    }
    float3 tint=(1-scene_weight)*float3(7,10,14)/31;
    return float4(brightness*(ink.rgb*ink.a+(1-ink.a)*tint),1-brightness*(1-ink.a)*scene_weight);
})";
            ComPtr<ID3DBlob> code,errors;
            check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&code,&errors),"Bonus title shader");
            check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&bonus_overlay_ps),"Bonus title pixel shader");
            D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=48;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            check(device->CreateBuffer(&buffer,nullptr,&bonus_overlay_constants),"Bonus title constants");
            D3D11_TEXTURE2D_DESC tex{};tex.Width=256;tex.Height=168;tex.ArraySize=tex.MipLevels=tex.SampleDesc.Count=1;
            tex.Format=DXGI_FORMAT_R8G8B8A8_UNORM;tex.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            check(device->CreateTexture2D(&tex,nullptr,&bonus_overlay_texture),"Bonus title texture");
            check(device->CreateShaderResourceView(bonus_overlay_texture.Get(),nullptr,&bonus_overlay_view),"Bonus title view");
        }
        const D3D11_BOX upper{0,0,0,208,128,1},lower{0,128,0,256,168,1};
        context->UpdateSubresource(bonus_overlay_texture.Get(),0,&upper,rgba.data(),208*4,0);
        if(bonus_has_dashboard)context->UpdateSubresource(bonus_overlay_texture.Get(),0,&lower,dashboard.data(),256*4,0);
    }
    void set_rain(std::span<const srf::RainPixel> pixels) {
        show_rain=!pixels.empty();
        if(!show_rain)return;
        if(!rain_mask) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=208;desc.Height=128;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
            desc.Format=DXGI_FORMAT_R8_UINT;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            check(device->CreateTexture2D(&desc,nullptr,&rain_mask),"Rain mask");
            check(device->CreateShaderResourceView(rain_mask.Get(),nullptr,&rain_view),"Rain mask view");
        }
        std::array<std::uint8_t,208*128> mask{};
        for(const auto p:pixels)mask.at(p.y*208+p.x)=1;
        context->UpdateSubresource(rain_mask.Get(),0,nullptr,mask.data(),208,0);
    }
    void load_textures(std::span<const srf::WorldSprite> sprites, std::span<const srf::WorldSprite> dusk = {}) {
        constexpr unsigned side=128;
        texture_locations.clear();
        textured_materials.fill(false);surface_offsets.clear();
        for(unsigned i=0;i<sprites.size();++i) {
            const auto& sprite=sprites[i];
            if(sprite.material_table==0x8f91)textured_materials.at(sprite.phase)=true;
            if(sprite.material_table==0x9ba3)textured_materials.at(0xc5)=true;
            if(!texture_locations.emplace(std::pair<unsigned,unsigned>{sprite.material_table,sprite.phase},TextureLocation{i+1,sprite.width,sprite.height,sprite.texcoords}).second)
                throw std::runtime_error("Duplicate original texture key");
        }
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=side;desc.MipLevels=1;
        desc.ArraySize=static_cast<UINT>(sprites.size()+1);desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        const auto upload_layers = [&](std::span<const srf::WorldSprite> layers, auto& view) {
            if (layers.size() != sprites.size()) throw std::runtime_error("Course texture sets have different sizes");
            std::vector<std::uint32_t> pixels(desc.ArraySize*side*side);
            std::fill_n(pixels.begin(),side*side,0xffffffffu);
            for (std::size_t i=0; i<layers.size(); ++i) {
                const auto& layer=layers[i];
                for (unsigned y=0; y<layer.height; ++y)
                    std::copy_n(layer.pixels.begin()+y*layer.width,layer.width,pixels.begin()+(i+1)*side*side+y*side);
            }
            std::vector<D3D11_SUBRESOURCE_DATA> initial;
            for (unsigned i=0; i<desc.ArraySize; ++i)
                initial.push_back({pixels.data()+i*side*side,side*4,side*side*4});
            ComPtr<ID3D11Texture2D> texture;
            check(device->CreateTexture2D(&desc,initial.data(),&texture),"Original texture array");
            check(device->CreateShaderResourceView(texture.Get(),nullptr,view.ReleaseAndGetAddressOf()),"Original texture view");
        };
        upload_layers(sprites,texture_view);
        dusk_texture_view.Reset();
        if (!dusk.empty()) upload_layers(dusk,dusk_texture_view);
        palette_indices_view.Reset();
        if (!sprites.empty() && !sprites.front().palette_indices.empty()) {
            desc.Format=DXGI_FORMAT_R8_UINT;
            std::vector<std::uint8_t> indices(desc.ArraySize*side*side);
            for(std::size_t i=0;i<sprites.size();++i) {
                const auto& sprite=sprites[i];
                for(unsigned y=0;y<sprite.height;++y)
                    std::copy_n(sprite.palette_indices.begin()+y*sprite.width,sprite.width,
                                indices.begin()+(i+1)*side*side+y*side);
            }
            std::vector<D3D11_SUBRESOURCE_DATA> initial;
            for(unsigned i=0;i<desc.ArraySize;++i)initial.push_back({indices.data()+i*side*side,side,0});
            ComPtr<ID3D11Texture2D> texture;
            check(device->CreateTexture2D(&desc,initial.data(),&texture),"Original sprite palette indices");
            check(device->CreateShaderResourceView(texture.Get(),nullptr,&palette_indices_view),"Indexed sprites");
        }
    }
    void load_backdrop(srf::CourseBackdrop source) {
        backdrop=std::move(source);backdrop_weight=skyline_phase=UINT_MAX;
        backdrop_indices.Reset();backdrop_colors.Reset();backdrop_color_texture.Reset();
        if(!backdrop.available()) return;
        D3D11_TEXTURE2D_DESC desc{};desc.Width=srf::CourseBackdrop::width;desc.Height=srf::CourseBackdrop::height;
        desc.MipLevels=1;desc.ArraySize=2;desc.Format=DXGI_FORMAT_R8_UINT;
        desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        std::array<D3D11_SUBRESOURCE_DATA,2> initial{};
        for(unsigned i=0;i<2;++i) {
            if(backdrop.indices[i].size()!=desc.Width*desc.Height)throw std::runtime_error("Invalid backdrop dimensions");
            initial[i]={backdrop.indices[i].data(),desc.Width,0};
        }
        ComPtr<ID3D11Texture2D> texture;
        check(device->CreateTexture2D(&desc,initial.data(),&texture),"Backdrop index texture");
        check(device->CreateShaderResourceView(texture.Get(),nullptr,&backdrop_indices),"Backdrop index view");
        desc.Width=784;desc.Height=1;desc.ArraySize=1;desc.Format=DXGI_FORMAT_R16_UINT;desc.Usage=D3D11_USAGE_DEFAULT;
        check(device->CreateTexture2D(&desc,nullptr,&backdrop_color_texture),"Backdrop color texture");
        check(device->CreateShaderResourceView(backdrop_color_texture.Get(),nullptr,&backdrop_colors),"Backdrop color view");
    }
    void set_environment(const srf::EnvironmentPresentation& value) {
        environment=value;
        if(!backdrop.available() || (backdrop_weight==value.palette_weight && backdrop_fog==value.fog_weight &&
            backdrop_fog_rgb555==value.fog_rgb555 && skyline_phase==value.skyline_phase &&
            skyline_blend_phase==value.skyline_blend_phase))return;
        const auto fog=[&](std::uint16_t color) {
            return srf::blend_environment_rgb555(color,value.fog_rgb555,value.fog_weight);
        };
        if (dusk_texture_view || palette_indices_view) {
            for (std::size_t i=0; i<world_palette.size(); ++i) {
                const auto color=srf::blend_environment_rgb555(course_colors->day_palette[i],
                    course_colors->dusk_palette[i],value.palette_weight);
                world_palette[i]=(backdrop.fog_protected[i/16]&(1u<<(i%16)))?color:fog(color);
            }
        }
        std::array<std::uint16_t,784> colors;
        for(unsigned i=0;i<16;++i)colors[i]=fog(srf::backdrop_color(backdrop,i,value.palette_weight,
                                                                    value.skyline_phase,value.skyline_blend_phase));
        for(unsigned i=0;i<512;++i)colors[i+16]=fog(srf::blend_environment_rgb555(backdrop.gradients[0][i],backdrop.gradients[1][i],value.palette_weight));
        std::copy(world_palette.begin(),world_palette.end(),colors.begin()+528);
        context->UpdateSubresource(backdrop_color_texture.Get(),0,nullptr,colors.data(),sizeof(colors),0);
        backdrop_weight=value.palette_weight;
        backdrop_fog=value.fog_weight;
        backdrop_fog_rgb555=value.fog_rgb555;
        skyline_phase=value.skyline_phase;
        skyline_blend_phase=value.skyline_blend_phase;
    }
    SceneTarget make_target(UINT w,UINT h,ComPtr<ID3D11Texture2D> color={},DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN) {
        SceneTarget result{w,h,std::move(color)};
        if (!result.color) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=w; desc.Height=h;
            desc.MipLevels=1; desc.ArraySize=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count=1; desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            check(device->CreateTexture2D(&desc,nullptr,&result.color),"Offscreen image");
        }
        D3D11_RENDER_TARGET_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        check(device->CreateRenderTargetView(result.color.Get(),format==DXGI_FORMAT_UNKNOWN?nullptr:&view,&result.rtv),"Render target");
        D3D11_TEXTURE2D_DESC dd{}; dd.Width=w; dd.Height=h; dd.MipLevels=1; dd.ArraySize=1;
        dd.Format=DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count=1; dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;
        check(device->CreateTexture2D(&dd,nullptr,&depth),"Depth image");
        check(device->CreateDepthStencilView(depth.Get(),nullptr,&result.dsv),"Depth view");
        return result;
    }
    void resize(UINT w,UINT h) {
        context->OMSetRenderTargets(0,nullptr,nullptr);
        desktop={};
        ComPtr<ID3D11Texture2D> back_buffer;
        if(swapchain) {
            check(swapchain->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0),"Swapchain resize");
            check(swapchain->GetBuffer(0,IID_PPV_ARGS(&back_buffer)),"Swapchain image");
        }
        desktop=make_target(w,h,std::move(back_buffer));
    }
    void load(const srf::Mesh& mesh,std::size_t frame) {
        const auto& source=mesh.frames.at(frame%mesh.frames.size());
        std::array<double,3> low{1e20,1e20,1e20}, high{-1e20,-1e20,-1e20};
        for (const auto& v:source) {
            const double p[]{double(v.x),double(v.y),double(v.z)};
            for(int a=0;a<3;++a) { low[a]=std::min(low[a],p[a]); high[a]=std::max(high[a],p[a]); }
        }
        const double radius=std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2],1.0})*.7;
        std::vector<GpuVertex> data;
        for(const auto& face:mesh.polygons) {
            const auto normal=[](std::uint8_t byte){return float(static_cast<std::int8_t>(byte))/127.f;};
            const float brightness=std::clamp(.55f+.35f*std::abs(normal(face.attributes[3])*.3f-
                normal(face.attributes[4])*.8f+normal(face.attributes[5])*.4f),.25f,1.f);
            // Fan triangulation is a diagnostic view. Original material and
            // visibility commands remain unimplemented and are not inferred.
            for(std::size_t i=1;i+1<face.indices.size();++i) {
                for(const auto index:{face.indices[0],face.indices[i],face.indices[i+1]}) {
                    const auto& v=source.at(index);
                    data.push_back({float((v.x-(low[0]+high[0])*.5)/radius),
                        -float((v.y-(low[1]+high[1])*.5)/radius),float((v.z-(low[2]+high[2])*.5)/radius),
                        brightness*.63f,brightness*.83f,brightness});
                }
            }
        }
        upload(data);
    }
    std::array<float,3> normalize_world(const std::array<float,3>& point) const {
        return {float((point[0]-world_center[0])/world_radius),
                -float((point[1]-world_center[1])/world_radius),
                float((point[2]-world_center[2])/world_radius)};
    }
    float normalized_distance(float distance) const {return float(distance/world_radius);}
    void load_race_world(const srf::World& world,std::span<const RenderVehicle> vehicles,const srf::Rom& rom,
                         const srf::NativeRace& race,const srf::CourseScenery& scenery) {
        load_world(world,0,vehicles,&rom,scenery.poses(),race.rock_poses(),race.cone_poses(),
                   race.bridge_poses(),scenery.sprites(),&race);
    }
    void load_stunt_world(const srf::World& world,const RenderVehicle& player,const srf::Rom& rom,
                          const srf::RockFieldDrive& drive,const srf::RockFieldRules& rules) {
        load_world(world,0,std::span(&player,1),&rom,drive.poses(),{},rules.gate_poses(),
                   drive.bridge_poses(),{},nullptr,rules.star_poses());
    }
    void load_world(const srf::World& world,std::size_t frame,std::span<const RenderVehicle> vehicles={},const srf::Rom* rom=nullptr,
                    std::span<const srf::SceneryPose> scenery={}, std::span<const srf::SceneryPose> extra_actors={},
                    std::span<const srf::SceneryPose> cones={}, std::span<const srf::SceneryPose> bridges={},
                    std::span<const srf::ScenerySprite> scene_sprites={},const srf::NativeRace* race=nullptr,
                    std::span<const srf::SceneryPose> geometric_actors={}) {
        const auto props=race ? race->prop_poses() : std::span<const srf::SceneryPose>{};
        const auto bonus=race ? race->bonus_poses() : std::span<const srf::SceneryPose>{};
        const auto pickups=race ? race->pickup_poses() : std::span<const srf::SceneryPose>{};
        std::map<unsigned,const srf::WorldModel*> models;
        for(const auto& model:world.models) models.emplace(model.descriptor_address,&model);
        std::array<double,3> low{1e20,1e20,1e20},high{-1e20,-1e20,-1e20};
        for(const auto& instance:world.instances) {
            const auto found=models.find(instance.visual_descriptor);
            if(found==models.end() || found->second->primitives.empty()) continue;
            const auto& model=*found->second;
            for(const auto& vertex:model.frames[frame%model.frames.size()]) {
                const auto p=srf::world_position(vertex,instance);
                for(int a=0;a<3;++a) { low[a]=std::min(low[a],p[a]); high[a]=std::max(high[a],p[a]); }
            }
        }
        world_radius=std::max({high[0]-low[0],high[1]-low[1],high[2]-low[2],1.0})*.55;
        for(unsigned a=0;a<3;++a) world_center[a]=(low[a]+high[a])*.5;
        std::vector<GpuVertex> triangles,lines;
        const auto draw_model=[&](const srf::WorldModel& model,const auto& transform,std::uint8_t opcode=0,std::int16_t descriptor_bias=0,float painter_depth=0,bool static_model=false,
                                  std::optional<std::size_t> animation_frame=std::nullopt, unsigned sign_phase=0) {
            const auto pose_index=animation_frame.value_or(frame)%model.frames.size();
            const auto& pose=model.frames[pose_index];
            const auto key=std::pair<unsigned,std::size_t>{model.descriptor_address,pose_index};
            auto offsets=surface_offsets.find(key);
            if(offsets==surface_offsets.end())offsets=surface_offsets.emplace(key,
                srf::coplanar_surface_offsets(model,pose_index,textured_materials)).first;
            auto backs=barrier_backs.find(key);
            if(static_model && backs==barrier_backs.end())backs=barrier_backs.emplace(key,
                srf::missing_barrier_backfaces(model,pose_index)).first;
            const auto origin=transform(srf::Vertex{0,0,0});
            const auto road_origin=normalize_world({float(origin[0]),float(origin[1]),float(origin[2])});
            const float road_bias=normalized_distance(float(descriptor_bias)+(opcode==0x0c?8000.f:0.f));
            const std::array basis{transform(srf::Vertex{1,0,0}),transform(srf::Vertex{0,1,0}),transform(srf::Vertex{0,0,1})};
            std::size_t primitive_index=0;
            for(const auto& face:model.primitives) {
                const bool add_back=static_model && backs->second[primitive_index];
                const auto shift=offsets->second[primitive_index++];
                const bool priority_line=face.indices.size()==2 && (opcode==0x0c || descriptor_bias>0);
                const float support_depth=(priority_line || srf::road_support_surface(opcode,descriptor_bias,pose,face))?painter_depth:0.f;
                const unsigned material=face.attributes[2];
                const float intensity=.55f+.35f*std::abs(float(static_cast<std::int8_t>(face.attributes[4]))/127.f);
                float r=.7f*intensity,g=.25f*intensity,b=.7f*intensity;
                if(srf::generated::world_material_kinds[material]) {
                    // Original palette and shade ramps; lighting direction is
                    // still a presentation choice, not original fixed-point parity.
                    const auto shade=unsigned(std::clamp(int(intensity*15),6,15));
                    const auto color=world_palette[srf::generated::world_material_indices[material][shade]];
                    r=float(color&31)/31; g=float((color>>5)&31)/31; b=float((color>>10)&31)/31;
                }
                const bool course_sign=material==0xc5 && sign_phase!=0;
                const auto texture=texture_locations.find(course_sign ? std::pair{0x9ba3u,sign_phase} : std::pair{0x8f91u,material});
                const bool textured=texture!=texture_locations.end() && face.indices.size()==4;
                if(textured) r=g=b=1;
                auto vertex=[&](unsigned index) {
                    auto point=transform(pose.at(index));
                    for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b)
                        point[a]+=(basis[b][a]-origin[a])*shift[b];
                    const auto p=normalize_world({float(point[0]),float(point[1]),float(point[2])});
                    GpuVertex out{p[0],p[1],p[2],r,g,b};
                    out.painter_depth=support_depth;
                    if(support_depth!=0)out.road_origin_bias={road_origin[0],road_origin[1],road_origin[2],road_bias};
                    if(textured) {
                        const auto corner=std::find(face.indices.begin(),face.indices.end(),index)-face.indices.begin();
                        // GSU01:9171..917D consumes original metadata UV bytes
                        // in face-vertex order; no guessed decal rotation.
                        const float u=float(texture->second.uv[corner][0])+.5f;
                        const float v=float(texture->second.uv[corner][1])+.5f;
                        out.u=u/128.f;out.v=v/128.f;out.texture=float(texture->second.layer);
                    }
                    return out;
                };
                if(face.indices.size()==2) {
                    lines.push_back(vertex(face.indices[0])); lines.push_back(vertex(face.indices[1]));
                } else for(std::size_t i=1;i+1<face.indices.size();++i) {
                    for(auto index:{face.indices[0],face.indices[i],face.indices[i+1]}) triangles.push_back(vertex(index));
                    if(add_back)for(auto index:{face.indices[0],face.indices[i+1],face.indices[i]}) triangles.push_back(vertex(index));
                }
            }
        };
        for(const auto& instance:world.instances) {
            const auto replaces_instance=[&](const auto& pose){return pose.command_offset==instance.command_offset;};
            if(std::any_of(geometric_actors.begin(),geometric_actors.end(),replaces_instance) ||
               std::any_of(scenery.begin(),scenery.end(),replaces_instance) ||
               std::any_of(cones.begin(),cones.end(),replaces_instance) ||
               std::any_of(props.begin(),props.end(),replaces_instance) ||
               std::any_of(bonus.begin(),bonus.end(),replaces_instance) ||
               std::any_of(pickups.begin(),pickups.end(),replaces_instance)) continue;
            const auto found=models.find(instance.visual_descriptor);
            float painter_depth=0;
            std::int16_t signed_bias=0;
            if(rom && !vehicles.empty()) {
                const auto point=normalize_world({float(instance.position.x),float(instance.position.y),float(instance.position.z)});
                const float y=point[1]-controls.target_y;
                const float z=-std::sin(controls.yaw)*(point[0]-controls.target_x)+std::cos(controls.yaw)*(point[2]-controls.target_z);
                const float camera_z=(std::sin(controls.pitch)*y+std::cos(controls.pitch)*z+controls.distance)*float(world_radius);
                const float camera_y=float(world_center[1])-(controls.target_y-controls.distance*std::sin(controls.pitch))*float(world_radius);
                const auto bias=rom->u16((instance.visual_descriptor&0x7fff)+5);
                signed_bias=std::int16_t(bias<32768?int(bias):int(bias)-65536);
                painter_depth=srf::road_painter_depth(camera_z,float(instance.position.y)-camera_y,signed_bias,instance.opcode==0x0c)/float(world_radius);
            }
            if(found!=models.end()) {
                // The moving road deck uses its collision frame and the same depth pass as its approaches.
                auto bridge=std::find_if(bridges.begin(),bridges.end(),replaces_instance);
                std::optional<std::size_t> deck_frame;
                if(bridge!=bridges.end()) deck_frame=bridge->frame;
                draw_model(*found->second,[&](const srf::Vertex& v){return srf::world_position(v,instance);},
                           instance.opcode,signed_bias,painter_depth,true,deck_frame,instance.sign_phase);
            }
        }
        const float cy=std::cos(controls.yaw),sy=std::sin(controls.yaw);
        const float cp=std::cos(controls.pitch),sp=std::sin(controls.pitch);
        constexpr std::array world_winding{0,2,1,0,3,2}, vehicle_winding{0,1,2,0,2,3};
        const auto draw_sprite=[&](const RenderSprite& sprite,const std::array<int,6>& indices) {
            const auto found=texture_locations.find({sprite.table,sprite.phase});
            if(found==texture_locations.end()) return;
            const auto& t=found->second;const auto center=normalize_world(sprite.position);
            const float half=normalized_distance(sprite.diameter)*.5f;
            std::array<GpuVertex,4> quad;
            for(unsigned i=0;i<4;++i) {
                const float x=(i==1 || i==2)?half:-half,y=i>=2?-half:half;
                quad[i]={center[0]+cy*x+sy*sp*y,center[1]+cp*y,center[2]+sy*x-cy*sp*y,
                    1,1,1,((i==1 || i==2)?float(t.width)-.5f:.5f)/128.f,
                    (i>=2?float(t.height)-.5f:.5f)/128.f,float(t.layer)};
            }
            for(auto i:indices) triangles.push_back(quad[i]);
        };
        for(const auto& sprite:scene_sprites) {
            const auto p=sprite.position;
            // The world pass culls back faces, so face the camera.
            draw_sprite({sprite.table,0,{float(p.x),float(p.y),float(p.z)},float(sprite.diameter)},world_winding);
        }
        const auto draw_actors=[&](std::span<const srf::SceneryPose> actors) {
            for (const auto& pose : actors) {
                if (!pose.visible) continue;
                const auto found=models.find(pose.visual_descriptor);
                if(found==models.end()) throw std::runtime_error("Animated scenery descriptor missing from generated world");
                draw_model(*found->second,[&](const srf::Vertex& v){return srf::scenery_position(v,pose);},0,0,0,false,pose.frame);
            }
        };
        // Stunt stars move above and behind ramps. Test their actual geometry
        // against the deck before rebuilding the road-priority depth for cars.
        draw_actors(geometric_actors);
        world_vertex_count=static_cast<UINT>(triangles.size());
        world_line_count=static_cast<UINT>(lines.size());
        vehicle_depth_pass=rom && !vehicles.empty();
        for(const auto actors:{scenery,extra_actors,cones,props,pickups,bonus})draw_actors(actors);
        for(const auto& vehicle:vehicles) {
            const auto found=models.find(vehicle.descriptor);
            if(found==models.end()) throw std::runtime_error("Original vehicle visual descriptor missing from generated world");
            draw_model(*found->second,[&](const srf::Vertex& v) {
                const std::array<float,3> local{float(v.x),float(v.y),float(v.z)};
                auto p=vehicle.position;
                for(unsigned a=0;a<3;++a) for(unsigned b=0;b<3;++b) p[a]+=local[b]*vehicle.orientation[b*3+a];
                return p;
            });
        }
        // Wheel and eye sprites draw over the rebuilt actor depth. The trees
        // above belong to the world and occlude cars only at opaque texels.
        const auto surface_count=static_cast<UINT>(triangles.size());
        if(race)for(const auto sprites:{race->spray_sprites(),race->breakable_sprites()}) {
            for(const auto& sprite:sprites) {
                const auto p=sprite.position;
                draw_sprite({sprite.table,sprite.frame,{float(p.x),float(p.y),float(p.z)},float(sprite.diameter)},vehicle_winding);
            }
        }
        for(const auto& vehicle:vehicles) for(const auto& sprite:vehicle.sprites) draw_sprite(sprite,vehicle_winding);
        upload(triangles,lines,surface_count);
    }
    void render(std::span<const std::uint32_t> demo_frame={}) {
        render_scene(desktop,nullptr,demo_frame);
        if(!demo_frame.empty())render_course_map(demo_frame,true);
        else present();
    }
    void render_eye(const SceneTarget& target,const SceneEye& eye) {
        render_scene(target,&eye,{});
    }
private:
    void render_scene(const SceneTarget& target,const SceneEye* eye,std::span<const std::uint32_t> demo_frame) {
        const auto& [width,height,color,rtv,dsv]=target;
        const unsigned sky=world_palette[0];
        float background[]{controls.world?float(sky&31)/31:.035f,
            controls.world?float((sky>>5)&31)/31:.055f,controls.world?float((sky>>10)&31)/31:.075f,1};
        if(headset_output)for(int i=0;i<3;++i){
            auto& c=background[i];c=c<=.04045f?c/12.92f:std::pow((c+.055f)/1.055f,2.4f);
        }
        context->ClearRenderTargetView(rtv.Get(),background);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        context->OMSetDepthStencilState(reverse_depth.Get(),0);
        context->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());
        const float panel_scale=!eye && bonus_has_dashboard?std::min(width/256.f,height/168.f):0.f;
        const float play_height=std::max(1.f,float(height)-std::round(panel_scale*40));
        D3D11_VIEWPORT viewport{0,0,float(width),play_height,0,1};
        float aspect=float(width)/play_height;
        if(!demo_frame.empty()) {
            // Its 208x128 playfield sits inside the same 4:3 raster as the title.
            const float w=std::min(float(width),height*(4.f/3)),h=w*.75f;
            viewport={(width-w)*.5f+w*24/256,(height-h)*.5f+h*32/224,w*208/256,h*128/224,0,1};
            aspect=208.f/128;
            constexpr float black[]{0,0,0,1};
            context->ClearRenderTargetView(rtv.Get(),black);
        }
        context->RSSetViewports(1,&viewport); context->RSSetState(controls.wire?wire.Get():solid.Get());
        Constants value{std::cos(controls.yaw),std::sin(controls.yaw),std::cos(controls.pitch),std::sin(controls.pitch),
            aspect,controls.zoom,controls.distance,0,controls.target_x,controls.target_y,controls.target_z,0};
        if(eye)value.set_eye(*eye);
        context->UpdateSubresource(constants.Get(),0,nullptr,&value,0,0);
        context->VSSetConstantBuffers(0,1,constants.GetAddressOf());
        context->PSSetConstantBuffers(0,1,constants.GetAddressOf());
        BackdropConstants surroundings{};
        if(backdrop.available()) {
            // Preserve the original panorama's angular scale; widescreen reveals
            // more of it around the native chase camera's horizontal centre.
            constexpr double angle_scale=65536.0/6.283185307179586;
            // The view matrix rotates the world; BG2 scroll takes the camera's heading.
            const auto view=srf::backdrop_view(static_cast<std::int16_t>(static_cast<int>(-controls.yaw*angle_scale)),
                static_cast<std::int16_t>(static_cast<int>(controls.pitch*angle_scale)));
            surroundings.span_x=demo_frame.empty()?128.f*width/play_height:208.f;surroundings.span_y=128;
            surroundings.left=view.x+104-surroundings.span_x*.5f;surroundings.top=float(view.y);
            surroundings.layer=environment.alternate_backdrop;surroundings.blank=environment.blank_backdrop;
            surroundings.fixed_rgb555=environment.fixed_rgb555;surroundings.color_math_layers=environment.color_math_layers;
            surroundings.add_fixed=environment.additive_fixed_color;
            surroundings.gradient_y=view.gradient_y;surroundings.sky_rgb555=sky;
            surroundings.texture_weight=dusk_texture_view ? environment.palette_weight : 0;
            surroundings.indexed_textures=bool(palette_indices_view);
            if(std::any_of(environment.row_subtract_rgb555.begin(),environment.row_subtract_rgb555.end(),
                           [](auto color){return color!=0;})) {
                surroundings.rows_per_pixel=128.f/play_height;
                std::copy(environment.row_subtract_rgb555.begin(),environment.row_subtract_rgb555.end(),surroundings.row_subtract.begin());
            }
        }
        context->UpdateSubresource(backdrop_constants.Get(),0,nullptr,&surroundings,0,0);
        context->VSSetConstantBuffers(1,1,backdrop_constants.GetAddressOf());
        context->PSSetConstantBuffers(1,1,backdrop_constants.GetAddressOf());
        if(backdrop.available()) {
            context->RSSetState(sprite_solid.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(backdrop_vs.Get(),nullptr,0);context->PSSetShader(backdrop_ps.Get(),nullptr,0);
            context->PSSetShaderResources(1,1,backdrop_indices.GetAddressOf());
            context->PSSetShaderResources(2,1,backdrop_colors.GetAddressOf());
            context->Draw(3,0);
            context->RSSetState(controls.wire?wire.Get():solid.Get());
        }
        context->VSSetShader(vs.Get(),nullptr,0); context->PSSetShader(ps.Get(),nullptr,0);
        context->PSSetShaderResources(0,1,texture_view.GetAddressOf());
        context->PSSetShaderResources(3,1,dusk_texture_view.GetAddressOf());
        context->PSSetShaderResources(4,1,palette_indices_view.GetAddressOf());
        context->PSSetSamplers(0,1,texture_sampler.GetAddressOf());
        context->IASetInputLayout(layout.Get());
        const UINT stride=sizeof(GpuVertex),offset=0;
        context->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(vehicle_depth_pass) {
            context->Draw(world_vertex_count,0);
            if(world_line_count) {
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                context->Draw(world_line_count,vertex_count);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }
            // Preserve world color and its geometric self-occlusion, then rebuild
            // the actor occluders with the original road priority. No color or
            // geometry moves, and actual wall/car depth testing stays enabled.
            context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
            context->OMSetBlendState(depth_only.Get(),nullptr,UINT_MAX);
            value.road_pass=1;
            context->UpdateSubresource(constants.Get(),0,nullptr,&value,0,0);
            context->Draw(world_vertex_count,0);
            if(world_line_count) {
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                context->Draw(world_line_count,vertex_count);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }
            context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
            value.road_pass=0;
            context->UpdateSubresource(constants.Get(),0,nullptr,&value,0,0);
            context->Draw(surface_vertex_count-world_vertex_count,world_vertex_count);
        } else context->Draw(surface_vertex_count,0);
        context->RSSetState(controls.wire?wire.Get():sprite_solid.Get());
        if(vertex_count>surface_vertex_count)context->Draw(vertex_count-surface_vertex_count,surface_vertex_count);
        if(vehicle_depth_pass && line_count>world_line_count) {
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            context->Draw(line_count-world_line_count,vertex_count+world_line_count);
        }
        if(line_count && !vehicle_depth_pass) { context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST); context->Draw(line_count,vertex_count); }
        // The eye contains the scene. Dashboard, screen-space rain and menu
        // overlays are authored for the flat desktop viewport.
        if(eye)return;
        if(show_rain) {
            context->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
            context->RSSetState(sprite_solid.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(backdrop_vs.Get(),nullptr,0);context->PSSetShader(rain_ps.Get(),nullptr,0);
            context->PSSetShaderResources(5,1,rain_view.GetAddressOf());context->Draw(3,0);
            ID3D11ShaderResourceView* unbind=nullptr;
            context->PSSetShaderResources(5,1,&unbind);
        }
        if(bonus_has_overlay || bonus_brightness<15 || bonus_scene_weight<1.f) {
            const D3D11_VIEWPORT full{0,0,float(width),float(height),0,1};
            context->RSSetViewports(1,&full);
            const float scale=std::min(width/208.f,play_height/128.f);
            const std::array<float,12> placement{scale,(width-208.f*scale)*.5f,
                bonus_brightness/15.f,play_height,panel_scale,bonus_scene_weight,(width-256.f*panel_scale)*.5f,bonus_corner_rows,
                float(bonus_clip_top),float(bonus_clip_bottom)};
            context->UpdateSubresource(bonus_overlay_constants.Get(),0,nullptr,placement.data(),0,0);
            context->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
            composite_overlay();
            context->RSSetState(sprite_solid.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(backdrop_vs.Get(),nullptr,0);context->PSSetShader(bonus_overlay_ps.Get(),nullptr,0);
            context->PSSetConstantBuffers(0,1,bonus_overlay_constants.GetAddressOf());
            context->PSSetShaderResources(0,1,bonus_overlay_view.GetAddressOf());
            context->Draw(3,0);
            ID3D11ShaderResourceView* unbind=nullptr;
            context->PSSetShaderResources(0,1,&unbind);
            context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
        }
    }
public:
    void render_course_map(std::span<const std::uint32_t> pixels,bool over_scene=false) {
        const auto& [width,height,color,rtv,dsv]=desktop;
        constexpr UINT columns=256,rows=224;
        if(pixels.size()!=columns*rows)throw std::invalid_argument("Invalid course map size");
        if(!course_map_texture) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width=columns;desc.Height=rows;desc.MipLevels=desc.ArraySize=1;
            desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            check(device->CreateTexture2D(&desc,nullptr,&course_map_texture),"Course map texture");
            check(device->CreateShaderResourceView(course_map_texture.Get(),nullptr,&course_map_view),"Course map view");
            constexpr char shader[]=R"(
Texture2D<float4> board : register(t0);
float4 main(float4 position : SV_Position,float2 uv : TEXCOORD) : SV_Target {
    return board.Load(int3(clamp(int2(uv*float2(256,224)),int2(0,0),int2(255,223)),0));
})";
            ComPtr<ID3DBlob> code,errors;
            check(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"main","ps_5_0",0,0,&code,&errors),"Course map shader");
            check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&course_map_ps),"Course map pixel shader");
        }
        context->UpdateSubresource(course_map_texture.Get(),0,nullptr,pixels.data(),columns*4,0);
        const float w=std::min(float(width),height*(4.f/3)),h=w*.75f;
        const D3D11_VIEWPORT viewport{(width-w)*.5f,(height-h)*.5f,w,h,0,1};
        const float black[]{0,0,0,1};
        if(!over_scene)context->ClearRenderTargetView(rtv.Get(),black);
        context->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
        context->RSSetViewports(1,&viewport);
        context->RSSetState(sprite_solid.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(backdrop_vs.Get(),nullptr,0);
        context->PSSetShader(course_map_ps.Get(),nullptr,0);
        context->PSSetShaderResources(0,1,course_map_view.GetAddressOf());
        if(over_scene)composite_overlay();
        context->Draw(3,0);
        if(over_scene)context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
        ID3D11ShaderResourceView* unbind=nullptr;
        context->PSSetShaderResources(0,1,&unbind);
        present();
    }
    std::vector<BYTE> readback_bgra() {
        return readback_bgra(desktop);
    }
    std::vector<BYTE> readback_bgra(const SceneTarget& target) {
        const auto& [width,height,color,rtv,dsv]=target;
        D3D11_TEXTURE2D_DESC desc; color->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING; desc.BindFlags=0; desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        check(device->CreateTexture2D(&desc,nullptr,&staging),"Screenshot readback image");
        context->CopyResource(staging.Get(),color.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Screenshot readback");
        std::vector<BYTE> pixels(static_cast<std::size_t>(width)*height*4);
        for(UINT y=0;y<height;++y) {
            const auto row=static_cast<const BYTE*>(mapped.pData)+y*mapped.RowPitch;
            for(UINT x=0;x<width;++x) {
                const auto p=(static_cast<std::size_t>(y)*width+x)*4;
                pixels[p]=row[x*4+2]; pixels[p+1]=row[x*4+1]; pixels[p+2]=row[x*4]; pixels[p+3]=255;
            }
        }
        context->Unmap(staging.Get(),0);
        return pixels;
    }
    void screenshot(const std::filesystem::path& path,const std::wstring& hud=L"") {
        const auto width=desktop.width,height=desktop.height;
        auto pixels=readback_bgra();
        // The interactive HUD is a native GDI child window. Reuse its font,
        // text and colors in bounded offscreen captures, which have no HWND.
        if(!hud.empty()) {
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=static_cast<LONG>(width);info.bmiHeader.biHeight=-static_cast<LONG>(height);
            info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            void* bits=nullptr;HDC dc=CreateCompatibleDC(nullptr);
            HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(!dc || !bitmap || !bits) {if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);throw std::runtime_error("HUD capture surface failed");}
            const auto old_bitmap=SelectObject(dc,bitmap);
            std::copy(pixels.begin(),pixels.end(),static_cast<BYTE*>(bits));
            HBRUSH background=CreateSolidBrush(RGB(12,20,27));
            RECT panel{12,12,static_cast<LONG>(width)-12,124};FillRect(dc,&panel,background);
            HFONT font=CreateFontW(18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            const auto old_font=SelectObject(dc,font);SetTextColor(dc,RGB(235,241,245));SetBkMode(dc,TRANSPARENT);
            DrawTextW(dc,hud.c_str(),static_cast<int>(hud.size()),&panel,DT_LEFT|DT_TOP|DT_NOPREFIX|DT_WORDBREAK);
            GdiFlush();std::copy_n(static_cast<BYTE*>(bits),pixels.size(),pixels.begin());
            for(std::size_t alpha=3;alpha<pixels.size();alpha+=4) pixels[alpha]=255;
            SelectObject(dc,old_font);SelectObject(dc,old_bitmap);DeleteObject(font);DeleteObject(background);DeleteObject(bitmap);DeleteDC(dc);
        }
        write_png(path,pixels,width,height);
    }
    static void write_png(const std::filesystem::path& path,std::vector<BYTE>& pixels,UINT width,UINT height) {
        if (std::filesystem::exists(path)) throw std::runtime_error("Screenshot exists; select a new path");
        ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
        check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"PNG factory");
        check(factory->CreateStream(&stream),"PNG stream");
        check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG output");
        check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");
        check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG initialize");
        check(encoder->CreateNewFrame(&frame,nullptr),"PNG frame"); check(frame->Initialize(nullptr),"PNG frame initialize");
        check(frame->SetSize(width,height),"PNG dimensions");
        WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
        check(frame->SetPixelFormat(&format),"PNG pixel format");
        if(format!=GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("Unsupported PNG pixel format");
        check(frame->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data()),"PNG pixels");
        check(frame->Commit(),"PNG frame commit"); check(encoder->Commit(),"PNG commit");
    }
};

int signed_word(unsigned word) {return word>=32768?int(word)-65536:int(word);}
float lerp_angle(unsigned before,unsigned after,float blend) {
    const auto delta=signed_word(static_cast<std::uint16_t>(after-before));
    return (float(before)+float(delta)*blend)*(6.28318530718f/65536.f);
}
RenderVehicle vehicle_pose(const srf::Rom& rom,const srf::Vehicle& before,const srf::Vehicle& after,
                           bool player,float blend,std::uint8_t camera_byte,bool exterior_eyes=true) {
    RenderVehicle out;
    const auto& a=before.body;
    const auto& b=after.body;
    const auto definition=0x48000+(b.word(0x66)&0x7fff);
    out.descriptor=rom.u16(definition+(player?0:2));
    for(unsigned axis=0;axis<3;++axis) {
        const auto old=signed_word(a.word(0x1e + axis*2));
        const auto delta=signed_word(static_cast<std::uint16_t>(b.word(0x1e + axis*2)-a.word(0x1e + axis*2)));
        out.position[axis]=float(old)+float(delta)*blend;
    }
    const auto yaw=-lerp_angle(a.word(0x24),b.word(0x24),blend);
    const auto pitch=-lerp_angle(a.word(0x26),b.word(0x26),blend);
    const auto roll=-lerp_angle(a.word(0x28),b.word(0x28),blend);
    const auto sy=std::sin(yaw),cy=std::cos(yaw),sp=std::sin(pitch),cp=std::cos(pitch),sr=std::sin(roll),cr=std::cos(roll);
    out.orientation={sy*sr*sp+cy*cr,sr*cp,cy*sr*sp-sy*cr,
                     sy*cr*sp-cy*sr,cr*cp,cy*cr*sp+sy*sr,sy*cp,-sp,cy*cp};
    for(unsigned wheel_index=0;wheel_index<4;++wheel_index) {
        const auto& wheel=after.wheels[wheel_index];
        // CPU08:BD32..BD54 hides far-controller wheel sprites. Their
        // attachments intentionally retain stale yaw and collapse to body XYZ.
        if(b.word(0x16)&0x400)continue;
        const bool became_near=(a.word(0x16)&0x400)!=0;
        // A newly visible tire has no valid previous attachment pose.
        // Interpolating it would invent a large spin from stale far-AI data.
        const auto& old=became_near?wheel:before.wheels[wheel_index];
        if(wheel.word(0x46)&0x200) continue; // Original08:BA70 skips hidden wheel definitions.
        const auto data=0x48000+wheel.word(0x12);
        const auto diameter_addition=rom.u16(data+4);
        const auto sprite_descriptor=rom.u16(data);
        const auto sprite_base=std::size_t(sprite_descriptor&0x7fff);
        const float diameter=float(srf::original_sprite_diameter(rom.u16(sprite_base+16),rom.bytes()[sprite_base+7],std::uint8_t(diameter_addition)));
        RenderSprite sprite;
        const auto wheel_yaw=srf::interpolated_angle_byte(old.word(0x24),wheel.word(0x24),blend);
        const auto relative=std::uint8_t(wheel_yaw-camera_byte);
        const auto wheel_roll=srf::wheel_sprite_bank(srf::interpolated_angle_byte(old.word(0x28),wheel.word(0x28),blend),relative);
        sprite.table=rom.u16(data+2)+8+wheel_roll;
        sprite.phase=rom.bytes()[0x44056+(relative>>2)];
        sprite.diameter=diameter;
        // Original08:BF53..BF8A adds contact displacement, then raises
        // the center by the definition radius plus20 in original Y-down.
        for(unsigned axis=0;axis<3;++axis) {
            const auto value=[&](const srf::VehicleState& state) {
                return std::uint16_t(state.word(0x1e + axis*2)+state.word(0x4a+axis*2));
            };
            const auto start=value(old),end=value(wheel);
            sprite.position[axis]=float(signed_word(start))+float(signed_word(std::uint16_t(end-start)))*blend;
            if(became_near)sprite.position[axis]+=out.position[axis]-float(signed_word(b.word(0x1e + axis*2)));
        }
        sprite.position[1]-=float(diameter_addition+20);
        out.sprites.push_back(sprite);
    }
    // CPU08:DCB4..DD89 reads the original attachment list. The eye callback
    // rotates its signed-byte XYZ and doubles it (08:E7E7..E890).
    if(!exterior_eyes)return out;
    const unsigned attachment_count=rom.bytes()[definition+0x60];
    for(unsigned index=0;index<attachment_count;++index) {
        const auto entry=definition+0x61+index*5;
        const auto object=0x48000+rom.u16(entry);
        if(rom.u16(object)!=0xa190) continue;
        RenderSprite sprite; sprite.table=rom.u16(object+2);
        const auto yaw_byte=(unsigned(b.word(0x24)>>8)-camera_byte+unsigned(std::uint16_t(b.word(0x3a)<<2)>>8))&255;
        sprite.phase=rom.bytes()[0x468af+(yaw_byte>>3)];
        sprite.diameter=float(srf::original_sprite_diameter(rom.u16(0x2190+16),rom.bytes()[0x2190+7],std::uint8_t(rom.u16(object+4))));
        sprite.position=out.position;
        for(unsigned a=0;a<3;++a) for(unsigned axis=0;axis<3;++axis)
            sprite.position[a]+=2.f*float(static_cast<std::int8_t>(rom.bytes()[entry+2+axis]))*out.orientation[axis*3+a];
        out.sprites.push_back(sprite);
    }
    return out;
}

std::array<RenderVehicle,4> vehicle_poses(const srf::Rom& rom,
    const std::array<srf::Vehicle,4>& before,const std::array<srf::Vehicle,4>& after,
    std::size_t player,float blend,bool show_player_eyes=true) {
    const auto camera_byte=srf::interpolated_angle_byte(before[player].body.word(0x24),after[player].body.word(0x24),blend);
    std::array<RenderVehicle,4> result{};
    for(unsigned i=0;i<result.size();++i)
        result[i]=vehicle_pose(rom,before[i],after[i],i==player,blend,camera_byte,i!=player || show_player_eyes);
    return result;
}
using RoadTriangle=std::array<std::array<double,3>,3>;
std::vector<RoadTriangle> camera_surfaces(const srf::World& world,srf::CourseId course) {
    std::vector<RoadTriangle> triangles;
    const bool road_course=course==srf::CourseId::sky_ramp || course==srf::CourseId::harbor_city;
    if(course!=srf::CourseId::lake_side && course!=srf::CourseId::big_ravine && !road_course)return triangles;
    for(const auto& instance:world.instances) {
        // Lake Side uses its banked descent. Big Ravine's two D54C hairpins
        // can put the eye behind the inner cliff while the car is on the road.
        if(course==srf::CourseId::lake_side && instance.visual_descriptor!=0xf1d8 && instance.visual_descriptor!=0xf1bc)continue;
        if(course==srf::CourseId::big_ravine && instance.visual_descriptor!=0xd54c)continue;
        if(road_course && instance.opcode!=2)continue;
        const auto model=std::find_if(world.models.begin(),world.models.end(),[&](const auto& m) {
            return m.descriptor_address==instance.visual_descriptor;
        });
        if(model==world.models.end())continue;
        for(const auto& face:model->primitives) {
            const auto material=face.attributes[2];
            if(course==srf::CourseId::lake_side && material!=44 && material!=45)continue;
            if(course==srf::CourseId::big_ravine && material!=0x5b && material!=0x5c)continue;
            if(course==srf::CourseId::sky_ramp && (material<43 || material>45))continue;
            // Include both outer lanes of the banks and the submerged roadbed.
            if(course==srf::CourseId::harbor_city && (material<0x29 || material>0x2f) && material!=0xdf)continue;
            const auto point=[&](std::size_t i) {
                return srf::world_position(model->frames.front().at(face.indices[i]),instance);
            };
            for(std::size_t i=2;i<face.indices.size();++i)
                triangles.push_back({point(0),point(i-1),point(i)});
        }
    }
    return triangles;
}
// Slide the eye along the ravine wall. Keeping the target on the car lets it
// remain visible even when driving parallel to a cliff at a tight bend.
void clear_cliff_camera(const Renderer& renderer,const RenderVehicle& vehicle,float yaw,std::span<const RoadTriangle> cliffs) {
    const float behind=900.f*std::cos(controls.pitch),lift=-900.f*std::sin(controls.pitch);
    const double eye_y=vehicle.position[1]-30.f-lift;
    const double tx=vehicle.position[0],tz=vehicle.position[2];
    double x=tx+behind*std::sin(yaw),z=tz-behind*std::cos(yaw);
    bool shifted=false;
    for(const auto& tri:cliffs) {
        std::array<std::array<double,2>,2> edge{};
        unsigned count=0;
        for(unsigned i=0;i<3;++i) {
            const auto& a=tri[i];const auto& b=tri[(i+1)%3];
            if((a[1]>eye_y)==(b[1]>eye_y))continue;
            const double t=(eye_y-a[1])/(b[1]-a[1]);
            edge[count++]={a[0]+t*(b[0]-a[0]),a[2]+t*(b[2]-a[2])};
        }
        if(count!=2)continue;
        const auto [ax,az]=edge[0];
        const double dx=edge[1][0]-ax,dz=edge[1][1]-az,len=std::hypot(dx,dz);
        if(len<1.e-4)continue;
        double nx=-dz/len,nz=dx/len;
        const double side=(tx-ax)*nx+(tz-az)*nz;
        if(side<0){nx=-nx;nz=-nz;}
        constexpr double clearance=240;
        const double eye=(x-ax)*nx+(z-az)*nz;
        if(eye>=clearance)continue;
        // Check the finite cliff at the crossing, including a radius around
        // each endpoint where two triangles meet.
        const double t=eye<0?std::abs(side)/(std::abs(side)-eye):1;
        const double along=((tx+(x-tx)*t-ax)*dx+(tz+(z-tz)*t-az)*dz)/len;
        const double beyond=std::max({0.,-along,along-len});
        if(beyond>=clearance)continue;
        const double margin=std::sqrt(clearance*clearance-beyond*beyond);
        const double correction=std::clamp(margin-eye,0.,clearance);
        x+=nx*correction;z+=nz*correction;shifted|=correction>0;
    }
    if(shifted) {
        const auto horizontal=static_cast<float>(std::hypot(x-tx,z-tz));
        controls.yaw=static_cast<float>(std::atan2(x-tx,tz-z));
        controls.pitch=-std::atan2(lift,horizontal);
        controls.distance=renderer.normalized_distance(std::hypot(horizontal,lift));
    }
}
void chase_camera(const Renderer& renderer,const RenderVehicle& vehicle,float yaw,std::span<const RoadTriangle> road,
                  srf::CourseId course) {
    const auto target=renderer.normalize_world({vehicle.position[0],vehicle.position[1]-30,vehicle.position[2]});
    controls.target_x=target[0]; controls.target_y=target[1]; controls.target_z=target[2];
    controls.yaw=yaw; controls.pitch=-.18f; controls.zoom=2.f;
    controls.distance=renderer.normalized_distance(900.f);
    if(road.empty())return;
    if(course==srf::CourseId::big_ravine){clear_cliff_camera(renderer,vehicle,yaw,road);return;}
    const float behind=900.f*std::cos(controls.pitch);
    const float x=vehicle.position[0]+behind*std::sin(yaw),z=vehicle.position[2]-behind*std::cos(yaw);
    const float target_y=vehicle.position[1]-30.f;
    const float original_lift=-900.f*std::sin(controls.pitch);
    float lift=original_lift,target_lift=0;
    for(const auto& [a,b,c]:road) {
        const double det=(b[2]-c[2])*(a[0]-c[0])+(c[0]-b[0])*(a[2]-c[2]);
        if(std::abs(det)<1.e-6)continue;
        const auto weights=[&](float px,float pz) {
            return std::array{((b[2]-c[2])*(px-c[0])+(c[0]-b[0])*(pz-c[2]))/det,
                              ((c[2]-a[2])*(px-c[0])+(a[0]-c[0])*(pz-c[2]))/det};
        };
        const auto height=[&](double u,double v){return u*a[1]+v*b[1]+(1-u-v)*c[1];};
        if(course==srf::CourseId::harbor_city) {
            // An overhead deck can cover the same X/Z as the player's road.
            // Compare its plane at the car, so a steep approach behind the
            // car still lifts the eye when the player is driving down it.
            const auto [cu,cv]=weights(vehicle.position[0],vehicle.position[2]);
            const float car_ground=static_cast<float>(height(cu,cv));
            if(car_ground<target_y-original_lift)continue;
            // During compression the body origin can dip below the deck.
            // Its painted stripes sit up to 32 units above the sloped mesh.
            if(cu>=0 && cv>=0 && cu+cv<=1)
                target_lift=std::max(target_lift,target_y-car_ground+32.f);
        }
        const auto [u,v]=weights(x,z);
        if(u<0 || v<0 || u+v>1)continue;
        const float ground=static_cast<float>(height(u,v));
        // Original Y grows down. Keep the eye above the local surface while
        // retaining its horizontal position and looking at the same car.
        lift=std::max(lift,target_y-ground+96.f);
    }
    controls.target_y+=renderer.normalized_distance(target_lift);
    lift=std::max(original_lift,lift-target_lift);
    if(lift>original_lift) {
        controls.pitch=-std::atan2(lift,behind);
        controls.distance=renderer.normalized_distance(std::hypot(behind,lift));
    }
}
std::uint16_t keyboard_control(HWND window) {
    if(GetForegroundWindow()!=window) return 0;
    const auto down=[](int key){return (GetAsyncKeyState(key)&0x8000)!=0;};
    std::uint16_t result=0;
    if(down('W')||down(VK_UP)) result|=0x10;
    if(down('S')||down(VK_DOWN)) result|=0x40;
    if(down('A')||down(VK_LEFT)) result|=1;
    if(down('D')||down(VK_RIGHT)) result|=2;
    if(down(VK_SPACE)) result|=0x400;
    if(down('X')) result|=0x20;
    return result;
}
struct RecordedInput { std::uint16_t buttons; std::uint8_t ticks; bool requested; };
std::vector<RecordedInput> read_inputs(const std::filesystem::path& path) {
    std::ifstream file(path);std::string line;
    if(!std::getline(file,line)) throw std::runtime_error("Input CSV has no header");
    if(!line.empty() && line.back()=='\r') line.pop_back();
    const bool requested=line=="phase,requested,clock_ticks";
    if(!requested && line!="phase,controls,clock_ticks")
        throw std::runtime_error("Input CSV requires phase,requested,clock_ticks or phase,controls,clock_ticks header");
    std::vector<RecordedInput> result;
    while(std::getline(file,line)) {
        std::istringstream row(line);unsigned phase=0,buttons=0,ticks=0;char a=0,b=0;
        if(!(row>>phase>>a>>buttons>>b>>ticks) || a!=',' || b!=',' || phase!=result.size() || buttons&~(requested?0x473u:0xfc73u) || ticks>255)
            throw std::runtime_error("Invalid input CSV row or nonsequential phase");
        row>>std::ws;if(!row.eof()) throw std::runtime_error("Trailing input CSV fields");
        result.push_back({static_cast<std::uint16_t>(buttons),static_cast<std::uint8_t>(ticks),requested});
        if(result.size()>1000000) throw std::runtime_error("Input CSV phase limit exceeded");
    }
    if(result.empty()) throw std::runtime_error("Input CSV has no controls");
    return result;
}
std::wstring race_status(const srf::NativeRace& race,double simulation_hz,bool championship=false,
                         std::optional<unsigned> grid_seconds={},bool held_on_grid=false) {
    const auto& body=race.racers()[race.player_index()].body;
    std::wostringstream text;
    const auto name=srf::course_definition(race.course_id()).name;
    text<<L"  "<<std::wstring(name.begin(),name.end())<<L" — NATIVE RACE    "<<simulation_hz<<L" Hz prototype pace\r\n"
        <<L"  Lap "<<std::min(3u,unsigned(body.word(0x7a))+1)<<L" / 3    Position "<<race.standings()[race.player_index()]<<L" / 4"
        <<L"    Time left "<<grid_seconds.value_or(race.remaining_seconds())<<L" s"
        <<L"    Time "<<race.elapsed_centiseconds()/6000<<L":"<<std::setfill(L'0')<<std::setw(2)<<(race.elapsed_centiseconds()/100)%60
        <<L"."<<std::setw(2)<<race.elapsed_centiseconds()%100<<std::setfill(L' ')
        <<L"\r\n  Speed (native) "<<signed_word(body.word(0x4a))<<L"    Boost "<<body.word(0x6a)
        <<L"    Health "<<std::max(0,512-int(body.word(0x82)))<<L" / 512"
        <<L"    "<<(held_on_grid || race.countdown_active()?L"START LIGHTS  -  HOLD":
                      race.time_over_pending()?L"TIME OVER":L"GO")
        <<L"    "<<(race.completion().finished?L"FINISHED — R TO RESTART":controls.paused?L"PAUSED":L"")
        <<L"\r\n  W / UP accelerate   S / DOWN or X brake/reverse   A/D steer   SPACE boost   "
        <<(championship?L"R retire":L"R restart")<<L"   P pause   H status   F11 fullscreen   ESC back/exit";
    if(race.failure()==srf::RaceFailure::wrecked) text<<L"\r\n  WRECKED - R TO RESTART";
    if(race.failure()==srf::RaceFailure::time_out) text<<L"\r\n  TIME OUT - R TO RESTART";
    if(race.failure()==srf::RaceFailure::out_of_bounds) text<<L"\r\n  OUT OF COURSE - R TO RESTART";
    return text.str();
}
}

#include "speed_bonus_window.inc"
#include "boot_window_input.inc"
#include "course_map_window.inc"
#include "boot_window.inc"
#include "grid_countdown_art.hpp"
#include "grid_countdown_view.inc"
#include "rock_field_window.inc"
#include "bonnet_drive.inc"

int wmain(int argc,wchar_t** argv) {
    bool launch_game=false,vr=false;
    std::filesystem::path screenshot;
    try {
        std::filesystem::path assets,world_path,rom_path,trace_path,input_path,controller_path,progress_path;
        bool validate=false,restart_check=false,borderless=false,screenshot_no_hud=false;
        auto player_car=srf::PlayerCar::four_wd;
        auto course=srf::CourseId::easy_ride;
        unsigned descriptor=0;
        unsigned steps=0,fixed_control=0;
        double simulation_hz=14;
        unsigned vr_refresh=0,vr_scale=100;
        for(int i=1;i<argc;++i) {
            const std::wstring arg=argv[i];
            if(arg==L"--assets" && i+1<argc) assets=argv[++i];
            else if(arg==L"--world" && i+1<argc) { world_path=argv[++i]; controls.world=true; controls.yaw=0; controls.pitch=-1.1f; }
            else if(arg==L"--screenshot" && i+1<argc) screenshot=argv[++i];
            else if(arg==L"--model" && i+1<argc) controls.model=std::stoul(argv[++i]);
            else if(arg==L"--descriptor" && i+1<argc) descriptor=std::stoul(argv[++i],nullptr,0);
            else if(arg==L"--race") controls.race=true;
            else if(arg==L"--vr") vr=true;
            else if(arg==L"--game") { launch_game=true; controls.race=true; }
            else if(arg==L"--course" && i+1<argc) {
                const std::wstring value=argv[++i];
                course=srf::parse_course(std::string(value.begin(),value.end()));
            }
            else if(arg==L"--car" && i+1<argc) {
                const std::wstring car=argv[++i];
                if(car==L"4wd")player_car=srf::PlayerCar::four_wd;
                else if(car==L"f-type")player_car=srf::PlayerCar::f_type;
                else if(car==L"coupe")player_car=srf::PlayerCar::coupe;
                else throw std::runtime_error("Supported cars: 4wd, f-type, coupe");
            }
            else if(arg==L"--borderless")borderless=true;
            else if(arg==L"--screenshot-no-hud")screenshot_no_hud=true;
            else if(arg==L"--rom" && i+1<argc) rom_path=argv[++i];
            else if(arg==L"--steps" && i+1<argc) steps=std::stoul(argv[++i]);
            else if(arg==L"--control" && i+1<argc) fixed_control=std::stoul(argv[++i],nullptr,0);
            else if(arg==L"--simulation-hz" && i+1<argc) simulation_hz=std::stod(argv[++i]);
            else if((arg==L"--desktop-fps" || arg==L"--vr-hz" || arg==L"--vr-scale") && i+1<argc){
                const std::wstring value=argv[++i];
                if(value.empty() || value.size()>3 || value.find_first_not_of(L"0123456789")!=std::wstring::npos)
                    throw std::runtime_error("Frame rate and eye scale require an integer.");
                const unsigned n=std::stoul(value);
                if(arg==L"--vr-scale")vr_scale=n;
                else if(arg==L"--vr-hz")vr_refresh=n;
                else desktop_fps=n;
            }
            else if(arg==L"--restart-check") restart_check=true;
            else if(arg==L"--replay-input" && i+1<argc) input_path=argv[++i];
            else if(arg==L"--controller-config" && i+1<argc) controller_path=argv[++i];
            else if(arg==L"--progress-file" && i+1<argc) progress_path=argv[++i];
            else if(arg==L"--trace" && i+1<argc) trace_path=argv[++i];
            else if(arg==L"--yaw" && i+1<argc) controls.yaw=std::stof(argv[++i]);
            else if(arg==L"--pitch" && i+1<argc) controls.pitch=std::stof(argv[++i]);
            else if(arg==L"--zoom" && i+1<argc) controls.zoom=std::stof(argv[++i]);
            else if(arg==L"--width" && i+1<argc) controls.width=std::stoul(argv[++i]);
            else if(arg==L"--height" && i+1<argc) controls.height=std::stoul(argv[++i]);
            else if(arg==L"--validate-assets") validate=true;
            else throw std::runtime_error("Usage: srf_preview --assets <models.srfmesh> [--world <world.srfworld>] [--game | --race] [--rom <original.sfc> --course easy-ride|aqua-tunnel|sunset-valley|night-owl|kings-forest|sea-breeze|white-land|night-cruise|lake-side|big-ravine|sky-ramp|harbor-city --car 4wd|coupe|f-type] [--borderless] [--validate-assets | --screenshot <new.png>] [--steps count --control native_word --trace new.jsonl] [--replay-input controls.csv --restart-check] [--simulation-hz 14] [--controller-config path] [--model index --descriptor address --yaw radians --pitch radians --zoom factor --width pixels --height pixels]");
        }
        if(vr && (launch_game || !controls.race || course!=srf::CourseId::easy_ride || player_car!=srf::PlayerCar::four_wd || !screenshot.empty() || validate || steps || restart_check || !trace_path.empty() || fixed_control))
            throw std::runtime_error("Easy Ride VR requires an interactive 4WD race.");
        if((desktop_fps && (desktop_fps<30 || desktop_fps>240)) || (vr_refresh && (vr_refresh<60 || vr_refresh>240)) ||
           vr_scale<50 || vr_scale>100 || (!vr && (vr_refresh || vr_scale!=100)) || (vr && desktop_fps))
            throw std::runtime_error("Invalid frame settings for the selected display mode.");
        if(simulation_hz!=14)
            throw std::runtime_error("This build supports 14 simulation phases/s. Changing callback frequency speeds up the game; faithful 120 Hz integration is not yet implemented.");
        if(!std::isfinite(simulation_hz) || simulation_hz<1 || simulation_hz>120 || steps>1000000 || fixed_control>65535 ||
           !std::isfinite(controls.yaw) || !std::isfinite(controls.pitch) || !std::isfinite(controls.zoom) || controls.zoom<=0 || controls.zoom>80 || controls.width<64 || controls.width>7680 || controls.height<64 || controls.height>4320)
            throw std::runtime_error("Camera or image dimensions are out of range");
        if(assets.empty() && !world_path.empty()) assets=world_path.parent_path()/"models.srfmesh";
        if(assets.empty()) throw std::runtime_error("Select a generated mesh package with --assets");
        if(launch_game && (world_path.empty() || rom_path.empty() || descriptor || validate || steps || fixed_control || restart_check || !trace_path.empty() || !input_path.empty()))
            throw std::runtime_error("Launch Game requires --world and --rom; replay and inspection options belong to --race");
        const auto package_root=course==srf::CourseId::easy_ride?assets.parent_path():assets.parent_path().parent_path();
        const auto launch_controls=controls;
        if(progress_path.empty()) {
            const wchar_t* local=_wgetenv(L"LOCALAPPDATA");
            progress_path=(local?std::filesystem::path(local)/"StuntRaceFXPC":package_root)/"speed-trax.sav";
        }
        srf::ChampionshipProgress progress;
        if(launch_game && screenshot.empty()) {
            SetProcessDPIAware();
            try {progress=srf::ChampionshipProgress::load(progress_path);}
            catch(const std::exception& error) {
                MessageBoxA(nullptr,error.what(),"Speed Trax could not read your class records",MB_OK|MB_ICONWARNING);
            }
        }
        std::optional<srf::Championship> championship;
        std::unique_ptr<srf::AudioDevice> next_grid_audio;
        srf::GameSessionChoice selection;
        bool cold_boot=true;
        auto stunt_return=srf::StuntMenuReturn::game_selection;
        const auto stunt_scores_path=[&](srf::StuntCourse course) {
            return progress_path.parent_path()/(std::string(srf::stunt_course_id(course))+".sav");
        };
        std::array<std::vector<srf::StuntRecord>,srf::playable_stunt_courses.size()> stunt_scores;
        if(launch_game && screenshot.empty()) {
            for(const auto course:srf::playable_stunt_courses) {
                try {stunt_scores[static_cast<unsigned>(course)]=srf::load_stunt_scores(stunt_scores_path(course),course);}
                catch(const std::exception& error){MessageBoxA(nullptr,error.what(),"Stunt Trax could not read your records",MB_OK|MB_ICONWARNING);}
            }
        }
        for(;;) {
        if(launch_game) {
            if(!championship) {
                const auto rom=srf::Rom::load(rom_path);
                if(!screenshot.empty()){capture_boot_title(rom,screenshot);return 0;}
                if(!run_boot_menus(rom,selection,cold_boot,controller_path,borderless,next_grid_audio,stunt_return,stunt_scores))return 0;
                cold_boot=false;
                stunt_return=srf::StuntMenuReturn::game_selection;
                if(selection.stunt_trax) {
                    next_grid_audio.reset();
                    StuntExit exit;
                    auto attempt=srf::StuntAttempt::first;
                    for(;;) {
                        exit=play_stunt_course(rom,selection,attempt,stunt_scores_path(selection.stunt_course),
                            stunt_scores.at(static_cast<unsigned>(selection.stunt_course)),controller_path,
                            launch_controls.width,launch_controls.height,borderless);
                        if(exit==StuntExit::retry_finish)attempt=srf::StuntAttempt::after_finish;
                        else if(exit==StuntExit::retry_timeout)attempt=srf::StuntAttempt::after_timeout;
                        else if(exit==StuntExit::restart)attempt=srf::StuntAttempt::first;
                        else break;
                    }
                    if(exit==StuntExit::quit)return 0;
                    if(exit==StuntExit::course)stunt_return=srf::StuntMenuReturn::course;
                    continue;
                }
                const auto league=show_initial_course_map(rom,progress,selection.car,controller_path,
                    launch_controls.width,launch_controls.height,borderless,next_grid_audio,selection.league);
                if(!league)return 0;
                selection.league=*league;
                championship.emplace(*league,selection.car,selection.initials);
            }
            course=championship->course();
            player_car=selection.car;
            const auto directory=course==srf::CourseId::easy_ride?package_root:package_root/std::string(srf::course_definition(course).slug);
            assets=directory/"models.srfmesh";world_path=directory/"world.srfworld";
            controls=launch_controls;
        }
        const auto& course_package=srf::generated::course_packages[static_cast<unsigned>(course)];
        const auto& course_definition=srf::course_definition(course);
        const std::wstring course_name(course_definition.name.begin(),course_definition.name.end());
        const auto bytes=read_file(assets);
        if(srf::sha256(bytes)!=srf::generated::mesh_sha256) throw std::runtime_error("Mesh package does not match this locally compiled preview");
        const auto models=srf::decode_meshes(bytes);
        if(models.empty() || models.size()!=srf::generated::model_count) throw std::runtime_error("Mesh manifest mismatch");
        const auto scene_bytes=read_file(assets.parent_path()/(std::string(course_definition.slug)+"-scene.json"));
        if(srf::sha256(scene_bytes)!=course_package.scene_sha256) throw std::runtime_error("Scene report does not match this locally compiled preview");
        srf::World world;
        std::vector<srf::WorldSprite> sprites, dusk_sprites;
        std::size_t world_polygon_instances=0,world_primitives=0;
        if(!world_path.empty()) {
            const auto world_bytes=read_file(world_path);
            if(srf::sha256(world_bytes)!=course_package.world_sha256) throw std::runtime_error("World package does not match this locally compiled preview");
            world=srf::decode_world(world_bytes);
            const auto sprite_bytes=read_file(world_path.parent_path()/"sprites.srfsprites");
            if(srf::sha256(sprite_bytes)!=course_package.sprite_sha256) throw std::runtime_error("Original textures do not match this locally compiled preview");
            sprites=srf::decode_world_sprites(sprite_bytes);
            if (*course_package.dusk_sprite_sha256) {
                const auto dusk_bytes=read_file(world_path.parent_path()/"sprites-dusk.srfsprites");
                if(srf::sha256(dusk_bytes)!=course_package.dusk_sprite_sha256)
                    throw std::runtime_error("Dusk textures do not match this locally compiled preview");
                dusk_sprites=srf::decode_world_sprites(dusk_bytes);
            }
            if(world.models.size()!=course_package.models || world.instances.size()!=course_package.instances) throw std::runtime_error("World manifest mismatch");
            for(const auto& instance:world.instances) {
                const auto model=std::find_if(world.models.begin(),world.models.end(),[&](const auto& m){return m.descriptor_address==instance.visual_descriptor;});
                if(model!=world.models.end() && !model->primitives.empty()) { ++world_polygon_instances; world_primitives+=model->primitives.size(); }
            }
        }
        if(validate) {
            std::cout << "{\"validated\":true,\"model_count\":" << models.size()
                      << ",\"scene_object_count\":" << course_package.objects
                      << ",\"scene_explicit_descriptor_meshes\":" << course_package.explicit_meshes
                      << ",\"scene_coordinate_count\":" << course_package.points
                      << ",\"scene_visual_layout_ready\":true"
                      << ",\"world_model_count\":" << world.models.size()
                      << ",\"world_polygon_instances\":" << world_polygon_instances
                      << ",\"world_placed_primitives\":" << world_primitives
                      << ",\"course\":" << srf::json_string(std::string(course_definition.slug))
                      << ",\"native_gameplay_available\":true,\"first_milestone_complete\":false,\"renderer\":\"D3D11 native race and model inspection\"}\n";
            return 0;
        }
        if(descriptor) {
            if(!controls.world || descriptor>65535 || std::none_of(world.models.begin(),world.models.end(),[&](const auto& model){return model.descriptor_address==descriptor && !model.primitives.empty();}))
                throw std::runtime_error("Select a renderable visual descriptor from --world");
            world.instances={{0,static_cast<std::uint16_t>(descriptor),0,0,{0,0,0}}};
        }
        if(controls.model>=models.size()) throw std::runtime_error("Model index is out of range");
        std::unique_ptr<srf::Rom> race_rom;
        std::unique_ptr<srf::NativeRace> race;
        std::unique_ptr<srf::CourseScenery> scenery;
        std::unique_ptr<srf::CourseEnvironment> environment;
        std::optional<srf::RainParticles> rain;
        bool rain_changed=false;
        auto audio=std::move(next_grid_audio);
        bool grid_fanfare=bool(audio);
        std::optional<srf::GridCountdown> grid_countdown;
        std::optional<srf::GridCountdownArt> grid_art;
        std::vector<std::uint32_t> grid_pixels;
        std::array<float,3> grid_origin{};
        float grid_heading=0;
        std::string audio_failure;
        const auto camera_road=controls.race?camera_surfaces(world,course):std::vector<RoadTriangle>{};
        srf::RaceAudioState race_audio_state;
        srf::RaceAudioTuning race_audio_tuning;
        std::array<srf::Vehicle,4> previous{};
        std::ofstream trace;
        const auto recorded=input_path.empty()?std::vector<RecordedInput>{}:read_inputs(input_path);
        std::size_t input_index=0;
        std::array<std::uint32_t,3> lap_times{};
        std::uint32_t last_lap_end=0;
        bool retired=false;
        if(!recorded.empty() && !screenshot.empty() && !steps) steps=static_cast<unsigned>(recorded.size());
        if((!recorded.empty() && (fixed_control || steps>recorded.size())) || (restart_check && screenshot.empty()))
            throw std::runtime_error("Replay inputs cannot be combined with fixed controls or exceeded; restart check requires screenshot");
        if(steps && screenshot.empty()) throw std::runtime_error("--steps requires --screenshot for a bounded offscreen simulation");
        if(controls.race) {
            if(world_path.empty() || rom_path.empty() || descriptor) throw std::runtime_error("Development race requires --world and --rom, without --descriptor");
            race_rom=std::make_unique<srf::Rom>(srf::Rom::load(rom_path));
            auto slots=championship?championship->starting_grid():std::nullopt;
            if(championship && championship->round()==1)slots=srf::GridOrder{4,3,2,1};
            race=std::make_unique<srf::NativeRace>(*race_rom,player_car,course,
                championship?std::optional(championship->carry_seconds()):std::nullopt,
                slots);
            scenery=std::make_unique<srf::CourseScenery>(*race_rom,course);
            environment=std::make_unique<srf::CourseEnvironment>(*race_rom,course);
            if(course==srf::CourseId::big_ravine)rain.emplace(*race_rom);
            race_audio_tuning=srf::extract_race_audio_tuning(race_rom->bytes(),race->racers()[race->player_index()].body.word(0x66));
            previous=race->racers();
            if(slots) {
                const unsigned carry=championship->carry_seconds();
                const auto round=championship->round()==1?srf::GridCountdownRound::first:
                    championship->round()==3?srf::GridCountdownRound::third:srf::GridCountdownRound::fourth;
                grid_countdown.emplace(unsigned(championship->league()),unsigned(player_car),
                    race->remaining_seconds()-carry,carry,championship->credits(),*slots,round);
                grid_art.emplace(*race_rom,course,player_car);
                grid_pixels.resize(208*128);
                const auto& body=previous[race->player_index()].body;
                for(unsigned i=0;i<3;++i)grid_origin[i]=float(signed_word(body.word(0x1e + i*2)));
                grid_heading=lerp_angle(body.word(0x24),body.word(0x24),1);
            }
            if(!trace_path.empty()) {
                if(std::filesystem::exists(trace_path)) throw std::runtime_error("Trace exists; select a new path");
                trace.open(trace_path);
                if(!trace) throw std::runtime_error("Cannot write native race trace");
            }
        } else if(steps || fixed_control || !trace_path.empty() || !input_path.empty() || restart_check) throw std::runtime_error("Simulation arguments require --race");
        double countdown_clock=0;
        std::uint64_t phase_video_ticks=0,environment_video_ticks=0;
        const auto advance_environment_to=[&](std::uint64_t target) {
            if(target>environment_video_ticks) {
                environment->advance_video(static_cast<unsigned>(target-environment_video_ticks));
                environment_video_ticks=target;
            }
        };
        const auto step_race=[&](std::uint16_t input,std::optional<std::int16_t> steering=std::nullopt,
                                 std::optional<std::uint8_t> locked_grid_ticks=std::nullopt,
                                 std::array<srf::Vehicle,4>* halfway=nullptr) {
            bool map_wishes=recorded.empty() && screenshot.empty();
            auto ticks=locked_grid_ticks.value_or(0);
            if(!locked_grid_ticks) {
                countdown_clock+=60.0988/simulation_hz;
                ticks=static_cast<std::uint8_t>(std::floor(countdown_clock));
                countdown_clock-=ticks;
            }
            if(!recorded.empty() && !locked_grid_ticks) {
                steering.reset();
                if(input_index>=recorded.size()) {controls.paused=true;return;}
                input=recorded[input_index].buttons;ticks=recorded[input_index].ticks;
                map_wishes=recorded[input_index].requested;++input_index;
            }
            const auto previous_stage=race->countdown_stage();
            const auto previous_lap=race->completion().acknowledged_laps;
            const auto previous_phase=race->phase();
            const bool raw_throttle=(input&0x10)!=0;
            srf::RaceAudioOutput input_events;
            const auto& player_before=race->racers()[race->player_index()].body;
            if(map_wishes)
                input=srf::prepare_player_input(race_audio_state,player_before,input,race->countdown_active(),input_events);
            else input=srf::filter_player_boost(player_before,input);
            previous=race->racers(); race->step(input,locked_grid_ticks?0:ticks,steering,halfway);
            if(championship && race->completion().acknowledged_laps>previous_lap && previous_lap<lap_times.size()) {
                const auto elapsed=race->elapsed_centiseconds();
                lap_times[previous_lap]=elapsed-last_lap_end;
                last_lap_end=elapsed;
            }
            if(previous_phase!=race->phase()) {
                phase_video_ticks+=ticks;advance_environment_to(phase_video_ticks);
                scenery->step(previous[race->player_index()].body,previous_lap);
                environment->step(previous[race->player_index()].body,previous_lap,race->elapsed_centiseconds());
                if(rain) {
                    const auto& player=race->racers()[race->player_index()].body;
                    rain->step((player.word(0x8a)&1)!=0,static_cast<std::int16_t>(-signed_word(player.word(0x24))),
                               static_cast<std::int16_t>(player.word(0x4a)));
                    rain_changed=true;
                }
            }
            if(audio && previous_phase!=race->phase()) {
                try {
                    const auto& player=race->racers()[race->player_index()];
                    srf::RaceAudioInput sound_input;
                    sound_input.prestart=race->countdown_active() || (grid_countdown && !grid_countdown->frame().driving);
                    sound_input.throttle=raw_throttle;
                    sound_input.counter=static_cast<std::uint16_t>(race->phase());
                    if(course==srf::CourseId::sea_breeze || course==srf::CourseId::white_land || course==srf::CourseId::night_cruise ||
                       course==srf::CourseId::lake_side || course==srf::CourseId::big_ravine || course==srf::CourseId::sky_ramp ||
                       course==srf::CourseId::harbor_city)
                        sound_input.terrain_flags=player.body.word(0x8a);
                    auto events=srf::update_race_audio(race_audio_state,race_audio_tuning,player,sound_input);
                    events.effects.insert(events.effects.begin(),input_events.effects.begin(),input_events.effects.end());
                    if(!grid_countdown)srf::append_countdown_audio(previous_stage,race->countdown_stage(),events);
                    const auto& surroundings=environment->events();
                    events.commands.insert(events.commands.end(),surroundings.sound_commands.begin(),surroundings.sound_commands.end());
                    events.effects.insert(events.effects.end(),surroundings.effects.begin(),surroundings.effects.end());
                    const auto scenery_sounds=scenery->effects();
                    events.effects.insert(events.effects.end(),scenery_sounds.begin(),scenery_sounds.end());
                    for(const auto sounds:{race->rock_effects(),race->cone_effects(),race->prop_effects(),race->pickup_effects(),race->breakable_effects(),race->bonus_effects()})
                        events.effects.insert(events.effects.end(),sounds.begin(),sounds.end());
                    audio->update(events,sound_input.throttle,(player.body.word(0x42)&0x400)!=0,
                        !race->ended(),(grid_fanfare || !race->countdown_active()) && !race->ended());
                } catch(const std::exception& error) {audio_failure=error.what();audio.reset();}
            }
            if(trace.is_open()) {
                const auto& b=race->racers()[race->player_index()].body;
                trace<<"{\"phase\":"<<race->phase()<<",\"input\":"<<input<<",\"position\":["
                     <<signed_word(b.word(0x1e))<<','<<signed_word(b.word(0x20))<<','<<signed_word(b.word(0x22))
                     <<"],\"yaw\":"<<b.word(0x24)<<",\"speed\":"<<signed_word(b.word(0x4a))
                     <<",\"clock_ticks\":"<<unsigned(ticks)<<",\"lap\":"<<b.word(0x7a)
                     <<",\"damage\":"<<b.word(0x82)<<",\"finished\":"<<(race->completion().finished?"true":"false")
                     <<",\"failure\":"<<unsigned(race->failure())<<"}\n";
            }
        };
        unsigned last_grid_update=0;
        const auto settle_grid=[&](std::uint16_t input) {
            const auto& frame=grid_countdown->frame();
            // Actor creation followed GSU phase 1. The first visible grid already
            // contains completed updates, and parked opponents settle until green.
            while(race->phase()+1<frame.scene_phase) {
                step_race(input,std::nullopt,static_cast<std::uint8_t>(frame.tick-last_grid_update));
                last_grid_update=frame.tick;
                previous=race->racers();
            }
        };
        if(grid_countdown)settle_grid(0);
        controls.count=models.size();
        check(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED),"COM initialization");
        HWND window=nullptr;
        if(screenshot.empty()) {
            WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=window_proc;
            wc.lpszClassName=L"StuntRaceFXMeshInspector"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Window registration failed");
            RECT window_rect{0,0,static_cast<LONG>(controls.width),static_cast<LONG>(controls.height)};
            AdjustWindowRect(&window_rect,WS_OVERLAPPEDWINDOW,FALSE);
            window=CreateWindowExW(0,wc.lpszClassName,L"Stunt Race FX native model research",WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,CW_USEDEFAULT,window_rect.right-window_rect.left,window_rect.bottom-window_rect.top,nullptr,nullptr,wc.hInstance,nullptr);
            if(!window) throw std::runtime_error("Window creation failed");
        }
        if(vr) {
            // The companion accepts keyboard controls. Only OpenXR drives presentation.
            SetWindowTextW(window,L"Stunt Race FX — Easy Ride VR");
            SetWindowPos(window,nullptr,0,0,560,154,SWP_NOMOVE|SWP_NOZORDER);
            CreateWindowW(L"STATIC",L"Easy Ride is running in the headset.\r\nF2 / right stick: recenter     R / View: restart     P / Start: pause     Esc: exit",
                WS_CHILD|WS_VISIBLE,12,14,520,96,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            ShowWindow(window,SW_SHOW);
            std::ofstream diagnostics(L"openxr-last-drive.log");
            try {
                srf::vr::Drive drive(diagnostics,vr_refresh,vr_scale);
                Renderer renderer(nullptr,drive.device());
                renderer.set_course_colors(course_package); renderer.load_textures(sprites,dusk_sprites);
                renderer.load_world(world,0);
                try {audio=std::make_unique<srf::AudioDevice>(srf::extract_course_audio(*race_rom,course));
                    audio->update({},false,false,true,false);}
                catch(const std::exception& e){diagnostics<<"Audio: "<<e.what()<<std::endl;audio.reset();}
                run_bonnet_drive(window,drive,renderer,*race_rom,*race,*scenery,*environment,world,controller_path,
                    audio,step_race,[&]{
                        race->reset();previous=race->racers();countdown_clock=0;race_audio_state={};
                        input_index=0;
                        scenery->reset();environment->reset();phase_video_ticks=environment_video_ticks=0;
                        if(audio){audio->restart();audio->update({},false,false,true,false);}
                    },diagnostics);
            } catch(const std::exception& e){diagnostics<<e.what()<<std::endl;throw;}
            if(IsWindow(window))DestroyWindow(window);CoUninitialize();return 0;
        }
        Renderer renderer(window,nullptr,(window && race)?desktop_fps:0);
        renderer.set_course_colors(course_package);
        renderer.load_textures(sprites,dusk_sprites);
        if(race)renderer.load_backdrop(srf::extract_course_backdrop(*race_rom,course));
        if(controls.world) renderer.load_world(world,0); else renderer.load(models[controls.model],0);
        if(!screenshot.empty()) {
            if(race) {
                for(unsigned i=0;i<steps;++i) step_race(static_cast<std::uint16_t>(fixed_control));
                const auto poses=vehicle_poses(*race_rom,previous,race->racers(),race->player_index(),1);
                const auto& b=race->racers()[race->player_index()].body;
                chase_camera(renderer,poses[race->player_index()],lerp_angle(b.word(0x24),b.word(0x24),1),camera_road,course);
                renderer.set_environment(environment->presentation());
                renderer.load_race_world(world,poses,*race_rom,*race,*scenery);
                if(rain)renderer.set_rain(rain->pixels());
            }
            renderer.render(); renderer.screenshot(screenshot,race&&!screenshot_no_hud?race_status(*race,simulation_hz):L"");
            if(race) {
                const auto final=race->racers();const auto final_phase=race->phase();
                const auto final_scenery=std::vector<srf::SceneryPose>(scenery->poses().begin(),scenery->poses().end());
                const auto final_rocks=std::vector<srf::SceneryPose>(race->rock_poses().begin(),race->rock_poses().end());
                const auto final_cones=std::vector<srf::SceneryPose>(race->cone_poses().begin(),race->cone_poses().end());
                const auto final_props=std::vector<srf::SceneryPose>(race->prop_poses().begin(),race->prop_poses().end());
                const auto final_bonus=std::vector<srf::SceneryPose>(race->bonus_poses().begin(),race->bonus_poses().end());
                const auto final_pickups=std::vector<srf::SceneryPose>(race->pickup_poses().begin(),race->pickup_poses().end());
                const auto final_spray=std::vector<srf::ScenerySprite>(race->spray_sprites().begin(),race->spray_sprites().end());
                const auto final_breakables=std::vector<srf::ScenerySprite>(race->breakable_sprites().begin(),race->breakable_sprites().end());
                const auto final_bridges=std::vector<srf::SceneryPose>(race->bridge_poses().begin(),race->bridge_poses().end());
                const auto final_presentation=environment->presentation();
                const auto final_environment=environment->state();
                const auto final_video=environment->video_state();
                const auto finished=race->completion().finished;const auto failure=race->failure();
                if(restart_check) {
                    std::vector<srf::RainPixel> final_rain;
                    if(rain){final_rain.assign(rain->pixels().begin(),rain->pixels().end());rain->reset();}
                    race->reset();input_index=0;countdown_clock=0;previous=race->racers();race_audio_state={};
                    scenery->reset();environment->reset();
                    phase_video_ticks=environment_video_ticks=0;
                    if(race->phase()!=0 || race->ended()) throw std::runtime_error("Native restart failed to reset race lifecycle");
                    if(trace) trace.close();
                    for(unsigned i=0;i<steps;++i) step_race(static_cast<std::uint16_t>(fixed_control));
                    if(race->phase()!=final_phase || race->completion().finished!=finished || race->failure()!=failure)
                        throw std::runtime_error("Native restart repeat changed race outcome");
                    if(!std::equal(final_scenery.begin(),final_scenery.end(),scenery->poses().begin(),scenery->poses().end()) ||
                       !std::equal(final_rocks.begin(),final_rocks.end(),race->rock_poses().begin(),race->rock_poses().end()) ||
                       !std::equal(final_cones.begin(),final_cones.end(),race->cone_poses().begin(),race->cone_poses().end()) ||
                       !std::equal(final_props.begin(),final_props.end(),race->prop_poses().begin(),race->prop_poses().end()) ||
                       !std::equal(final_bonus.begin(),final_bonus.end(),race->bonus_poses().begin(),race->bonus_poses().end()) ||
                       !std::equal(final_pickups.begin(),final_pickups.end(),race->pickup_poses().begin(),race->pickup_poses().end()) ||
                       !std::equal(final_spray.begin(),final_spray.end(),race->spray_sprites().begin(),race->spray_sprites().end()) ||
                       !std::equal(final_breakables.begin(),final_breakables.end(),race->breakable_sprites().begin(),race->breakable_sprites().end()) ||
                       !std::equal(final_bridges.begin(),final_bridges.end(),race->bridge_poses().begin(),race->bridge_poses().end()) ||
                       final_environment!=environment->state() || final_video!=environment->video_state() ||
                       (rain && !std::ranges::equal(final_rain,rain->pixels())) ||
                       final_presentation!=environment->presentation())
                        throw std::runtime_error("Native restart repeat changed course scenery/environment");
                    for(unsigned i=0;i<4;++i) {
                        if(race->racers()[i].body.words!=final[i].body.words) throw std::runtime_error("Native restart repeat changed vehicle state");
                        for(unsigned j=0;j<4;++j) if(race->racers()[i].wheels[j].words!=final[i].wheels[j].words)
                            throw std::runtime_error("Native restart repeat changed wheel state");
                    }
                }
                std::cout<<"{\"native_phases\":"<<final_phase<<",\"finished\":"<<(finished?"true":"false")
                         <<",\"failure\":"<<unsigned(failure)<<",\"laps\":"<<final[race->player_index()].body.word(0x7a)
                         <<",\"restart_deterministic\":"<<(restart_check?"true":"false")<<"}\n";
            }
            std::cout << "{\"screenshot_written\":true,\"world_view\":" << (controls.world?"true":"false") << ",\"model_index\":" << controls.model << "}\n";
        } else {
            HFONT hud_font=nullptr;
            bool audio_paused=false;
            std::unique_ptr<srf::Gamepad> gamepad;
            std::string controller_failure;
            if(race) {
                grid_instruments=grid_countdown.has_value();
                hud_background=CreateSolidBrush(RGB(12,20,27));
                race_hud=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|SS_LEFT|(race_hud_visible && !grid_instruments?WS_VISIBLE:0),12,12,std::max(200,int(controls.width)-24),112,window,nullptr,GetModuleHandleW(nullptr),nullptr);
                hud_font=CreateFontW(18,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
                SendMessageW(race_hud,WM_SETFONT,reinterpret_cast<WPARAM>(hud_font),TRUE);
                SetWindowTextW(window,(L"Stunt Race FX — "+course_name).c_str());
                controls.dirty=false;
                try {
                    if(!audio) {
                        audio=std::make_unique<srf::AudioDevice>(srf::extract_course_audio(*race_rom,course));
                        if(grid_countdown) {
                            audio->play_music(srf::extract_trailer_loading_music(*race_rom),true);
                            grid_fanfare=true;
                        }
                    }
                    audio->update({},false,false,true,grid_fanfare);
                }
                catch(const std::exception& error) {audio_failure=error.what();}
                try {gamepad=std::make_unique<srf::Gamepad>(controller_path);}
                catch(const std::exception& error) {controller_failure=error.what();}
            }
            ShowWindow(window,SW_SHOW);
            if(borderless)SendMessageW(window,WM_KEYDOWN,VK_F11,0);
            LARGE_INTEGER frequency,last; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&last);
            bool running=true;
            double accumulator=0;
            double grid_fraction=0;
            while(running) {
                MSG message;
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT) running=false;
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                if(!running) break;
                if(controls.resize) { renderer.resize(controls.width,controls.height); controls.resize=false; }
                if(controls.dirty && !race) {
                    std::wostringstream title;
                    if(controls.world) {
                        renderer.load_world(world,controls.frame);
                        title << L"Stunt Race FX — "<<course_name<<L" world research | Original flat colors / magenta: unresolved material | "
                              << L"Drag: orbit  Right-drag/WASD/QE: move  Wheel: zoom  Tab: wire  F: pose  Home: reset";
                    } else {
                        const auto& model=models[controls.model];
                        controls.frame%=model.frames.size(); renderer.load(model,controls.frame);
                        title << L"Stunt Race FX — native model research | " << controls.model+1 << L"/" << models.size()
                              << L" offset 0x" << std::hex << model.rom_offset << std::dec << L" frame " << controls.frame
                              << L" | Arrows: model  F: frame  W: wire  Drag: rotate  Wheel: zoom  Space: spin | Materials/identity unverified";
                    }
                    controls.dirty=false;
                    SetWindowTextW(window,title.str().c_str());
                }
                LARGE_INTEGER now; QueryPerformanceCounter(&now);
                const float elapsed=std::min(.1f,float(double(now.QuadPart-last.QuadPart)/frequency.QuadPart));
                if(race) {
                    const bool focused=GetForegroundWindow()==window && !IsIconic(window);
                    const auto pad=gamepad?gamepad->poll(focused):srf::GamepadInput{};
                    if(pad.pause)controls.paused=!controls.paused;
                    if(pad.disconnected)controls.paused=true;
                    if(pad.restart)controls.restart=true;
                    if(race->time_over_pending())controls.restart=false;
                    if(controls.restart && championship) {
                        controls.restart=false;
                        if(audio && !audio_paused) {audio->pause(true);audio_paused=true;}
                        ShowWindow(window,SW_HIDE);
                        if(srf::show_game_result({L"RETIRE FROM RACE?",course_name,
                                L"Retiring ends this attempt. Retrying the course uses one spare car.",
                                L"Retire",L"Resume race"})) {
                            retired=true;
                            DestroyWindow(window);
                            continue;
                        }
                        ShowWindow(window,SW_SHOW);
                        SetForegroundWindow(window);
                        QueryPerformanceCounter(&last);
                        continue;
                    }
                    if(controls.restart) {
                        race->reset();previous=race->racers();accumulator=0;countdown_clock=0;input_index=0;controls.restart=false;controls.paused=false;race_audio_state={};
                        scenery->reset();environment->reset();
                        if(rain){rain->reset();rain_changed=true;}
                        phase_video_ticks=environment_video_ticks=0;
                        if(audio)try {audio->restart();audio->update({},false,false,true,false);}catch(const std::exception& error){audio_failure=error.what();audio.reset();}
                    }
                    if(audio) {
                        try {
                            if(audio_paused!=(controls.paused || !focused)) {audio_paused=controls.paused || !focused;audio->pause(audio_paused);}
                            const auto failure=audio->error();
                            if(!failure.empty()) {audio_failure=failure;audio.reset();}
                        } catch(const std::exception& error) {audio_failure=error.what();audio.reset();}
                    }
                    if(focused && !controls.paused) {
                        const auto input=srf::combine_player_input(keyboard_control(window),pad);
                        const auto advance_race=[&](double seconds) {
                            accumulator+=seconds;
                            while(accumulator>=1/simulation_hz) {
                                step_race(input.requested,input.steering_q15);
                                accumulator-=1/simulation_hz;
                            }
                        };
                        double remaining=elapsed;
                        constexpr double vblank=1./60.0988;
                        while(remaining>0 && grid_countdown && !grid_countdown->finished()) {
                            const double slice=std::min(remaining,vblank-grid_fraction);
                            if(grid_countdown->frame().driving)advance_race(slice);
                            remaining-=slice;
                            grid_fraction+=slice;
                            if(grid_fraction>=vblank-1e-12) {
                                grid_fraction=0;
                                grid_countdown->advance();
                                const auto& frame=grid_countdown->frame();
                                if(frame.tick<=grid_countdown->drive_tick()) {
                                    settle_grid(input.requested);
                                    advance_environment_to(frame.tick);
                                    if(frame.driving) {
                                        race->release_start_signal();
                                        phase_video_ticks=frame.tick;
                                    }
                                }
                                if(audio)try {
                                    if(frame.effect>=0)audio->play_race_effect(static_cast<std::uint8_t>(frame.effect));
                                    if(frame.start_music){audio->play_music();grid_fanfare=false;}
                                }catch(const std::exception& error){audio_failure=error.what();audio.reset();}
                            }
                        }
                        advance_race(remaining);
                        if(!race->ended())advance_environment_to(phase_video_ticks+
                            static_cast<std::uint64_t>(std::floor(countdown_clock+accumulator*60.0988)));
                    }
                    if(championship && race->ended()) {
                        DestroyWindow(window);
                        continue;
                    }
                    const float blend=controls.paused?1.f:float(accumulator*simulation_hz);
                    auto poses=vehicle_poses(*race_rom,previous,race->racers(),race->player_index(),blend);
                    const auto& a=previous[race->player_index()].body;
                    const auto& b=race->racers()[race->player_index()].body;
                    chase_camera(renderer,poses[race->player_index()],lerp_angle(a.word(0x24),b.word(0x24),blend),camera_road,course);
                    std::optional<unsigned> grid_seconds;
                    bool held_on_grid=false;
                    if(grid_countdown && !grid_countdown->finished()) {
                        const auto& frame=grid_countdown->frame();
                        const float chase_weight=std::clamp((int(frame.tick)-int(grid_countdown->drive_tick()))/14.f,0.f,1.f);
                        if(chase_weight<1) {
                            const auto view=grid_camera(renderer,grid_origin,grid_heading,frame.scene_phase,chase_weight);
                            for(unsigned i=0;i<poses.size();++i)
                                poses[i]=vehicle_pose(*race_rom,previous[i],race->racers()[i],i==race->player_index(),blend,view);
                        }
                        held_on_grid=!frame.driving;
                        grid_seconds=frame.clock_seconds;
                        grid_art->draw(grid_pixels,frame,*race);
                        renderer.set_bonus_overlay(grid_pixels,frame.brightness,1.f,40.f,grid_art->dashboard(frame,*race),
                                                   0,224,frame.banner_step>=0);
                    } else if(grid_instruments) {
                        grid_instruments=false;
                        renderer.clear_bonus_overlay();
                        ShowWindow(race_hud,race_hud_visible?SW_SHOWNA:SW_HIDE);
                    }
                    renderer.set_environment(environment->presentation());
                    renderer.load_race_world(world,poses,*race_rom,*race,*scenery);
                    auto status=race_status(*race,simulation_hz,championship.has_value(),grid_seconds,held_on_grid);
                    if(championship) {
                        const auto league=srf::class_name(championship->league());
                        status+=L"\r\n  Speed Trax    "+std::wstring(league.begin(),league.end())+L"    Race "+
                            std::to_wstring(championship->round())+L" / 4    Spare cars "+std::to_wstring(championship->credits());
                    }
                    const auto pad_status=gamepad?gamepad->status():"Controller unavailable: "+controller_failure;
                    const auto pad_size=MultiByteToWideChar(CP_UTF8,0,pad_status.data(),static_cast<int>(pad_status.size()),nullptr,0);
                    std::wstring pad_text(static_cast<std::size_t>(pad_size),L' ');
                    MultiByteToWideChar(CP_UTF8,0,pad_status.data(),static_cast<int>(pad_status.size()),pad_text.data(),pad_size);
                    status+=L"\r\n  "+pad_text;
                    if(!recorded.empty()) status+=L"\r\n  RECORDED BUTTON INPUT - native simulation (R restarts input sequence)";
                    if(!audio_failure.empty()) {
                        const auto size=MultiByteToWideChar(CP_UTF8,0,audio_failure.data(),static_cast<int>(audio_failure.size()),nullptr,0);
                        std::wstring reason(static_cast<std::size_t>(size),L' ');
                        MultiByteToWideChar(CP_UTF8,0,audio_failure.data(),static_cast<int>(audio_failure.size()),reason.data(),size);
                        status+=L"\r\n  Muted: "+reason;
                    }
                    SetWindowTextW(race_hud,status.c_str());
                }
                if(controls.spin) controls.yaw+=elapsed*.5f;
                if(controls.world && !race && GetForegroundWindow()==window) {
                    const float step=elapsed*.8f/controls.zoom;
                    const float forward=float(bool(GetAsyncKeyState('W')&0x8000))-float(bool(GetAsyncKeyState('S')&0x8000));
                    const float sideways=float(bool(GetAsyncKeyState('D')&0x8000))-float(bool(GetAsyncKeyState('A')&0x8000));
                    controls.target_x+=step*(sideways*std::cos(controls.yaw)-forward*std::sin(controls.yaw));
                    controls.target_z+=step*(sideways*std::sin(controls.yaw)+forward*std::cos(controls.yaw));
                    controls.target_y+=step*(float(bool(GetAsyncKeyState('E')&0x8000))-float(bool(GetAsyncKeyState('Q')&0x8000)));
                }
                last=now;
                if(IsIconic(window)) { Sleep(50); continue; }
                if(rain_changed){renderer.set_rain(rain->pixels());rain_changed=false;}
                renderer.render();
            }
            if(hud_font) DeleteObject(hud_font);
            if(hud_background) {DeleteObject(hud_background);hud_background=nullptr;}
        }
        audio.reset();
        CoUninitialize();
        race_hud=nullptr;
        if(!launch_game)return 0;
        if(!championship) continue;
        if(!race->ended() && !retired) { championship.reset(); continue; }
        const auto result=srf::championship_result(*race,retired);
        const auto outcome=championship->finish(result,progress);
        if(outcome==srf::ChampionshipOutcome::class_complete) {
            try {progress.save(progress_path);}
            catch(const std::exception& error) {
                MessageBoxA(nullptr,error.what(),"Speed Trax could not save your class record",MB_OK|MB_ICONWARNING);
            }
        }
        const bool proceed=srf::show_round_result(*championship,result,lap_times);
        if(outcome==srf::ChampionshipOutcome::class_complete) {
            if(proceed) srf::show_class_result(*championship,progress);
            if(selection.league!=srf::SpeedClass::master)
                selection.league=static_cast<srf::SpeedClass>(static_cast<unsigned>(selection.league)+1);
            championship.reset();
        } else if(!proceed || outcome==srf::ChampionshipOutcome::game_over) {
            if(proceed && !championship->results().empty()) srf::show_class_result(*championship,progress);
            championship.reset();
        } else {
            std::optional<srf::CourseMapEntry> entry;
            if(outcome==srf::ChampionshipOutcome::bonus) {
                srf::BonusReward reward{};
                entry=srf::CourseMapEntry::skip;
                if(srf::show_game_result({L"BONUS TRAX",L"Before race 3",
                    L"Drive the Trailer. Collect flags for extra time and complete laps for spare cars.",L"Play",L"Skip"})) {
                    auto finish=srf::play_speed_bonus(*race_rom,*championship,controller_path,
                        launch_controls.width,launch_controls.height,borderless);
                    if(!finish){championship.reset();continue;}
                    reward=finish->reward;
                    entry=finish->entry;
                    next_grid_audio=std::move(finish->audio);
                }
                championship->finish_bonus(reward.extra_seconds,reward.extra_cars);
            } else {
                if(outcome==srf::ChampionshipOutcome::next_race && championship->round()==3)
                    entry=srf::CourseMapEntry::race_result;
                championship->advance();
            }
            if(entry && !show_course_map(*race_rom,*championship,progress,*entry,controller_path,
                                launch_controls.width,launch_controls.height,borderless,next_grid_audio)) {
                next_grid_audio.reset();
                championship.reset();
            }
        }
        }
    } catch(const std::exception& error) {
        std::cerr << "{\"error\":" << srf::json_string(error.what()) << "}\n";
        if((launch_game || vr) && screenshot.empty())MessageBoxA(nullptr,error.what(),"Stunt Race FX could not start",MB_OK|MB_ICONERROR);
        return 1;
    }
}
