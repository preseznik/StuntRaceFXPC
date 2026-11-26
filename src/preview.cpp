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
#include "demo_dashboard.hpp"
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
#include <bit>
#include <climits>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <deque>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tuple>
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
enum class SurfaceFinish : std::uint32_t { none, paint, rubber, glass, steel_grating, foliage, barrier_paint, cloud_bank };
struct GpuVertex {
    float x,y,z,r,g,b,u=0,v=0,texture=0,painter_depth=0;
    std::array<float,4> road_origin_bias{};
    SurfaceFinish finish=SurfaceFinish::none;
};
#include "ray_shadows.inc"
struct RenderSprite {
    unsigned table=0,phase=0;
    std::array<float,3> position{};
    float diameter=0;
    bool wheel=false;
    float steer=0,spin=0;
};
struct RenderVehicle {
    unsigned descriptor=0;
    std::array<float,3> position{};
    std::array<float,9> orientation{1,0,0,0,1,0,0,0,1};
    std::vector<RenderSprite> sprites;
    bool player=false;
    float route_follow=0;
    bool source_model=false; // The articulated Bonus Trax cab and cargo have their own shells and wheels.
    float heading=0;
    bool remote_rally_truck=false;
};
#include "n64_4wd.inc"
#include "wet_tire_spray.inc"
#include "n64_track.inc"
#include "n64_scenery.inc"
#include "forest_roadside.inc"
#include "n64_terrain.inc"
#include "n64_tiles.inc"
struct SceneTarget {
    UINT width{},height{};
    ComPtr<ID3D11Texture2D> color;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11ShaderResourceView> depth_srv;
};
struct SceneEye {
    // Relative to the game camera: +X right, +Y up, +Z forward. Offsets use
    // normalized scene units. Frustum edges are signed tangents at unit depth.
    std::array<float,3> offset{};
    std::array<float,4> orientation{0,0,0,1}; // eye-to-camera quaternion, xyzw
    float left=-1,right=1,down=-1,up=1;
    bool second=false;
};
struct Constants {
    float cy,sy,cp,sp,aspect,zoom,distance,road_pass;
    float tx,ty,tz,eye_enabled;
    std::array<float,4> eye_offset{},eye_right{},eye_up{},eye_forward{},projection{};
    std::array<float,4> shadow_origin{},shadow_axis{};
    std::array<float,4> sun{};
    float isometric=0,cutaway=0,padding[2]{};
    std::array<std::array<float,4>,4> car_centers{};
    std::array<float,4> probe_step{}; // metres per cell for course-wide indirect lighting
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
    unsigned indexed_textures{},add_fixed{};
    float aqua_meters{},aqua_plane_y{};
    unsigned sunset_valley{};
    float sunset_dusk{};
    float night_lighting=-1; // 0..1 from sunset to night, negative outside the night courses
    float panorama_fog=-1; // negative for courses without a daylight panorama
    std::array<float,3> panorama_fog_color{1,1,1};
    unsigned harbor_windows{};
};
struct Controls {
    std::size_t model=2, frame=0;
    float yaw=.65f, pitch=-.45f, zoom=2.1f;
    bool wire=false, spin=false, dirty=true, resize=false, dragging=false, panning=false, world=false;
    float target_x=0,target_y=0,target_z=0,distance=3.f;
    int mouse_x=0, mouse_y=0;
    unsigned width=1280, height=720;
    std::size_t count=0;
    bool back=false,race=false,restart=false,paused=false,recenter=false;
};
Controls controls;
enum class RaceCamera { chase,far_chase,cockpit,bonnet,isometric,count };
struct RemasteredCamera {
    RaceCamera view=RaceCamera::chase;
    bool enabled=false,look=false;
    float heading=0,tilt=0,magnification=1;
    unsigned max_distance_m=80;

    bool operator==(const RemasteredCamera&) const = default;
    bool mounted() const {return view==RaceCamera::cockpit || view==RaceCamera::bonnet;}
    float starting_distance() const {
        if(view==RaceCamera::isometric)return std::min(5400.f,90.f*max_distance_m);
        return view==RaceCamera::far_chase?1450.f:900.f;
    }
    void scroll(float steps) {
        const float minimum=mounted()?.65f:starting_distance()/(90.f*max_distance_m);
        magnification=std::clamp(magnification*std::pow(1.12f,steps),minimum,1.8f);
    }
    void center() {heading=tilt=0;magnification=1;}
    void rotate(float right,float down) {
        if(view==RaceCamera::isometric)return;
        heading=mounted()?std::clamp(heading-right,-1.35f,1.35f):std::remainder(heading-right,6.2831853f);
        tilt=std::clamp(tilt-down,mounted()?-.52f:-1.f,mounted()?.55f:.14f);
    }
} race_camera;
unsigned desktop_fps=0;
srf::vr::Drive* game_xr=nullptr;
bool game_closing=false;
#ifdef SRF_NAVIGATION_TEST
// Only the navigation test executable can advance on the private, non-input desktop.
bool private_test_desktop() {
    static const bool isolated=[] {
        wchar_t name[128]{};
        DWORD size{};
        return GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,name,sizeof(name),&size)
            && std::wstring_view(name).starts_with(L"SRF-Navigation-");
    }();
    return isolated;
}
bool capture_test_frame=false;
#endif
bool game_focused(HWND window) {
#ifdef SRF_NAVIGATION_TEST
    if(!game_xr && private_test_desktop())return !IsIconic(window);
#endif
    return game_xr?game_xr->focused():GetForegroundWindow()==window && !IsIconic(window);
}
HWND race_hud=nullptr;
bool race_hud_visible=true;
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM key, LPARAM data) {
    switch(message) {
    case WM_DESTROY: PostQuitMessage(0); return 0;
    case WM_SIZE:
        if (LOWORD(data) && HIWORD(data)) { controls.width=LOWORD(data); controls.height=HIWORD(data); controls.resize=true;
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
            if(key=='H' && !(data&(1L<<30)))race_hud_visible=!race_hud_visible;
            if(key==VK_F2 && !(data&(1L<<30))) controls.recenter=true;
            if(key=='C' && !game_xr && race_camera.enabled && !(data&(1L<<30))) {
                race_camera.view=static_cast<RaceCamera>((static_cast<int>(race_camera.view)+1)%static_cast<int>(RaceCamera::count));
                race_camera.center();
            }
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
        if(controls.race && !race_camera.look) return 0;
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
            if(race_camera.look) {
                race_camera.rotate((GET_X_LPARAM(data)-controls.mouse_x)*.005f,(GET_Y_LPARAM(data)-controls.mouse_y)*.005f);
                controls.mouse_x=GET_X_LPARAM(data); controls.mouse_y=GET_Y_LPARAM(data);
                return 0;
            }
            const float dx=(GET_X_LPARAM(data)-controls.mouse_x)*.006f/controls.zoom;
            const float dy=(GET_Y_LPARAM(data)-controls.mouse_y)*.006f/controls.zoom;
            controls.target_x-=dx*std::cos(controls.yaw)+dy*std::sin(controls.yaw);
            controls.target_z-=dx*std::sin(controls.yaw)-dy*std::cos(controls.yaw);
            controls.mouse_x=GET_X_LPARAM(data); controls.mouse_y=GET_Y_LPARAM(data);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if(race_camera.look)race_camera.scroll(GET_WHEEL_DELTA_WPARAM(key)/120.f);
        if(controls.race) return 0;
        controls.zoom=std::clamp(controls.zoom*std::pow(1.15f,GET_WHEEL_DELTA_WPARAM(key)/120.f),.3f,controls.world?80.f:8.f); return 0;
    }
    return DefWindowProcW(window,message,key,data);
}

enum class RaceLook { classic,n64,remastered };

struct RaceWeather {
    float clouds=0, extinction=0, rain=0, seconds=0; // matches Atmosphere in the material shader
};

struct RaceGraphics {
    RaceLook look=RaceLook::classic;
    bool low_raster=false;
    unsigned hour=12,msaa=0;
    RaceWeather weather{};
    bool ray_shadows=false,ray_reflections=false,indirect_light=false,material_roughness=false,clear_coat=false,high_detail_road=false,high_detail_scenery=false;
    unsigned shadow_rays=4;
    bool wet_tire_spray=false,volumetric_lighting=false;
};

class Renderer {
    HWND output_window{};
    DesktopFrameLimit frame_limit;
    RaceCamera rendered_camera() const {return grid_sweep?RaceCamera::chase:race_camera.view;}
    const bool enhanced_scene,n64_mode;
    bool stunt_style,blue_lake_style,aqua_style,sunset_style,night_style,cruise_style,forest_style,sea_style,white_land_style,lake_style,ravine_style,sky_ramp_style,harbor_style;
    const unsigned start_hour;
    static std::array<float,4> sun_at(float hour) {
        const float angle=(hour-6.f)*3.14159265f/12.f;
        return {std::cos(angle),.8f*std::sin(angle),.6f*std::sin(angle),0};
    }
    const bool low_raster,trace_sun,trace_reflections,trace_indirect,roughness,clear_coat,high_detail_road,high_detail_scenery;
    bool course_road_detail=false,course_scenery_detail=false,owl_scenery=false;
    std::map<unsigned,std::vector<float>> road_offsets; // Per placed road mesh; shared across race frames.
    ray_shadows::Tree reflection_actors;
    ray_shadows::ActorBuffer<ray_shadows::Node> actor_nodes;
    ray_shadows::ActorBuffer<ray_shadows::Triangle> actor_faces;
    bool trace_course() const {return trace_sun || trace_indirect || (trace_reflections && (weather.rain>0 || ravine_style));}
    ComPtr<ID3D11ShaderResourceView> ray_nodes,ray_faces;
    std::map<unsigned,std::vector<float>> forest_banks;
    std::vector<GpuVertex> ray_landscape;
    std::vector<std::uint32_t> classic_frame;
    std::array<float,4> sun{};
    RaceWeather weather;
    ComPtr<ID3D11Buffer> atmosphere;
    ComPtr<ID3D11PixelShader> weather_rain_ps;
    ComPtr<ID3D11ComputeShader> probe_shader;
    ComPtr<ID3D11UnorderedAccessView> probe_output;
    ComPtr<ID3D11ShaderResourceView> probe_view;
    ComPtr<ID3D11SamplerState> probe_sampler;
    static constexpr UINT probe_width=48,probe_height=12;
    float probe_cloud_cover=-1;
    bool probe_dirty=true;
    std::array<float,4> course_probe_step{};
    SceneTarget sampled_scene;
    UINT coverage_samples=1;
    ComPtr<ID3D11Texture2D> resolved_scene;
    ComPtr<ID3D11ShaderResourceView> scene_view,material_tiles,foliage_mips;
    ComPtr<ID3D11SamplerState> asphalt_sampler;
    ComPtr<ID3D11PixelShader> display_ps,eye_display_ps;
    struct EyeSurface {
        SceneTarget scene;
        ComPtr<ID3D11Texture2D> resolved;
        ComPtr<ID3D11ShaderResourceView> view;
    };
    std::map<std::pair<UINT,UINT>,EyeSurface> eye_surfaces;
    static constexpr UINT car_shadow_size=512;
    static constexpr UINT course_shadow_size=4096;
    ComPtr<ID3D11VertexShader> car_shadow_vs;
    ComPtr<ID3D11VertexShader> course_shadow_vs;
    ComPtr<ID3D11PixelShader> course_shadow_ps;
    ComPtr<ID3D11DepthStencilView> car_shadow_depth;
    ComPtr<ID3D11DepthStencilView> course_shadow_depth;
    ComPtr<ID3D11ShaderResourceView> car_shadow_view;
    ComPtr<ID3D11ShaderResourceView> course_shadow_view;
    UINT course_shadow_vertices=0,cloud_vertices=0;
    bool course_shadow_dirty=true;
    std::vector<std::pair<unsigned,std::size_t>> course_shadow_poses;
    ComPtr<ID3D11SamplerState> car_shadow_sampler;
    UINT car_shadow_vertices=0;
    UINT rival_start=0;
    std::array<std::array<float,4>,4> car_centers{};
    std::array<float,4> shadow_origin{},shadow_axis{};
    std::map<ID3D11Texture2D*,SceneTarget> eye_targets;
    std::chrono::steady_clock::time_point next_mirror{};
    void present(std::span<const uint32_t> panel={},bool show_mirror=true) {
#ifdef SRF_NAVIGATION_TEST
        if(capture_test_frame && swapchain){
            capture_test_frame=false;
            screenshot(L"navigation-frame.tmp");
            std::filesystem::rename(L"navigation-frame.tmp",L"navigation-frame.png");
        }
#endif
        if(game_xr && swapchain){
            const auto update=[&](int64_t,bool){srf::vr::Display hud{headset_hud,controls.recenter};controls.recenter=false;return hud;};
            const auto draw=[&](ID3D11Texture2D* texture,DXGI_FORMAT format,const srf::vr::Eye& view){
                auto [it,added]=eye_targets.try_emplace(texture);
                if(added){D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d);it->second=make_target(d.Width,d.Height,texture,format);}
                auto offset=view.meters;
                for(auto& axis:offset)axis=normalized_distance(axis*90);
                render_eye(it->second,{offset,view.orientation,view.left,view.right,view.down,view.up,view.second});
            };
            if(!game_xr->step(update,draw,panel))game_closing=true;
            if(show_mirror) {
                const auto hr=swapchain->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
                if(hr!=DXGI_ERROR_WAS_STILL_DRAWING)check(hr,"VR companion present");
            }
            return;
        }
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
    ComPtr<ID3D11DepthStencilState> spray_depth;
    ComPtr<ID3D11PixelShader> spray_shader;
    ComPtr<ID3D11Buffer> spray_vertices;
    UINT spray_count=0;
    ComPtr<ID3D11BlendState> depth_only;
    ComPtr<ID3D11ShaderResourceView> texture_view, dusk_texture_view, palette_indices_view;
    srf::generated::CoursePackage course_colors = srf::generated::course_packages.front();
    srf::WorldPalette world_palette = course_colors.day_palette;
    ComPtr<ID3D11ShaderResourceView> backdrop_indices,backdrop_colors;
    ComPtr<ID3D11Texture2D> backdrop_color_texture;
    ComPtr<ID3D11VertexShader> backdrop_vs;
    ComPtr<ID3D11PixelShader> backdrop_ps;
    ComPtr<ID3D11PixelShader> rain_ps;
    ComPtr<ID3D11Texture2D> rain_mask;
    ComPtr<ID3D11ShaderResourceView> rain_view;
    bool show_rain=false,ravine_wet=false;
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
    unsigned foliage_layer=0;
    std::map<std::pair<unsigned,unsigned>,TextureLocation> texture_locations;
    std::array<bool,256> textured_materials{};
    std::map<std::pair<unsigned,std::size_t>,std::vector<srf::SurfaceOffset>> surface_offsets;
    std::map<std::pair<unsigned,std::size_t>,std::vector<bool>> barrier_backs;
    UINT vertex_count=0,surface_vertex_count=0,world_vertex_count=0,line_count=0,world_line_count=0;
    UINT two_sided_world_start=0;
    bool vehicle_depth_pass=false;
    std::array<double,3> world_center{};
    double world_radius=1;
    ComPtr<ID3D11Buffer> landscape_buffer;
    UINT landscape_vertex_count=0;
#include "volumetric_light.inc"
    void upload_vertices(std::span<const GpuVertex> data,ComPtr<ID3D11Buffer>& buffer) {
        if(data.empty()) throw std::runtime_error("Model has no renderable primitives");
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=static_cast<UINT>(data.size()*sizeof(GpuVertex));
        desc.Usage=D3D11_USAGE_IMMUTABLE; desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA initial{data.data(),0,0};
        check(device->CreateBuffer(&desc,&initial,buffer.ReleaseAndGetAddressOf()),"Geometry buffer");
    }
    void upload(const std::vector<GpuVertex>& triangles,const std::vector<GpuVertex>& lines={},UINT surface_count=UINT_MAX) {
        spray_count=0;
        std::vector<GpuVertex> data=triangles; data.insert(data.end(),lines.begin(),lines.end());
        upload_vertices(data,vertices);
        vertex_count=static_cast<UINT>(triangles.size()); line_count=static_cast<UINT>(lines.size());
        surface_vertex_count=std::min(surface_count,vertex_count);
    }
public:
    bool grid_sweep=false;
    void use_classic_frame(const srf::Rom& rom) {
        if(n64_mode || !low_raster)return;
        srf::DemoDashboard surround(rom);
        const auto frame=surround.draw(100,15,0);
        classic_frame.assign(frame.begin(),frame.end());
    }
    void create_probes() {
        D3D11_TEXTURE3D_DESC desc{};
        desc.Width=probe_width;desc.Height=probe_height;desc.Depth=4*probe_width;desc.MipLevels=1;
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
        ComPtr<ID3D11Texture3D> volume;
        check(device->CreateTexture3D(&desc,nullptr,&volume),"Irradiance volume");
        check(device->CreateUnorderedAccessView(volume.Get(),nullptr,probe_output.ReleaseAndGetAddressOf()),"Irradiance write view");
        check(device->CreateShaderResourceView(volume.Get(),nullptr,probe_view.ReleaseAndGetAddressOf()),"Irradiance sample view");
    }
    void select_course(std::optional<srf::CourseId> course,std::optional<srf::StuntCourse> stunt={}) {
        road_offsets.clear();
        course_road_detail=high_detail_road && course.has_value();
        owl_scenery=high_detail_scenery && course==srf::CourseId::night_owl;
        course_scenery_detail=high_detail_scenery && (course==srf::CourseId::kings_forest || course==srf::CourseId::easy_ride);
        stunt_style=enhanced_scene && !course;
        blue_lake_style=stunt_style && stunt==srf::StuntCourse::blue_lake;
        aqua_style=enhanced_scene && course==srf::CourseId::aqua_tunnel;
        sunset_style=enhanced_scene && course==srf::CourseId::sunset_valley;
        night_style=enhanced_scene && (course==srf::CourseId::night_owl || course==srf::CourseId::night_cruise || course==srf::CourseId::harbor_city);
        cruise_style=enhanced_scene && course==srf::CourseId::night_cruise;
        forest_style=enhanced_scene && course==srf::CourseId::kings_forest;
        sea_style=enhanced_scene && course==srf::CourseId::sea_breeze;
        white_land_style=enhanced_scene && course==srf::CourseId::white_land;
        lake_style=enhanced_scene && course==srf::CourseId::lake_side;
        ravine_style=enhanced_scene && course==srf::CourseId::big_ravine;
        sky_ramp_style=enhanced_scene && course==srf::CourseId::sky_ramp;
        harbor_style=enhanced_scene && course==srf::CourseId::harbor_city;
        if(enhanced_scene && !n64_mode)sun=sun_at(cruise_style?17.5f:night_style?0.f:float(start_hour));
        classic_frame.clear();
        show_rain=ravine_wet=grid_sweep=false;
        clear_bonus_overlay();
    }
    std::vector<uint32_t> headset_hud;
    void set_headset_hud(std::span<const uint32_t> upper,std::span<const uint32_t> dashboard) {
        if(!headset_output)return;
        if(dashboard.empty() && std::none_of(upper.begin(),upper.end(),[](auto pixel){return pixel>>24;})){
            headset_hud.clear();return;
        }
        headset_hud.assign(256*224,0);
        for(int y=0;y<128;++y)std::copy_n(upper.begin()+208*y,208,headset_hud.begin()+256*(32+y)+24);
        std::copy(dashboard.begin(),dashboard.end(),headset_hud.begin()+256*168);
    }
    Renderer(HWND window,ID3D11Device* headset_device=nullptr,unsigned fps=0,RaceGraphics graphics={},std::optional<srf::CourseId> course=srf::CourseId::easy_ride):
        output_window(window),frame_limit(fps),enhanced_scene(graphics.look!=RaceLook::classic),n64_mode(graphics.look==RaceLook::n64),
        start_hour(graphics.hour),low_raster(graphics.low_raster),trace_sun(graphics.ray_shadows),trace_reflections(graphics.ray_reflections),trace_indirect(graphics.indirect_light),
        roughness(graphics.material_roughness),clear_coat(graphics.clear_coat),high_detail_road(graphics.high_detail_road),high_detail_scenery(graphics.high_detail_scenery),weather(graphics.weather) {
        select_course(course);
        headset_output=headset_device!=nullptr;
        D3D_FEATURE_LEVEL level;
        const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_11_0};
        HRESULT hr=E_FAIL;
        if(headset_device){
            device=headset_device;device->GetImmediateContext(&context);hr=S_OK;
            if(window){
                ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIFactory> factory;
                check(device.As(&dxgi),"Headset DXGI device");check(dxgi->GetAdapter(&adapter),"Headset adapter");
                check(adapter->GetParent(IID_PPV_ARGS(&factory)),"Headset DXGI factory");
                DXGI_SWAP_CHAIN_DESC desc{};desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
                desc.OutputWindow=window;desc.Windowed=TRUE;
                check(factory->CreateSwapChain(device.Get(),&desc,&swapchain),"Headset companion swapchain");
            }
        }
        else for (auto driver : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
            if(driver==D3D_DRIVER_TYPE_HARDWARE && std::getenv("SRF_TEST_WARP"))continue;
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
        constexpr char shader[]=
#include "n64_materials.hlsl.inc"
R"(
cbuffer View : register(b0) { float cy,sy,cp,sp,aspect,zoom,distance,road_pass; float3 target; float eye_enabled;
    float4 eye_offset,eye_right,eye_up,eye_forward,projection,shadow_origin,shadow_axis,sun;
    float isometric,cutaway; float2 view_padding; float4 car_centers[4],probe_step; };
cbuffer Environment : register(b1) { float4 crop; uint layer,blank,fixed_color,math_layers; uint gradient_y,sky_color,texture_weight; float rows_per_pixel; uint4 row_subtract[32]; uint indexed_textures,add_fixed; float aqua_meters,aqua_plane_y; uint sunset_valley; float sunset_dusk; float night_lighting; float panorama_fog; float4 panorama_fog_color; };
Texture2DArray<float4> tiles:register(t0);
#if ROADSIDE_SCENERY
Texture2D<float4> filtered_leaves:register(t15);
#endif
Texture2DArray<float4> dusk_tiles:register(t3);
Texture2DArray<uint> palette_indices:register(t4);
Texture2D<uint> colors:register(t2);
SamplerState tile_sampler:register(s0);
struct Vertex { float3 position:POSITION; float3 color:COLOR; float3 uv:TEXCOORD; float painter_depth:TEXCOORD1; float4 road:TEXCOORD2; uint finish:TEXCOORD6; };
#ifdef REMASTERED_LIGHT
Texture2D<float> car_depth:register(t8);
Texture2D<float> course_depth:register(t9);
SamplerComparisonState shadow_sampler:register(s1);
// The fixed sun and normalized course share a single world-anchored map.
// Its projection is independent of the race camera and its cutaway.
static const float course_half_width=1.6;
float3 course_shadow_position(float3 position) {
    float3 right=normalize(float3(sun.z,0,-sun.x));
    return float3(dot(position,right)/course_half_width,
                  dot(position,cross(sun.xyz,right))/course_half_width,
                  .5+.5*dot(position,sun.xyz)/course_half_width);
}
struct CourseCaster {float4 position:SV_POSITION; float3 uv:TEXCOORD;};
CourseCaster course_shadow_vertex(Vertex v) {
    CourseCaster o;
    o.position=float4(course_shadow_position(v.position),1);
    o.uv=v.uv;
    return o;
}
void course_shadow_pixel(CourseCaster p) {
    if(p.uv.z>0) clip(tiles.SampleLevel(tile_sampler,p.uv,0).a-.5);
}
)"
#include "ray_shadows.hlsl.inc"
#include "indirect_light.hlsl.inc"
R"(
float course_sunlight(float3 position,float3 normal) {
#if RAY_SUN_SHADOWS
    return traced_sunlight(position,normal);
#else
    if(sun.y<=.01)return 1;
    float3 p=course_shadow_position(position);
    if(any(abs(p.xy)>=1) || p.z<=0 || p.z>=1)return 1;
    uint w,h;
    course_depth.GetDimensions(w,h);
    // Bias the receiver by the depth it can span across a comparison texel.
    float cosine=abs(dot(normal,sun.xyz));
    float slope=sqrt(saturate(1-cosine*cosine))/max(.08,cosine);
    float bias=(.15+1.5*slope)/w;
    return course_depth.SampleCmpLevelZero(shadow_sampler,p.xy*float2(.5,-.5)+.5,p.z+bias);
#endif
}
float4 car_shadow_position(float3 position) {
    float3 offset=(position-shadow_origin.xyz)*shadow_origin.w; // metres from chassis
    // Below the horizon use an overhead contact shadow.
    float3 direction=sun.y>.01 ? sun.xyz : float3(0,1,0);
    float3 right=sun.y>.01 ? normalize(float3(direction.z,0,-direction.x)) : float3(1,0,0);
    float3 up=cross(direction,right);
    return float4(dot(offset,right)/5,dot(offset,up)/5,.5+dot(offset,direction)/12,1);
}
float4 car_shadow_vertex(float3 position:POSITION):SV_POSITION {
    return car_shadow_position(position);
}
float car_shadow_light(float3 projected) {
    if(shadow_origin.w==0 || any(abs(projected.xy)>1) || projected.z>=1) return 1;
    float2 uv=projected.xy*float2(.5,-.5)+.5;
    float metersBehind=(.5-projected.z)*12;
    float daylight=smoothstep(.01,.16,sun.y);
    float opacity=lerp(.34*(1-smoothstep(1,3,metersBehind)),.66,daylight);
    // A small world-space filter preserves the wheel gaps at contact and softens
    // the outline as the receiver gets farther from the vehicle.
    float radius=clamp(.018+.012*metersBehind,.012,.075)/10;
    float visible=0;
    [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x)
        visible+=car_depth.SampleCmpLevelZero(shadow_sampler,uv+float2(x,y)*radius,
                                               max(.0001,projected.z+.001));
    return 1-opacity*(1-.71*cloud_cover)*(1-visible/9);
}
#endif
struct Pixel { float4 position:SV_POSITION; float3 color:COLOR; float3 uv:TEXCOORD;
#ifdef ENHANCED_SCENE
    float3 shadow:TEXCOORD3;
#endif
#ifdef REMASTERED_LIGHT
    float3 world_pos:TEXCOORD4;
    float depth_m:TEXCOORD5;
#if MATERIAL_ROUGHNESS || CAR_CLEAR_COAT || ROAD_DETAIL || ROADSIDE_SCENERY
    nointerpolation uint finish:TEXCOORD6;
#endif
#endif
#ifdef N64_MODE
    float3 view_pos:TEXCOORD4;
    float fog:TEXCOORD5;
#endif
};
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
#ifdef REMASTERED_LIGHT
    if(isometric) o.position=float4(p.x*zoom/(aspect*distance),p.y*zoom/distance,1-p.z/4,1);
    o.world_pos=v.position;
#if MATERIAL_ROUGHNESS || CAR_CLEAR_COAT || ROAD_DETAIL || ROADSIDE_SCENERY
    o.finish=v.finish;
#endif
    o.depth_m=p.z*sun.w;
    o.shadow=car_shadow_position(v.position).xyz;
#endif
#ifdef N64_MODE
    float3 offset=(v.position-shadow_origin.xyz)*shadow_origin.w;
    o.shadow=float3(dot(offset.xz,shadow_axis.xy),dot(offset.xz,shadow_axis.zw),offset.y);
    o.view_pos=p;
    o.fog=.68*smoothstep(85,340,p.z*shadow_origin.w);
#endif
    o.color=v.color; o.uv=v.uv; return o;
}
float3 course_color_math(float3 rgb,float row_y) {
#ifdef REMASTERED_LIGHT
    if(night_lighting>=0 && rows_per_pixel>0)return rgb; // World-space headlights replace Night Owl's screen-row mask.
#endif
    if((math_layers&1) || rows_per_pixel>0) {
        uint color_word=fixed_color;
        if(rows_per_pixel>0) {
            uint row=min(127u,(uint)(row_y*rows_per_pixel));
            color_word=row_subtract[row/4][row%4];
        }
        float3 fixed_rgb=float3(color_word&31,(color_word>>5)&31,(color_word>>10)&31);
#ifdef REMASTERED_LIGHT
        if(aqua_meters || sunset_valley || night_lighting>=0 || add_fixed)return saturate(rgb+(add_fixed?fixed_rgb:-fixed_rgb)/31);
#endif
        rgb=clamp(round(rgb*31)+(add_fixed?fixed_rgb:-fixed_rgb),0,31)/31;
    }
    return rgb;
}
#ifdef ENHANCED_SCENE
float3 aqua_color(float3 rgb,float meters,float row_y) {
    if(layer && !blank) {
        float transmission=exp2(-.012*max(0,meters-4));
        rgb=lerp(float3(.035,.27,.52),rgb*float3(.77,.94,1.03),transmission);
    }
    return course_color_math(rgb,row_y);
}
#endif
#ifdef REMASTERED_LIGHT
float3 scene_eye_position() {
    float3 back=float3(sy*cp,-sp,-cy*cp);
    float3 right=float3(cy,0,sy),up=float3(sy*sp,cp,-cy*sp);
    return target+distance*back+eye_enabled*(right*eye_offset.x+up*eye_offset.y-back*eye_offset.z);
}
float3 finish_view(float3 position) {
    return isometric?float3(sy*cp,-sp,-cy*cp):normalize(scene_eye_position()-position);
}
#endif

#if MATERIAL_ROUGHNESS || CAR_CLEAR_COAT
// A rough surface spreads the sun reflection and blurs the sky/ground boundary.
// Direct light already contains the sun colour, cloud cover and geometry shadow.
float3 finish_radiance(float3 n,float3 v,float rough,float3 sky,float3 direct) {
    float3 r=reflect(-v,n);
    float horizon=smoothstep(-rough,rough,r.y);
    float exponent=max(1,2/(rough*rough)-2);
    float glint=pow(saturate(dot(r,sun.xyz)),exponent)*saturate(4*dot(n,sun.xyz));
    // The pale horizon is brighter than the zenith. Rough surfaces average
    // both, while polished panels pick up the sky gradient as the car turns.
    float3 tint=lerp(float3(1.44,1.47,1.40),float3(.60,.92,1.35),
                     smoothstep(-.05,.50,r.y)*(1-.8*cloud_cover));
    float3 sky_reflection=lerp(tint,1,rough*rough)*sky;
    return lerp(.18*sky,sky_reflection,horizon)+direct*glint*(1.25/(rough+.10));
}

float3 car_finish(float3 base,float3 n,float3 v,uint kind,float3 sky,float3 direct) {
    float edge=pow(1-saturate(dot(n,v)),5);
#if MATERIAL_ROUGHNESS
    bool tire=kind==2;
    float rough=tire?.78:.34;
    float weight=tire?.065+.035*edge:.10+.12*edge;
    base=lerp(base,finish_radiance(n,v,rough,sky,direct),weight);
#endif
#if CAR_CLEAR_COAT
    if(kind==1) {
        // An untinted, smoother dielectric layer over the coloured base coat.
        float fresnel=.055+.75*edge;
        base=lerp(base,finish_radiance(n,v,.13,sky,direct),fresnel);
    }
#endif
    return base;
}
#endif
#ifdef REMASTERED_LIGHT
float4 tire_spray_pixel(Pixel p):SV_TARGET {
    float2 uv=p.uv.xy;
    float noise=grain(uv*2.7+p.color.g+float2(0,p.color.b))-.5;
    float edge=1-smoothstep(.12,1,length(uv)+.35*noise);
    float alpha=p.color.r*edge*(.78+.35*noise)*smoothstep(.35,1.25,p.depth_m);
    float3 mist=float3(.68,.75,.80)*(.44+.56*saturate(sun.y*2));
    return float4(mist*alpha,alpha);
}
#endif
float4 pixel_main(Pixel p):SV_TARGET {
#ifdef N64_MODE
    // A soft chassis shadow on the road also shades painted lines and chequers.
    // Vertical separation fades it out when the 4WD goes over a crest.
    float contact=(1-smoothstep(.55,1.12,length(p.shadow.xy/float2(2.15,2.40))))*
                  (1-smoothstep(.6,2.25,abs(p.shadow.z)));
    float light=shadow_origin.w>0 ? 1-.60*contact : 1;
#endif
#ifdef REMASTERED_LIGHT
    if(cutaway) {
        // Use one screen-space coverage pattern across overlapping roof layers.
        // Surviving fragments write ordinary depth, including the soft outer rim.
        float coverage=1;
        [unroll] for(int i=0;i<4;++i) {
            float3 d=(p.world_pos-car_centers[i].xyz)*sun.w;
            float forward=-sy*d.x+cy*d.z;
            if(car_centers[i].w && d.y>1.2 && sp*d.y+cp*forward < -1.5) {
                float2 plane=float2(cy*d.x+sy*d.z,cp*d.y-sp*forward);
                coverage=min(coverage,lerp(.20,1,smoothstep(2.6,4.6,length(plane))));
            }
        }
        uint2 xy=(uint2)p.position.xy;
        uint2 bits=xy&1, high=(xy>>1)&1;
        uint threshold=4*(2*(bits.x^bits.y)+bits.y)+2*(high.x^high.y)+high.y;
        clip(coverage-(threshold+.5)/16);
    }
    float light=car_shadow_light(p.shadow);
    // Screen-space derivatives give the visible face normal in world coordinates,
    // including moving opponents and the generated road, banks and chassis.
    float3 area=cross(ddx(p.world_pos),ddy(p.world_pos));
    float len2=dot(area,area);
    float3 normal=len2>1e-24 ? area*rsqrt(max(len2,1e-24)) : float3(0,1,0);
    // Sunset's source palette already darkens the canyon and its animals.
    float3 ambient=ambient_color(sun.xyz)+sunset_dusk*float3(.13,.12,.12);
#if RAY_INDIRECT && !defined(COMPUTE_PROBES)
    ambient=indirect_ambient(ambient,p.world_pos,normal);
#endif
    float3 direct=sun_color(sun.xyz)*smoothstep(-.08,.17,sun.y)*(1-.89*cloud_cover)*course_sunlight(p.world_pos,normal);
    if(aqua_meters && layer) {
        ambient=lerp(ambient,float3(.44,.57,.74),.65);
        direct*=.38;
    }
    float3 lighting=ambient*(.83+.17*saturate(normal.y))+direct*saturate(dot(normal,sun.xyz))*.63;
    if(night_lighting>=0) {
        // The 4WD headlights stay on the road as the player turns or changes camera.
        float3 offset=(p.world_pos-shadow_origin.xyz)*sun.w;
        float ahead=dot(offset.xz,shadow_axis.zw);
        float side=dot(offset.xz,shadow_axis.xy);
        float width=1.2+.18*max(ahead,0);
        float beam=(1-smoothstep(.3,1,abs(abs(side)-.85)/width))
                  *smoothstep(.5,3,ahead)*(1-smoothstep(18,65,ahead))
                  *(1-smoothstep(1.5,6,abs(offset.y)));
        lighting+=night_lighting*(float3(.14,.14,.15)+beam*float3(.94,.83,.64));
#if ROADSIDE_SCENERY
        if(abs(p.uv.z+46)<.1) {
            // Reflectors return the headlight spill toward the driver, fading from side views.
            float3 view=finish_view(p.world_pos);
            float alignment=saturate(dot(view,normalize(-offset)));
            float spill=(1-smoothstep(.32,.76,abs(side)/max(ahead,1)))
                       *smoothstep(1,4,ahead)*(1-smoothstep(28,76,ahead))*(1-smoothstep(2,6,abs(offset.y)));
            lighting+=3.8*night_lighting*spill*pow(alignment,8);
        }
#endif
    }
#if RAY_WET_REFLECTIONS || MATERIAL_ROUGHNESS
    if(abs(p.uv.z+25)<.1 && rain_strength==0)p.uv=0; // Dry markings use the original palette path.
#endif
    if(p.uv.z<0) {
        float across=p.color.r;
#if ROAD_DETAIL
        across=road_coordinates(p.uv,p.color,p.world_pos.xz*sun.w);
#endif
        float3 albedo=scene_material(p.uv,p.color,weather_seconds,across);
        // Palette-tinted pavement already includes the current course fog.
        if(panorama_fog>0 && (p.uv.z>-20.5 || p.uv.z < -36.5 || (p.uv.z < -32.5 && p.uv.z > -34.5)))albedo=lerp(albedo,panorama_fog_color.rgb,panorama_fog);
        // At grazing angles wet asphalt catches the color of the low cloud deck.
        float wet=rain_strength*(step(-1.5,p.uv.z)*step(p.uv.z,-.5)+.45*step(abs(p.uv.z+21),.1));
#if RAY_WET_REFLECTIONS || MATERIAL_ROUGHNESS
        if(p.uv.z < -24.5 && p.uv.z > -36.5)wet=rain_strength;
#endif
#if ROAD_DETAIL
        bool detail=p.uv.z < -25.5 && p.uv.z > -36.5;
        if(detail)wet=abs(p.uv.z+34)<.5 ? 1 : rain_strength;
        bool pavement=palette_pavement(p.uv.z) || abs(p.uv.z+26)<.1 || (p.uv.z < -28.5 && p.uv.z > -30.5);
        float wear=pavement?wheel_wear(p.uv,across):0;
        if(gravel_road(p.uv.z))wet*=.55+.30*wear;
        if(icy_road(p.uv.z))wet=lerp(.06+.18*wear,1,rain_strength);
        // Filter the grain in world space before perturbing the lighting.
        if(pavement) {
            float2 grit=float2(grain(p.uv.xy*21),grain(p.uv.xy*21+7))-.5;
            float3 micro=normalize(normal+float3(grit.x,0,grit.y)*(.29-.22*wear));
            lighting+=direct*(saturate(dot(micro,sun.xyz))-saturate(dot(normal,sun.xyz)))*.63;
        }
#endif
#if ROADSIDE_SCENERY
        if(abs(p.uv.z+47)<.1)wet=lerp(p.color.r,1,rain_strength);
#endif
        float3 lit=albedo*lighting*(1-.36*wet);
#if ROADSIDE_SCENERY
        if(abs(p.uv.z+46)<.1 && p.color.b>p.color.r)lit+=p.color*.55*(1-panorama_fog);
#endif
        float3 film=normal;
        if(wet>0) {
            float patches=.6+.4*grain(p.uv.xy*.29);
#if MATERIAL_ROUGHNESS
            float3 v=finish_view(p.world_pos);
            float grain_scale=grain(p.uv.xy*1.7);
            float rough=lerp(.46,.14,patches*wet);
            film=normalize(normal+float3(grain_scale-.5,0,grain(p.uv.xy*1.7+12)-.5)*rough*.12);
            float edge=1-saturate(dot(film,v));
            float weight=wet*patches*(.06+.65*pow(edge,3));
            lit=lerp(lit,finish_radiance(film,v,rough,ambient,direct),weight);
#else
            float3 eye=scene_eye_position();
            float grazing=1-saturate(dot(normal,normalize(eye-p.world_pos)));
            lit=lerp(lit,mist_color(sun.xyz),.50*wet*patches*pow(grazing,3));
#endif
        }
        if(abs(p.uv.z+4)<.1) {
            float3 eye=scene_eye_position();
            float3 reflection=reflect(-sun.xyz,normal);
            lit+=direct*.44*pow(saturate(dot(reflection,normalize(eye-p.world_pos))),38);
        }
#if ROAD_DETAIL
        if(detail && wet>0) {
            float3 v=finish_view(p.world_pos);
            float polish=pavement ? wear : .55;
            float2 grit=float2(grain(p.uv.xy*4.2),grain(p.uv.xy*4.2+7))-.5;
            film=normalize(normal+float3(grit.x,0,grit.y)*lerp(.16,.018,polish));
            float3 r=reflect(-v,film);
            float sky=smoothstep(-.14,.25,r.y);
            float grazing=1-saturate(dot(film,v));
            float gloss=pow(saturate(dot(r,sun.xyz)),lerp(14,125,polish));
            float3 reflected=mist_color(sun.xyz)*(.42+.58*sky)+direct*gloss;
            float film_cover=.36+.64*grain(p.uv.xy*.43);
            lit=lerp(lit,reflected,wet*film_cover*(.05+.43*polish)*(.34+.66*grazing*grazing));
        }
#endif
        lit*=light;
#if RAY_WET_REFLECTIONS
        if(wet>0) {
            float3 to_eye=finish_view(p.world_pos);
            lit=wet_road_reflection(lit,p.world_pos,film,to_eye,wet,p.uv.xy,p.uv.z < -25.5 && p.uv.z > -36.5);
        }
#endif
        lit=air_perspective(lit,p.depth_m,sun.xyz);
        if(sunset_valley || night_lighting>=0 || add_fixed)lit=course_color_math(lit,p.position.y);
        return float4(aqua_meters?aqua_color(lit,p.depth_m,p.position.y):lit,1);
    }
#elif defined(N64_MODE)
    float3 haze=night_lighting>=0 ? lerp(float3(.63,.36,.23),float3(.065,.08,.15),night_lighting)
                                  : lerp(float3(.65,.73,.71),float3(.52,.27,.19),sunset_dusk);
    if(p.uv.z<0) {
        float3 surface=scene_material(p.uv,p.color,0);
        if(panorama_fog>0 && p.uv.z>-20.5)surface=lerp(surface,panorama_fog_color.rgb,panorama_fog);
        if(abs(p.uv.z+4)<.1) {
            float3 n=normalize(cross(ddx(p.view_pos),ddy(p.view_pos)));
            float3 reflected=reflect(normalize(p.view_pos),n);
            float height=reflected.y*cp+reflected.z*sp;
            float heading=atan2(reflected.x,reflected.z)-atan2(sy,cy);
            float sky=triangle_sample(float2(heading*.159155,saturate(.5+.47*height)),0).b;
            float2 edge=min(p.uv.xy,1-p.uv.xy);
            surface+=float3(.22,.31,.35)*(sky-.45)*smoothstep(.025,.065,min(edge.x,edge.y));
        }
        surface*=lerp(float3(1,1,1),float3(.86,.65,.52),sunset_dusk);
        surface=lerp(surface*light,haze,p.fog);
        if(night_lighting>=0)surface*=lerp(float3(1,1,1),float3(.73,.79,.90),night_lighting);
        if(sunset_valley || night_lighting>=0 || add_fixed)surface=course_color_math(surface,p.position.y);
        return float4(aqua_meters?aqua_color(surface,p.view_pos.z*aqua_meters,p.position.y):surface,1);
    }
#endif
    float4 color;
#if ROADSIDE_SCENERY
    if(p.finish==5) {
        // Transparent texels are black, so the filtered RGB is premultiplied.
        color=filtered_leaves.Sample(asphalt_sampler,p.uv.xy);
        clip(color.a-.35);
        color.rgb=lerp(color.rgb/color.a,panorama_fog_color.rgb,saturate(panorama_fog));
    }
    else
#endif
    {
        color=tiles.Sample(tile_sampler,p.uv);
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
    }
#if ROADSIDE_SCENERY
    if(p.finish==6) {
        // Scuffs follow the native stripe UVs on both faces of the moving sign.
        float2 plate=p.uv.xy*128;
        float chips=smoothstep(.57,.78,.65*grain(plate*.52)+.35*grain(plate*1.7));
        color.rgb=lerp(color.rgb*.91,float3(.48,.50,.46),chips*.75);
    } else if(p.finish==7) {
        // World-space billows continue across tiles. The native alpha cutout
        // and the course's entry/exit fog still determine cloud coverage.
        float2 meters=p.world_pos.xz*sun.w;
        float billow=.72*grain(meters*.09)+.28*grain(meters*.27);
        float core=1-smoothstep(.40,1,length(p.color.rg));
        float3 tint=lerp(float3(.68,.76,.85),float3(1.09,1.08,1.05),smoothstep(.2,.8,billow));
        color.rgb*=lerp(float3(1,1,1),tint,core);
        p.color=1;
    }
#endif
    float3 rgb=color.rgb*p.color;
#ifdef ENHANCED_SCENE
    rgb*=light;
#ifdef REMASTERED_LIGHT
#if ROAD_DETAIL
    if(p.finish==4) {
        // The leaf UVs move with the drawbridge. Keep the native cutout above
        // and add anti-aliased metal ribs and rain sheen only on its solid bars.
        float2 deck=p.uv.xy*128;
        float2 ribs=deck*float2(3,2);
        float2 visibility=1-smoothstep(.15,.65,fwidth(ribs));
        float2 ridge=cos(ribs*6.2831853)*visibility;
        float wear=grain(deck*3.7);
        rgb*=float3(.79,.84,.87)*(.95+.10*ridge.x+.07*ridge.y+.13*(wear-.5));
        rgb=lerp(rgb,rgb*.70+float3(.13,.16,.18)*(.65+.35*ridge.y),rain_strength);
    }
#endif
    rgb*=lighting;
#if MATERIAL_ROUGHNESS || CAR_CLEAR_COAT
    if(p.finish==1 || (MATERIAL_ROUGHNESS && p.finish==2))
        rgb=car_finish(rgb,normal,finish_view(p.world_pos),p.finish,ambient*light,direct*light);
#endif
#endif
#endif
#ifdef ENHANCED_SCENE
    if(!aqua_meters)
#endif
        rgb=course_color_math(rgb,p.position.y);
#ifdef XR_COLOR
    // Original palette values are display-encoded. The compositor expects linear scene color.
    rgb=lerp(rgb/12.92,pow((rgb+.055)/1.055,2.4),step(.04045,rgb));
#endif
#ifdef N64_MODE
    if(p.uv.z==0)rgb*=lerp(float3(1,1,1),float3(.94,.75,.64),sunset_dusk);
    rgb=lerp(rgb,haze,p.fog);
    if(aqua_meters)rgb=aqua_color(rgb,p.view_pos.z*aqua_meters,p.position.y);
#elif defined(REMASTERED_LIGHT)
    rgb=air_perspective(rgb,p.depth_m,sun.xyz);
    if(aqua_meters)rgb=aqua_color(rgb,p.depth_m,p.position.y);
#endif
    return float4(rgb,1);
}
)";
        const D3D_SHADER_MACRO xr_color[]={{"XR_COLOR","1"},{nullptr,nullptr}};
        const D3D_SHADER_MACRO enhanced_macros[]={{"ENHANCED_SCENE","1"},{"REMASTERED_LIGHT","1"},{nullptr,nullptr}};
        const D3D_SHADER_MACRO n64_macros[]={{"ENHANCED_SCENE","1"},{"N64_MODE","1"},{nullptr,nullptr}};
        const auto scene_macros=n64_mode?n64_macros:enhanced_scene?enhanced_macros:nullptr;
        const D3D_SHADER_MACRO ray_macros[]={{"ENHANCED_SCENE","1"},{"REMASTERED_LIGHT","1"},{"RAY_SUN_SHADOWS",trace_sun?"1":"0"},
            {"SUN_RAYS",graphics.shadow_rays==1?"1":graphics.shadow_rays==2?"2":"4"},
            {"RAY_WET_REFLECTIONS",trace_reflections?"1":"0"},{"RAY_INDIRECT",trace_indirect?"1":"0"},
            {"ROADSIDE_SCENERY",high_detail_scenery?"1":"0"},{"ROAD_DETAIL",high_detail_road?"1":"0"},{"MATERIAL_ROUGHNESS",roughness?"1":"0"},{"CAR_CLEAR_COAT",clear_coat?"1":"0"},{nullptr,nullptr}};
        const auto pixel_macros=headset_output && !enhanced_scene?xr_color:(trace_sun || trace_reflections || trace_indirect || roughness || clear_coat || high_detail_road || high_detail_scenery)?ray_macros:scene_macros;
        ComPtr<ID3DBlob> vertex_blob,pixel_blob,errors;
        check(D3DCompile(shader,sizeof(shader),nullptr,(roughness || clear_coat || high_detail_road || high_detail_scenery)?pixel_macros:scene_macros,nullptr,"vertex_main","vs_5_0",0,0,&vertex_blob,&errors),"Vertex shader");
        check(D3DCompile(shader,sizeof(shader),nullptr,pixel_macros,nullptr,"pixel_main","ps_5_0",0,0,&pixel_blob,&errors),"Pixel shader");
        check(device->CreateVertexShader(vertex_blob->GetBufferPointer(),vertex_blob->GetBufferSize(),nullptr,&vs),"Vertex shader creation");
        check(device->CreatePixelShader(pixel_blob->GetBufferPointer(),pixel_blob->GetBufferSize(),nullptr,&ps),"Pixel shader creation");
        if(trace_indirect) {
            const D3D_SHADER_MACRO macros[]={{"ENHANCED_SCENE","1"},{"REMASTERED_LIGHT","1"},
                {"RAY_INDIRECT","1"},{"COMPUTE_PROBES","1"},{nullptr,nullptr}};
            ComPtr<ID3DBlob> blob;
            HRESULT compiled=D3DCompile(shader,sizeof(shader),nullptr,macros,nullptr,"trace_probes","cs_5_0",0,0,&blob,&errors);
            if(FAILED(compiled) && errors)throw std::runtime_error(static_cast<const char*>(errors->GetBufferPointer()));
            check(compiled,"Indirect lighting compute shader");
            check(device->CreateComputeShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,&probe_shader),"Indirect lighting shader");
            create_probes();
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter=D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
            check(device->CreateSamplerState(&sampler,&probe_sampler),"Irradiance sampler");
        }
        constexpr char background_shader[]=
#include "n64_materials.hlsl.inc"
R"(
cbuffer View : register(b0) { float cy,sy,cp,sp,aspect,zoom,distance,road_pass; float3 target; float eye_enabled;
    float4 eye_offset,eye_right,eye_up,eye_forward,projection,shadow_origin,shadow_axis,sun; float isometric; };
cbuffer Environment : register(b1) { float4 crop; uint layer,blank,fixed_color,math_layers; uint gradient_y,sky_color,pad2a,pad2b; uint4 row_subtract[32]; uint indexed_textures,add_fixed; float aqua_meters,aqua_plane_y; uint sunset_valley; float sunset_dusk; float night_lighting; float panorama_fog; float3 panorama_fog_color; uint harbor_windows; };
Texture2DArray<uint> indices:register(t1);
Texture2D<uint> colors:register(t2);
Texture2D<uint> rain_mask:register(t5);
struct Pixel {float4 position:SV_POSITION;float2 uv:TEXCOORD;};
Pixel background_vertex(uint id:SV_VertexID) {
    Pixel p;p.uv=float2((id<<1)&2,id&2);
    p.position=float4(p.uv*float2(2,-2)+float2(-1,1),0,1);return p;
}
float4 background_pixel(Pixel p):SV_TARGET {
#ifdef ENHANCED_SCENE
    float2 ray=(p.uv*float2(2,-2)+float2(-1,1))*float2(aspect,1)/zoom;
    float3 camera_ray=float3(ray,1);
    if(eye_enabled) {
        ray=(p.uv*float2(2,-2)+float2(-1,1)-projection.zw)/projection.xy;
        camera_ray=eye_right.xyz*ray.x+eye_up.xyz*ray.y+eye_forward.xyz;
    }
    float z=cp*camera_ray.z-sp*camera_ray.y;
    float3 direction=normalize(float3(cy*camera_ray.x-sy*z,cp*camera_ray.y+sp*camera_ray.z,sy*camera_ray.x+cy*z));
    if(!aqua_meters && !sunset_valley && night_lighting<0 && panorama_fog<0) {
#ifdef REMASTERED_LIGHT
        return float4(coastal_sky(direction,sun.xyz),1);
#else
        return float4(coastal_sky(direction,normalize(float3(-.56,.23,.79))),1);
#endif
    }
#endif
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
#ifdef ENHANCED_SCENE
    // Sample Aqua's own panorama at the horizon. Through the transparent deck
    // the view extends down to a world-anchored seabed, or the sea outside.
#ifdef REMASTERED_LIGHT
    // Night skylines include emissive windows and their own sky gradient.
    if((sunset_valley || panorama_fog>=0) && night_lighting<0 && !blank) {
        float3 terrain=rgb/31*(ambient_color(sun.xyz)+.47*sun_color(sun.xyz)*smoothstep(-.08,.25,sun.y));
        // Preserve the native horizon. The gradient index identifies open sky.
        float3 horizon=air_perspective(terrain,200,sun.xyz);
        if(index==1) {
            float sky_mix=smoothstep(.025,.13,direction.y)*(1-.5*sunset_dusk);
            horizon=lerp(horizon,coastal_sky(direction,sun.xyz),sky_mix*(1-saturate(panorama_fog)));
        }
        rgb=31*horizon;
    }
#endif
    if(aqua_meters && !blank) {
        rgb/=31;
        float3 axis=float3(-sy*cp,sp,cy*cp);
        float3 eye=target-distance*axis+eye_enabled*(float3(cy,0,sy)*eye_offset.x
            +float3(sy*sp,cp,-cy*sp)*eye_offset.y+axis*eye_offset.z);
        if(isometric) {
            eye+=distance*(float3(cy,0,sy)*ray.x+float3(sy*sp,cp,-cy*sp)*ray.y);
            direction=axis;
        }
        float t=(aqua_plane_y-eye.y)/min(-.0001,direction.y);
        if(direction.y<-.001 && t>0) {
            float2 meters=(eye.xz+t*direction.xz)*aqua_meters;
            float detail=grain(meters*(layer?float2(.17,.28):float2(.42,1.3)));
            float3 plane=layer?lerp(float3(.055,.25,.40),float3(.15,.43,.53),detail)
                              :lerp(float3(.20,.48,.54),float3(.35,.64,.68),detail);
            rgb=lerp(rgb,plane,smoothstep(.008,.18,-direction.y));
        }
#ifdef REMASTERED_LIGHT
        if(!layer) {
            rgb*=ambient_color(sun.xyz)+.46*sun_color(sun.xyz)*smoothstep(-.08,.25,sun.y);
            rgb=lerp(rgb,coastal_sky(direction,sun.xyz),smoothstep(.015,.10,direction.y));
            rgb=air_perspective(rgb,direction.y<-.001 && t>0?t*aqua_meters:320,sun.xyz);
        }
#endif
        rgb*=31;
    }
#endif
    if(index!=0 && (math_layers&2)) {
        float3 fixed_rgb=float3(fixed_color&31,(fixed_color>>5)&31,(fixed_color>>10)&31);
        rgb=clamp(rgb+(add_fixed?fixed_rgb:-fixed_rgb),0,31);
    }
    rgb/=31;
#if ROADSIDE_SCENERY
    if(harbor_windows && index==15 && !blank) {
        // Work inside the animated window texel, keeping its palette and timing.
        float2 size=crop.zw*fwidth(p.uv);
        float2 edge=abs(frac(crop.xy+p.uv*crop.zw)-.5);
        float2 glass=1-smoothstep(.36-size*.5,.36+size*.5,edge);
        rgb*=lerp(1,.57+.49*glass.x*glass.y,1-smoothstep(.24,.60,max(size.x,size.y)));
    }
#endif
#ifdef XR_COLOR
    rgb=lerp(rgb/12.92,pow((rgb+.055)/1.055,2.4),step(.04045,rgb));
#endif
    return float4(rgb,1);
}
#ifdef REMASTERED_LIGHT
float rain_layer(float2 uv,float scale,float speed) {
    float2 q=float2(uv.x*aspect+.17*uv.y,uv.y)*scale;
    q.y-=weather_seconds*speed;
    float2 cell=floor(q);
    float seed=grain_hash(cell);
    float2 delta=frac(q)-float2(.1+.8*seed,.5);
    float width=max(fwidth(uv.x)*aspect*scale,.012);
    float streak=(1-smoothstep(.008,.008+width,abs(delta.x)))*
                 (1-smoothstep(.07,.29,abs(delta.y)))*step(.76,seed);
    return streak;
}
float4 weather_rain_pixel(Pixel p):SV_TARGET {
    float alpha=(.22*rain_layer(p.uv,28,19)+.35*rain_layer(p.uv+.37,15,14))*rain_strength;
    float3 tint=lerp(float3(.37,.47,.61),float3(.80,.86,.90),smoothstep(-.16,.32,sun.y));
    return float4(tint*alpha,alpha);
}
#endif
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
        check(D3DCompile(background_shader,sizeof(background_shader),nullptr,pixel_macros,nullptr,"background_pixel","ps_5_0",0,0,&backdrop_pixel_blob,&errors),"Backdrop pixel shader");
        check(device->CreatePixelShader(backdrop_pixel_blob->GetBufferPointer(),backdrop_pixel_blob->GetBufferSize(),nullptr,&backdrop_ps),"Backdrop pixel shader creation");
        if(graphics.look==RaceLook::remastered) {
            // Fall back to the highest mutually supported color/depth sample count.
            for(UINT samples=graphics.msaa;samples>=2;samples/=2) {
                UINT color_levels=0,depth_levels=0;
                device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,samples,&color_levels);
                device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT,samples,&depth_levels);
                if(color_levels && depth_levels) {coverage_samples=samples;break;}
            }
            std::cout<<"Remastered MSAA samples: "<<coverage_samples<<"\n";
            D3D11_BUFFER_DESC desc_atmosphere{};
            desc_atmosphere.ByteWidth=sizeof(RaceWeather);desc_atmosphere.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            check(device->CreateBuffer(&desc_atmosphere,nullptr,&atmosphere),"Weather constants");
            ComPtr<ID3DBlob> code;
            if(weather.rain>0 || ravine_style) {
                check(D3DCompile(background_shader,sizeof(background_shader),nullptr,scene_macros,nullptr,"weather_rain_pixel","ps_5_0",0,0,&code,&errors),"Rain streak shader");
                check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&weather_rain_ps),"Rain streak pass");
                code.Reset();
            }
            check(D3DCompile(shader,sizeof(shader),nullptr,scene_macros,nullptr,"car_shadow_vertex","vs_5_0",0,0,&code,&errors),"Car shadow shader");
            check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&car_shadow_vs),"Car shadow pass");
            auto make_shadow_map=[&](UINT size,auto& dsv,auto& srv) {
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width=desc.Height=size; desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
                desc.Format=DXGI_FORMAT_R32_TYPELESS; desc.BindFlags=D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE;
                ComPtr<ID3D11Texture2D> map;
                check(device->CreateTexture2D(&desc,nullptr,&map),"Shadow map");
                D3D11_DEPTH_STENCIL_VIEW_DESC depth{};
                depth.Format=DXGI_FORMAT_D32_FLOAT; depth.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
                check(device->CreateDepthStencilView(map.Get(),&depth,dsv.GetAddressOf()),"Shadow depth view");
                D3D11_SHADER_RESOURCE_VIEW_DESC view{};
                view.Format=DXGI_FORMAT_R32_FLOAT; view.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; view.Texture2D.MipLevels=1;
                check(device->CreateShaderResourceView(map.Get(),&view,srv.GetAddressOf()),"Shadow texture view");
            };
            make_shadow_map(car_shadow_size,car_shadow_depth,car_shadow_view);
            if(!trace_sun && (sunset_style || harbor_style || sun[1]>.01f)) {
                make_shadow_map(course_shadow_size,course_shadow_depth,course_shadow_view);
                check(D3DCompile(shader,sizeof(shader),nullptr,scene_macros,nullptr,"course_shadow_vertex","vs_5_0",0,0,code.ReleaseAndGetAddressOf(),&errors),"Course shadow vertex shader");
                check(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&course_shadow_vs),"Course shadow vertex pass");
                check(D3DCompile(shader,sizeof(shader),nullptr,scene_macros,nullptr,"course_shadow_pixel","ps_5_0",0,0,code.ReleaseAndGetAddressOf(),&errors),"Course shadow alpha shader");
                check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&course_shadow_ps),"Course shadow alpha pass");
            }
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter=D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
            sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;
            sampler.ComparisonFunc=D3D11_COMPARISON_GREATER_EQUAL;
            check(device->CreateSamplerState(&sampler,&car_shadow_sampler),"Car shadow sampler");
        }
        if(enhanced_scene)material_tiles=n64::material_tiles(device.Get());
        if(enhanced_scene && !n64_mode) {
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter=D3D11_FILTER_ANISOTROPIC;
            sampler.MaxAnisotropy=16;sampler.MaxLOD=D3D11_FLOAT32_MAX;
            sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_WRAP;
            check(device->CreateSamplerState(&sampler,&asphalt_sampler),"Remastered asphalt sampler");
        }
        if(n64_mode || low_raster || (headset_output && enhanced_scene)) {
            constexpr char display_shader[]=
#include "n64_display.hlsl.inc"
            ;
            ComPtr<ID3DBlob> code;
            check(D3DCompile(display_shader,sizeof(display_shader),nullptr,nullptr,nullptr,
                n64_mode?"display_pixel":"classic_pixel","ps_5_0",0,0,&code,&errors),"N64 display shader");
            check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&display_ps),"N64 display pass");
            if(headset_output && enhanced_scene) {
                const D3D_SHADER_MACRO style[]={{"N64_EYE",n64_mode?"1":"0"},{nullptr,nullptr}};
                check(D3DCompile(display_shader,sizeof(display_shader),nullptr,style,nullptr,"eye_pixel","ps_5_0",0,0,&code,&errors),"Stereo display shader");
                check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&eye_display_ps),"Stereo display pass");
            }
            if(n64_mode && low_raster) {
                UINT color_levels=0,depth_levels=0;
                device->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM,4,&color_levels);
                device->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT,4,&depth_levels);
                if(color_levels && depth_levels)coverage_samples=4;
            }
        }
        ComPtr<ID3DBlob> rain_blob;
        check(D3DCompile(background_shader,sizeof(background_shader),nullptr,nullptr,nullptr,"rain_pixel","ps_5_0",0,0,&rain_blob,&errors),"Rain shader");
        check(device->CreatePixelShader(rain_blob->GetBufferPointer(),rain_blob->GetBufferSize(),nullptr,&rain_ps),"Rain shader creation");
        const D3D11_INPUT_ELEMENT_DESC elements[]{
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32B32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",1,DXGI_FORMAT_R32_FLOAT,0,36,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",2,DXGI_FORMAT_R32G32B32A32_FLOAT,0,40,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",6,DXGI_FORMAT_R32_UINT,0,offsetof(GpuVertex,finish),D3D11_INPUT_PER_VERTEX_DATA,0}};
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
        raster.MultisampleEnable=coverage_samples>1;
        check(device->CreateRasterizerState(&raster,&solid),"Solid rasterizer");
        raster.CullMode=D3D11_CULL_NONE;
        check(device->CreateRasterizerState(&raster,&sprite_solid),"Sprite rasterizer");
        raster.FillMode=D3D11_FILL_WIREFRAME;
        check(device->CreateRasterizerState(&raster,&wire),"Wire rasterizer");
        D3D11_DEPTH_STENCIL_DESC depth_desc{};
        depth_desc.DepthEnable=TRUE; depth_desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ALL;
        depth_desc.DepthFunc=D3D11_COMPARISON_GREATER_EQUAL;
        check(device->CreateDepthStencilState(&depth_desc,&reverse_depth),"Reverse depth state");
        if(graphics.wet_tire_spray) {
            ComPtr<ID3DBlob> code;
            check(D3DCompile(shader,sizeof(shader),nullptr,pixel_macros,nullptr,"tire_spray_pixel","ps_5_0",0,0,&code,&errors),"Tire spray shader");
            check(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&spray_shader),"Tire spray pixel shader");
            depth_desc.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;
            check(device->CreateDepthStencilState(&depth_desc,&spray_depth),"Tire spray depth test");
        }
        D3D11_BLEND_DESC no_color{};
        no_color.RenderTarget[0].RenderTargetWriteMask=0;
        check(device->CreateBlendState(&no_color,&depth_only),"Depth-only world pass");
        D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD=D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampler,&texture_sampler),"Original texture sampler");
        load_textures({});
        if(graphics.volumetric_lighting)init_volume();
        if(window || !headset_device)resize(controls.width,controls.height);
        if(window)std::clog<<"Desktop renderer ready\n"<<std::flush;
    }
    void set_course_colors(const srf::generated::CoursePackage& course) {
        course_colors = course;
        world_palette = course.day_palette;
    }
    void clear_bonus_overlay() {
        headset_hud.clear();
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
        set_headset_hud(rgba,dashboard);
        if(!classic_frame.empty() && bonus_has_dashboard)
            std::copy(dashboard.begin(),dashboard.end(),classic_frame.begin()+168*256);
        if(!bonus_overlay_ps) {
            constexpr char shader[]=R"(
cbuffer Overlay : register(b0) { float scale, center_left, brightness, play_height; float panel_scale, scene_weight, panel_left, corner_rows; float clip_top, clip_bottom; float2 origin; };
Texture2D<float4> art : register(t0);
float4 main(float4 position : SV_Position) : SV_Target {
    position.xy-=origin;
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
        if(!show_rain || (ravine_style && !n64_mode))return;
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
        textured_materials.fill(false);surface_offsets.clear();barrier_backs.clear();
        landscape_buffer.Reset();landscape_vertex_count=0;forest_banks.clear();
        course_shadow_poses.clear();course_shadow_dirty=true;probe_dirty=true;
        ray_nodes.Reset();ray_faces.Reset();ray_landscape.clear();
        for(unsigned i=0;i<sprites.size();++i) {
            const auto& sprite=sprites[i];
            if(sprite.material_table==0x8f91)textured_materials.at(sprite.phase)=true;
            if(sprite.material_table==0x9ba3)textured_materials.at(0xc5)=true;
            if(!texture_locations.emplace(std::pair<unsigned,unsigned>{sprite.material_table,sprite.phase},TextureLocation{i+1,sprite.width,sprite.height,sprite.texcoords}).second)
                throw std::runtime_error("Duplicate original texture key");
        }
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=side;desc.MipLevels=1;
        foliage_layer=course_scenery_detail ? static_cast<unsigned>(sprites.size()+1) : 0;
        foliage_mips.Reset();
        desc.ArraySize=static_cast<UINT>(sprites.size()+1+(foliage_layer!=0));desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
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
            if(foliage_layer) {
                auto* leaves=pixels.data()+foliage_layer*side*side;
                forest_roadside::foliage_tile(leaves);
                if(!foliage_mips) {
                    D3D11_TEXTURE2D_DESC leaf_desc=desc;
                    leaf_desc.ArraySize=1;leaf_desc.MipLevels=0;leaf_desc.Usage=D3D11_USAGE_DEFAULT;
                    leaf_desc.BindFlags|=D3D11_BIND_RENDER_TARGET;
                    leaf_desc.MiscFlags=D3D11_RESOURCE_MISC_GENERATE_MIPS;
                    ComPtr<ID3D11Texture2D> leaf_texture;
                    check(device->CreateTexture2D(&leaf_desc,nullptr,&leaf_texture),"Foliage mip chain");
                    context->UpdateSubresource(leaf_texture.Get(),0,nullptr,leaves,side*4,0);
                    check(device->CreateShaderResourceView(leaf_texture.Get(),nullptr,&foliage_mips),"Filtered foliage");
                    context->GenerateMips(foliage_mips.Get());
                }
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
        if((sunset_style || cruise_style || harbor_style) && !n64_mode && backdrop_weight!=value.palette_weight) {
            // Night starts approach the previous evening without crossing noon.
            const float start=float(start_hour<6?start_hour+24:start_hour);
            const float blend=value.palette_weight/31.f;
            const float hour=harbor_style ? std::lerp(3.f,6.5f,blend)
                           : cruise_style ? std::lerp(17.5f,20.f,blend) : std::lerp(start,17.65f,blend);
            sun=sun_at(hour);
            course_shadow_dirty=probe_dirty=true;
        }
        environment=value;
        if(!backdrop.available() || (backdrop_weight==value.palette_weight && backdrop_fog==value.fog_weight &&
            backdrop_fog_rgb555==value.fog_rgb555 && skyline_phase==value.skyline_phase &&
            skyline_blend_phase==value.skyline_blend_phase))return;
        const auto fog=[&](std::uint16_t color) {
            return srf::blend_environment_rgb555(color,value.fog_rgb555,value.fog_weight);
        };
        if (dusk_texture_view || palette_indices_view) {
            for (std::size_t i=0; i<world_palette.size(); ++i) {
                const auto color=srf::blend_environment_rgb555(course_colors.day_palette[i],
                    course_colors.dusk_palette[i],value.palette_weight);
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
    SceneTarget make_target(UINT w,UINT h,ComPtr<ID3D11Texture2D> color={},DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN,
                            UINT samples=1,bool readable=false) {
        SceneTarget result{w,h,std::move(color)};
        if (!result.color) {
            D3D11_TEXTURE2D_DESC desc{}; desc.Width=w; desc.Height=h;
            desc.MipLevels=1; desc.ArraySize=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count=samples; desc.BindFlags=D3D11_BIND_RENDER_TARGET | (readable?D3D11_BIND_SHADER_RESOURCE:0);
            check(device->CreateTexture2D(&desc,nullptr,&result.color),"Offscreen image");
        }
        D3D11_RENDER_TARGET_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
        check(device->CreateRenderTargetView(result.color.Get(),format==DXGI_FORMAT_UNKNOWN?nullptr:&view,&result.rtv),"Render target");
        D3D11_TEXTURE2D_DESC dd{}; dd.Width=w; dd.Height=h; dd.MipLevels=1; dd.ArraySize=1;
        dd.Format=DXGI_FORMAT_D32_FLOAT; dd.SampleDesc.Count=samples; dd.BindFlags=D3D11_BIND_DEPTH_STENCIL;
        const bool sample_depth=volume_integrate && samples==coverage_samples && format==DXGI_FORMAT_UNKNOWN;
        if(sample_depth){dd.Format=DXGI_FORMAT_R32_TYPELESS;dd.BindFlags|=D3D11_BIND_SHADER_RESOURCE;}
        ComPtr<ID3D11Texture2D> depth;
        check(device->CreateTexture2D(&dd,nullptr,&depth),"Depth image");
        D3D11_DEPTH_STENCIL_VIEW_DESC dv{};dv.Format=DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension=samples>1?D3D11_DSV_DIMENSION_TEXTURE2DMS:D3D11_DSV_DIMENSION_TEXTURE2D;
        check(device->CreateDepthStencilView(depth.Get(),&dv,&result.dsv),"Depth view");
        if(sample_depth) {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};sv.Format=DXGI_FORMAT_R32_FLOAT;
            sv.ViewDimension=samples>1?D3D11_SRV_DIMENSION_TEXTURE2DMS:D3D11_SRV_DIMENSION_TEXTURE2D;
            if(samples==1)sv.Texture2D.MipLevels=1;
            check(device->CreateShaderResourceView(depth.Get(),&sv,&result.depth_srv),"Scene depth for volumetric lighting");
        }
        return result;
    }
    void resize(UINT w,UINT h) {
        context->OMSetRenderTargets(0,nullptr,nullptr);
        desktop={};sampled_scene={};scene_view.Reset();resolved_scene.Reset();
        ComPtr<ID3D11Texture2D> back_buffer;
        if(swapchain) {
            check(swapchain->ResizeBuffers(0,w,h,DXGI_FORMAT_UNKNOWN,0),"Swapchain resize");
            check(swapchain->GetBuffer(0,IID_PPV_ARGS(&back_buffer)),"Swapchain image");
        }
        desktop=make_target(w,h,std::move(back_buffer));
        if(n64_mode || low_raster || coverage_samples>1)
            sampled_scene=make_target(low_raster?(n64_mode?320:256):w,low_raster?(n64_mode?240:224):h,{},DXGI_FORMAT_UNKNOWN,coverage_samples,coverage_samples==1);
        if(n64_mode || low_raster) {
            if(coverage_samples>1) {
                D3D11_TEXTURE2D_DESC desc{};sampled_scene.color->GetDesc(&desc);
                desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                check(device->CreateTexture2D(&desc,nullptr,resolved_scene.ReleaseAndGetAddressOf()),"N64 coverage resolve");
            }
            check(device->CreateShaderResourceView(resolved_scene?resolved_scene.Get():sampled_scene.color.Get(),
                nullptr,scene_view.ReleaseAndGetAddressOf()),"N64 scene view");
        }
    }
    void load(const srf::Mesh& mesh,std::size_t frame) {
        cloud_vertices=0;
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
    void set_race_phase(float phase){weather.seconds=phase/14.f;}
    void load_race_world(const srf::World& world,std::span<const RenderVehicle> vehicles,const srf::Rom& rom,
                         const srf::NativeRace& race,const srf::CourseScenery& scenery,float phase_fraction=0,
                         const WetTireSpray* spray=nullptr) {
        set_race_phase(float(race.phase())+phase_fraction);
        ravine_wet=ravine_style && (race.racers()[race.player_index()].body.word(0x8a)&1);
        load_world(world,0,vehicles,&rom,scenery.poses(),race.rock_poses(),race.cone_poses(),
                   race.bridge_poses(),scenery.sprites(),&race);
        if(spray)load_spray(*spray,float(race.phase())+phase_fraction);
    }
    bool rain_spray_enabled() const {return spray_shader && weather.rain>0;}
    void load_spray(const WetTireSpray& spray,float phase) {
        std::vector<GpuVertex> mist;
        spray.mesh(mist,phase,std::cos(controls.yaw),std::sin(controls.yaw),
                   std::cos(controls.pitch),std::sin(controls.pitch),normalized_distance(1),
                   [this](const auto& p){return normalize_world(p);});
        spray_count=static_cast<UINT>(mist.size());
        if(spray_count)upload_vertices(mist,spray_vertices);
    }
    void load_stunt_world(const srf::World& world,const RenderVehicle& player,const srf::Rom& rom,
                          const srf::RockFieldSession& session) {
        weather.seconds=static_cast<float>(session.tick/srf::RockFieldSession::field_rate);
        load_world(world,0,std::span(&player,1),&rom,session.drive.poses(),{},session.rules.gate_poses(),
                   session.drive.bridge_poses(),{},nullptr,session.rules.star_poses());
    }
    void load_world(const srf::World& world,std::size_t frame,std::span<const RenderVehicle> vehicles={},const srf::Rom* rom=nullptr,
                    std::span<const srf::SceneryPose> scenery={}, std::span<const srf::SceneryPose> extra_actors={},
                    std::span<const srf::SceneryPose> cones={}, std::span<const srf::SceneryPose> bridges={},
                    std::span<const srf::ScenerySprite> scene_sprites={},const srf::NativeRace* race=nullptr,
                    std::span<const srf::SceneryPose> geometric_actors={},std::span<const srf::ScenerySprite> water_sprites={},
                    std::span<const srf::SceneryPose> local_bonus={}) {
        const auto props=race ? race->prop_poses() : std::span<const srf::SceneryPose>{};
        const auto bonus=race ? race->bonus_poses() : local_bonus;
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
        if(trace_indirect) {
            for(unsigned axis=0;axis<3;++axis) {
                const unsigned cells=axis==1?probe_height:probe_width;
                course_probe_step[axis]=std::max(4.f,float((high[axis]-low[axis])/(90.*(cells-3))));
            }
        }
        std::vector<GpuVertex> triangles,lines;
        // Terrain, truss and seating share one immutable buffer for the Easy Ride session.
        std::vector<GpuVertex> landscape;
        if(enhanced_scene && !landscape_buffer) {
            const auto normalize=[this](const auto& p){return normalize_world(p);};
            // Y increases downward. Keep the surface just below the tree roots
            // or the native water crossing, both placed at course Y=0.
            const auto edges=course_scenery_detail ? forest_roadside::road_edges(world,models,forest_banks,n64::graded_road)
                                                 : std::vector<forest_roadside::Edge>{};
            if(blue_lake_style)n64::level_terrain(landscape,8,-24,normalize);
            else if(forest_style) {
                n64::level_terrain(landscape,8,-3,normalize,{1,.60f,0},edges);
                if(course_scenery_detail)for(const auto& instance:world.instances)
                    if(instance.visual_descriptor==0xa104 || instance.visual_descriptor==0xa0cc)
                        forest_roadside::tree(landscape,instance,float(foliage_layer),normalize);
            }
            else if(white_land_style) {
                n64::level_terrain(landscape,8,-23,normalize,{.85f,.90f,.95f});
                if(high_detail_scenery)for(const auto& instance:world.instances) {
                    const auto d=instance.visual_descriptor;
                    if(d!=0xbe8c && d!=0xf92c && d!=0xef00)continue;
                    const auto& pose=models.at(d)->frames.front();
                    auto bank=[&](std::initializer_list<unsigned> edge,int side) {
                        n64::snowbank(landscape,pose,edge,side,instance,normalize);
                    };
                    // Follow the outside of each turn, including the two
                    // lobes of the southern chicane. Leave the crest open.
                    if(d==0xbe8c) {bank({2,5,8},1);bank({7,10,14},-1);}
                    else if(d==0xf92c) {bank({2,23,26},1);bank({19,22,42},-1);}
                    else bank({3,9,19,24,26,27},-1);
                }
            }
            // The lake creature swims at Y=160, below the lowest road (Y=0).
            else if(lake_style)n64::level_terrain(landscape,160,-10,normalize);
            // Harbor's two water crossings bottom out at Y=-160.
            else if(harbor_style)n64::level_terrain(landscape,-152,-10,normalize);
            else if(sea_style)n64::level_terrain(landscape,8,-10,normalize);
            else if(!stunt_style && !aqua_style && !sunset_style && !night_style && !ravine_style && !sky_ramp_style)n64::coastal_terrain(landscape,world,models,normalize,edges,foliage_layer);
        }
        const auto draw_model=[&](const srf::WorldModel& model,const auto& transform,std::uint8_t opcode=0,std::int16_t descriptor_bias=0,float painter_depth=0,bool static_model=false,
                                  std::optional<std::size_t> animation_frame=std::nullopt, unsigned sign_phase=0, bool open_cockpit=false,std::span<const float> lane_offsets={}) {
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
            const bool truss=enhanced_scene && static_model && model.descriptor_address==n64::truss_descriptor;
            const bool underpass=enhanced_scene && static_model && model.descriptor_address==n64::underpass_descriptor;
            const auto descriptor=model.descriptor_address;
            const bool cruise_scenery=cruise_style && high_detail_scenery;
            const bool sky_scenery=sky_ramp_style && high_detail_scenery && static_model;
            const bool sky_deck=sky_scenery && opcode==2;
            const bool harbor_sign=harbor_style && high_detail_scenery && descriptor==0xaf58;
            const bool harbor_quay=harbor_style && high_detail_scenery && static_model &&
                (descriptor==0xfa60 || descriptor==0xfa7c);
            const bool sea_crossing=sea_style && high_detail_scenery && static_model && descriptor==0xe5b4;
            const bool sea_barrier=sea_style && high_detail_scenery && descriptor==0xb700;
            auto placed=[&](const srf::Vertex& v) {
                const auto p=transform(v);
                return normalize_world({float(p[0]),float(p[1]),float(p[2])});
            };
            if(sea_barrier)n64::sea_barrier_hinges(triangles,placed);
            if(harbor_sign)n64::harbor_hinges(triangles,pose.at(8).y,placed);
            if(cruise_scenery && descriptor==0xaf74)n64::cruise_gate_housing(triangles,placed);
            const bool lake_scenery=lake_style && high_detail_scenery && static_model;
            if(lake_scenery && descriptor==0xc404)n64::lake_hinge_plates(triangles,pose,placed);
            if(lake_scenery && descriptor==0xd5a0 && !landscape_buffer)n64::lake_approach_gravel(landscape,placed);
            // These barrier models also appear on dry straights. Limit the tide mark
            // to the two placements on the submerged section beyond the grid.
            const bool lake_waterline=lake_scenery && origin[0]==4672 &&
                ((descriptor==0xaaa4 && origin[2]==5632) || (descriptor==0xaac0 && origin[2]==10432));
            const bool viaduct=static_model && ((owl_scenery && (descriptor==0xc244 || descriptor==0xc5e0 ||
                descriptor==0xd8cc || descriptor==0xd5f4 || descriptor==0xd7ec || descriptor==0xc3e8)) ||
                (cruise_scenery && (descriptor==0xf280 || descriptor==0xf29c || descriptor==0xc5e0 ||
                 descriptor==0xc244 || descriptor==0xd8b0)) || sky_deck);
            if(owl_scenery && static_model && descriptor==0xc244 && !landscape_buffer)
                n64::owl_pier(landscape,placed);
            const bool aqua_scenery=high_detail_scenery && aqua_style && static_model;
            const bool portal=aqua_scenery && (descriptor==0xe5ec || descriptor==0xe608);
            const bool approach_lining=portal || (aqua_scenery && (descriptor==0xe4b8 || descriptor==0xe480 ||
                descriptor==0xe448 || descriptor==0xe49c));
            const auto bank=forest_banks.find(descriptor);
            if(aqua_style && static_model && !landscape_buffer && (descriptor==0xd46c || descriptor==0xd450))
                n64::aqua_panel_seams(landscape,descriptor,course_road_detail,[&](unsigned i) {
                    const auto p=transform(pose.at(i));
                    return normalize_world({float(p[0]),float(p[1])+3,float(p[2])});
                });
            const bool opponent_shell=enhanced_scene && !n64_mode && (descriptor==0xa7e8 || descriptor==0xa804 || descriptor==0xa820);
            const bool stand=descriptor==n64::left_stand || descriptor==n64::right_stand;
            const bool building=enhanced_scene && static_model && (stand || descriptor==n64::fuel_station ||
                descriptor==n64::red_cottage || descriptor==n64::blue_cottage);
            if(building && (stand || descriptor==n64::fuel_station) && !landscape_buffer)
                n64::trackside_details(landscape,descriptor,[&](const srf::Vertex& v) {
                    const auto p=transform(v);
                    return normalize_world({float(p[0]),float(p[1]),float(p[2])});
                });
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
                // The cut between King's Forest's hills uses its own eight green palette shades.
                const bool forest_cut_bank=forest_style && static_model && descriptor==0xfa44 &&
                    material>=0x48 && material<=0x4f;
                if(sea_crossing && !landscape_buffer && material==0xdf) {
                    for(std::size_t i=0;i<face.indices.size();++i) {
                        const auto& a=pose.at(face.indices[i]);
                        const auto& b=pose.at(face.indices[(i+1)%face.indices.size()]);
                        if(a.x==b.x)n64::sea_gravel_edge(landscape,a,b,placed);
                    }
                }
                const bool detailed_forest_deck=course_road_detail && forest_style && static_model &&
                    n64::forest_bridge_surface(descriptor,material);
                const bool ravine_gravel=ravine_style && (descriptor==0xf0f8 || descriptor==0xf114) &&
                    (material==0x89 || material==0x8a);
                if(open_cockpit && material==0x0d)continue; // exterior canopy, above the F-TYPE's seat
                if(truss && face.indices.size()==4 && (material==0x34 || material==0x36)) {
                    // The two paint indices are opposite windings of the same flat member.
                    if(material==0x34 && !landscape_buffer) {
                        std::array<n64::Vec,4> panel;
                        n64::Vec depth;
                        for(unsigned a=0;a<3;++a)depth[a]=float(basis[2][a]-origin[a]);
                        for(unsigned i=0;i<4;++i) {
                            const auto p=transform(pose.at(face.indices[i]));
                            panel[i]={float(p[0]),float(p[1]),float(p[2])};
                        }
                        n64::steel_beam(landscape,panel,depth,[this](const auto& p){return normalize_world(p);});
                    }
                    continue;
                }
                const float intensity=.55f+.35f*std::abs(float(static_cast<std::int8_t>(face.attributes[4]))/127.f);
                float r=.7f*intensity,g=.25f*intensity,b=.7f*intensity;
                int palette_index=-1;
                if(srf::generated::world_material_kinds[material]) {
                    // Original palette and shade ramps; lighting direction is
                    // still a presentation choice, not original fixed-point parity.
                    const auto shade=unsigned(std::clamp(int(intensity*15),6,15));
                    palette_index=srf::generated::world_material_indices[material][shade];
                    const auto color=world_palette[palette_index];
                    r=float(color&31)/31; g=float((color>>5)&31)/31; b=float((color>>10)&31)/31;
                }
                const bool course_sign=material==0xc5 && sign_phase!=0;
                if(ravine_style && high_detail_scenery && static_model && !landscape_buffer &&
                   opcode==2 && material==0x5a && face.indices.size()==3 &&
                   std::abs(static_cast<std::int8_t>(face.attributes[4]))>=90) {
                    n64::ravine_shard(landscape,pose.at(face.indices[0]),pose.at(face.indices[1]),
                                      pose.at(face.indices[2]),{r,g,b},placed);
                }
                const auto texture=texture_locations.find(course_sign ? std::pair{0x9ba3u,sign_phase} : std::pair{0x8f91u,material});
                const bool textured=texture!=texture_locations.end() && face.indices.size()==4;
                const bool original_player=enhanced_scene && n64::original_player_shell(descriptor);
                const bool windshield=(opponent_shell || original_player) && material==0x0d;
                const bool side_glass=std::abs(int(std::int8_t(face.attributes[3])))>std::abs(int(std::int8_t(face.attributes[5])));
                int left=INT_MAX,right=INT_MIN,top=INT_MAX,bottom=INT_MIN;
                if(windshield)for(auto index:face.indices) {
                    const auto& v=pose.at(index);
                    const auto horizontal=side_glass?v.z:v.x;
                    left=std::min(left,horizontal);right=std::max(right,horizontal);
                    top=std::min(top,v.y);bottom=std::max(bottom,v.y);
                }
                bool along_z=false;
                if(building && face.indices.size()>=3) {
                    // Use the wider horizontal axis of the face for upright cladding.
                    const auto& a=pose.at(face.indices[0]);const auto& b=pose.at(face.indices[2]);
                    along_z=std::abs(a.z-b.z)>std::abs(a.x-b.x);
                }
                if(textured) r=g=b=1;
                const bool barrier=enhanced_scene && static_model && !textured && (material==0x7b || material==0x7c);
                if(barrier && opcode==2 && face.indices.size()==4) {
                    std::array<n64::Vec,4> panel;
                    for(unsigned i=0;i<4;++i) {
                        const auto p=transform(pose.at(face.indices[i]));
                        panel[i]={float(p[0]),float(p[1]),float(p[2])};
                    }
                    const auto marker=sky_deck ? n64::BarrierMarker::guide_lamp :
                        (owl_scenery || cruise_scenery) ? n64::BarrierMarker::reflector : n64::BarrierMarker::painted;
                    n64::barrier_trim(triangles,panel,[this](const auto& p){return normalize_world(p);},marker);
                }
                // Each solid rib panel has a short flange axis and a long axis along the arch.
                const bool steel_rib=aqua_scenery && n64::aqua_tunnel_rib(descriptor) &&
                    material>=0x2a && material<=0x2f && face.indices.size()==4;
                float rib_u=0,rib_v=0;
                if(steel_rib) {
                    const auto& a=pose[face.indices[0]];
                    auto meters=[&](unsigned corner) {
                        const auto& b=pose[face.indices[corner]];
                        return float(std::hypot(b.x-a.x,b.y-a.y,b.z-a.z)/90.);
                    };
                    rib_u=meters(1);rib_v=meters(3);
                }
                auto vertex=[&](unsigned index) {
                    auto local=pose.at(index);
                    // The F-TYPE wing has coincident white and dark faces in the
                    // ROM. Separate its underside in the two-sided player pass.
                    if(enhanced_scene && descriptor==n64::f_type_player_shell && material==4 && local.y==-64)
                        local.y+=3;
                    // The Coupe's yellow rear deck and black underside share
                    // four vertices with opposite winding in the source mesh.
                    if(original_player && descriptor==n64::coupe_player_shell && material==4 && face.attributes[5]==224)
                        local.y+=3;
                    // The gantry's two inner leg faces coincide with the barrier
                    // faces at +/-1920. Clear the 11-unit rail cap as well.
                    if(enhanced_scene && model.descriptor_address==0xac9c && std::abs(local.x)==1920)
                        local.x+=local.x<0?24:-24;
                    auto point=transform(local);
                    for(unsigned a=0;a<3;++a)for(unsigned b=0;b<3;++b)
                        point[a]+=(basis[b][a]-origin[a])*shift[b];
                    const auto p=normalize_world({float(point[0]),float(point[1]),float(point[2])});
                    GpuVertex out{p[0],p[1],p[2],r,g,b};
                    // Native body panels and their racing-number decals. Glass,
                    // lights, undertrays and trim retain their own materials.
                    if((opponent_shell || original_player) && (material==1 || material==6 || material==8 ||
                       material==0x0a || material==0xbc || material==0xbd || material==0xbf))out.finish=SurfaceFinish::paint;
                    out.painter_depth=support_depth;
                    if(support_depth!=0)out.road_origin_bias={road_origin[0],road_origin[1],road_origin[2],road_bias};
                    if((detailed_forest_deck && descriptor==0xfa0c) ||
                       (course_road_detail && ravine_style && static_model && descriptor==0xe368 && material==0xc9)) {
                        // Anchor grain to the board's rest pose while the native vertices animate.
                        const auto& rest=model.frames.front()[index];
                        out.texture=-33;out.u=rest.x/90.f;out.v=rest.z/90.f;
                        out.r=.56f;out.g=.39f;out.b=.20f;
                    } else if(ravine_style && high_detail_scenery && descriptor==0xb4d0) {
                        // Keep the bedding attached to the boulder as it tumbles.
                        const auto nx=std::abs(static_cast<std::int8_t>(face.attributes[3]));
                        const auto ny=std::abs(static_cast<std::int8_t>(face.attributes[4]));
                        const auto nz=std::abs(static_cast<std::int8_t>(face.attributes[5]));
                        out.texture=-41;
                        out.u=(nx>ny && nx>nz ? local.z : local.x)/50.f;
                        out.v=(ny>=nx && ny>=nz ? local.z : -local.y)/50.f;
                    } else if(course_road_detail && static_model && material==0xdf &&
                              ((sea_style && descriptor==0xe5b4) ||
                               (harbor_style && (descriptor==0xfa60 || descriptor==0xfa7c)))) {
                        // Only the flooded roadbed gets ripples. World UVs join the crossing panels.
                        out.texture=-34;out.u=float(point[0])/90.f;out.v=float(point[2])/90.f;
                    } else if(harbor_quay && material==0xde) {
                        // Both crossing sections share a coping height at local y=0.
                        out.texture=-50;out.u=float(point[0]+point[2])/90;out.v=(160.f-local.y)/90;
                        out.r=.59f;out.g=.62f;out.b=.60f;
                    } else if(harbor_sign && material>=0x5f && material<=0x61) {
                        // Keep the grain on each leaf as it folds flat.
                        const auto& rest=model.frames.front()[index];
                        out.texture=-48;out.u=float(material==0x61?rest.z:rest.x)/90;
                        out.v=float(material==0x5f?rest.z:-rest.y)/90;
                        out.r=.54f;out.g=.59f;out.b=.62f;
                    } else if(sea_barrier && material>=0x40 && material<=0x47) {
                        out.texture=-48;out.u=float(local.x+local.z)/90;out.v=-float(local.y)/90;
                        out.r=.40f;out.g=.43f;out.b=.43f;
                    } else if(building && face.indices.size()>=3) {
                        n64::building_surface(out,descriptor,material,local,along_z);
                    } else if(windshield) {
                        out.texture=-4;
                        out.u=float((side_glass?local.z:local.x)-left)/std::max(1,right-left);
                        out.v=float(bottom-local.y)/std::max(1,bottom-top);
                    } else if(textured) {
                        const auto corner=std::find(face.indices.begin(),face.indices.end(),index)-face.indices.begin();
                        // GSU01:9171..917D consumes original metadata UV bytes
                        // in face-vertex order; no guessed decal rotation.
                        const float u=float(texture->second.uv[corner][0])+.5f;
                        const float v=float(texture->second.uv[corner][1])+.5f;
                        out.u=u/128.f;out.v=v/128.f;out.texture=float(texture->second.layer);
                        if((sea_barrier && material==0xb0) || (harbor_sign && material==0xe2))
                            out.finish=SurfaceFinish::barrier_paint;
                        if(sky_scenery && descriptor==0xb850 && material==0xda) {
                            // Normalized tile coordinates feather the shading out before the square boundary.
                            out.finish=SurfaceFinish::cloud_bank;
                            out.r=local.x/1280.f;out.g=local.z/1280.f;
                        }
                        if(course_road_detail && lake_style && descriptor==0xc404 && material==0xd7)
                            out.finish=SurfaceFinish::steel_grating;
                    } else if(enhanced_scene && static_model && face.indices.size()>=3) {
                        // Easy Ride's alternating asphalt, concrete barriers and hillside greens.
                        if(aqua_style ? n64::aqua_driving_surface(descriptor,opcode,material) : n64::asphalt_material(descriptor,material)) out.texture=-1;
                        if((sunset_style || night_style || forest_style || white_land_style || lake_style || ravine_style || sky_ramp_style) && n64::graded_road(opcode,material))out.texture=-21;
                        if(detailed_forest_deck)out.texture=-21; // fixed bridge approaches
                        // Stunt decks use course-specific dirt and ice colours. Keep their palette under the surface grain.
                        if(stunt_style && opcode==2 && std::abs(static_cast<std::int8_t>(face.attributes[4]))>=64)
                            out.texture=-21;
                        if((sea_style || harbor_style) && n64::sea_breeze_road(opcode,material))out.texture=-21;
                        if((sunset_style || ravine_style) && material>=0x5a && material<=0x62) {
                            out.texture=-22;
                            if(high_detail_scenery) {
                                const int up=std::abs(static_cast<std::int8_t>(face.attributes[4]));
                                out.texture=up>=90 ? -42 : -41; // loose talus on shelves, bedrock on cliffs
                            }
                        }
                        if(aqua_style && n64::aqua_tunnel_rib(descriptor) && material>=0x2a && material<=0x2f)
                            out.texture=-5; // exposed metal arch, including the overhead span
                        // Lane stripes, curbs and the start checker share the wet road surface.
                        if((trace_reflections || roughness) && opcode==2 && static_cast<std::int8_t>(face.attributes[4])>=64 &&
                           (material==0x1e || material==0x27 || material==0x32))out.texture=-25;
                        // This chicane already models its sloped curb faces, including the triangular ends.
                        if(course_road_detail && white_land_style && descriptor==0xf92c && (material==0x1e || material==0x32))
                            out.texture=-27;
                        if(barrier) out.texture=-2;
                        if(material>=0x55 && material<=0x59)out.texture=white_land_style?-23:-3;
                        if(material==0xa8 && !white_land_style)out.texture=-3;
                        if(forest_cut_bank) {out.texture=-3;out.g=.60f;}
                        if(bank!=forest_banks.end() && (forest_cut_bank || (material>=0x55 && material<=0x58))) {
                            out.texture=-39;out.r=bank->second.at(index);
                        }
                        // The truss carries two grassy banks bordering the lower road at X = ±960.
                        if(course_scenery_detail && truss && material==0x57) {
                            out.texture=-40;out.r=1;out.g=0;
                            out.b=std::clamp((std::abs(local.x)-960)/forest_roadside::verge_width,0.f,1.f);
                        }
                        if(model.descriptor_address==0xad60 && material>=0x4c && material<=0x4f) {
                            out.texture=-9;
                            out.r=1.12f-.085f*(material-0x4c);out.g=0;out.b=.78f;
                        }
                        if(out.texture<0) {out.u=float(point[0])/90.f;out.v=float(point[2])/90.f;}
                        if(forest_cut_bank || (forest_style && descriptor==0xfa44 && material==0x57)) {
                            // Map uphill as well as across the verge so its upright faces retain grain.
                            // At ground level the coordinates match the surrounding forest floor.
                            const float uphill=(local.x<0 ? -1.f : 1.f)*local.y*.35f/90.f;
                            out.u+=float(basis[0][0]-origin[0])*uphill;
                            out.v+=float(basis[0][2]-origin[2])*uphill;
                        }
                        if(out.texture==-22 || out.texture==-41) {
                            out.u=(float(point[0])*.71f+float(point[2])*.71f)/90.f;
                            out.v=float(point[1])/90.f;
                        }
                        if(barrier) {out.u=(float(point[0])*.81f+float(point[2])*.59f)/90.f;out.v=float(point[1])/90.f;}
                        if(lake_waterline && barrier) {
                            out.texture=-50;out.u=float(point[2])/90;out.v=-float(local.y)/90;
                        }
                        if(underpass && out.texture==-1) {
                            out.texture=-6;out.u=float(local.x)/90;out.v=float(local.z)/90;
                        } else if(truss && (material==0x29 || material==0x88 || material==0x26)) {
                            out.texture=material==0x29?-7:-8;
                            out.u=float(material==0x88?local.y:local.z)/90;out.v=float(local.x)/90;
                        }
                        // Each portal has a native black hexagon across its mouth during the backdrop transition.
                        // Only the longitudinal lining faces carry concrete.
                        const bool lining=approach_lining && ((material==0xa0 && face.indices.size()==4) || (portal &&
                           ((material>=0x69 && material<=0x6d) || material==0x73 || material==0x74 || barrier)));
                        if(lining || (viaduct && (material==0x25 || material==0x26 || material==0x6c || material==0x6d)) ||
                           (sky_deck && (material==0x27 || barrier))) {
                            out.texture=-43;
                            const int nx=std::abs(static_cast<std::int8_t>(face.attributes[3]));
                            const int ny=std::abs(static_cast<std::int8_t>(face.attributes[4]));
                            const int nz=std::abs(static_cast<std::int8_t>(face.attributes[5]));
                            // Dominant-axis projection keeps joints on upright, sloped and overhead panels.
                            out.u=float(nx>ny && nx>nz ? local.z : local.x)/90;
                            out.v=float(ny>=nx && ny>=nz ? local.z : -local.y)/90;
                        }
                        if(steel_rib) {
                            const auto corner=std::find(face.indices.begin(),face.indices.end(),index)-face.indices.begin();
                            const float u=corner==1 || corner==2 ? 1.f : 0.f,v=corner>=2 ? 1.f : 0.f;
                            out.texture=-44;
                            out.u=rib_u<rib_v ? u : v;
                            out.v=rib_u<rib_v ? v*rib_v : u*rib_u;
                        }
                        if(!lane_offsets.empty()) {
                            // Detailed asphalt uses the red component for the interpolated
                            // distance across the road. Its grain still uses stable world UVs.
                            if(out.texture==-1 && aqua_style && n64::aqua_tunnel_rib(descriptor)) {
                                out.texture=-31; // solid steel ties across the transparent deck
                                out.u=float(local.x)/90;out.v=float(local.z)/90;
                            } else if(out.texture==-21 || ravine_gravel) {
                                // Keep the course palette, including its current fog or dusk colour.
                                out.texture=ravine_gravel?-36:(white_land_style?-35:-32);out.u=lane_offsets[index];
                                out.v=backdrop_colors?float(palette_index):-1.f;
                            } else if(out.texture==-1){out.texture=-26;out.r=lane_offsets[index];}
                            if(out.texture==-6)out.texture=-29; // concrete underpass
                            if(out.texture==-7 && truss)out.texture=-30; // upper deck
                        }
                    }
                    return out;
                };
                if(portal && material==0x2c && face.indices.size()==4) {
                    n64::aqua_approach(triangles,{vertex(face.indices[0]),vertex(face.indices[1]),
                                                  vertex(face.indices[2]),vertex(face.indices[3])});
                    continue;
                }
                if(!lane_offsets.empty() && face.indices.size()==4 && n64::road_curb(descriptor,face.attributes[1],material,sea_style || harbor_style || white_land_style || lake_style)) {
                    std::array<GpuVertex,4> strip;
                    for(unsigned i=0;i<4;++i) {
                        strip[i]=vertex(face.indices[i]);
                        const auto p=transform(pose[face.indices[i]]);
                        strip[i].u=float(p[0])/90; strip[i].v=float(p[2])/90;
                    }
                    n64::beveled_curb(triangles,strip,normalized_distance(12));
                    continue;
                }
                if(face.indices.size()==2) {
                    if(original_player && rendered_camera()==RaceCamera::cockpit)continue;
                    lines.push_back(vertex(face.indices[0])); lines.push_back(vertex(face.indices[1]));
                } else for(std::size_t i=1;i+1<face.indices.size();++i) {
                    for(auto index:{face.indices[0],face.indices[i],face.indices[i+1]}) triangles.push_back(vertex(index));
                    if(add_back)for(auto index:{face.indices[0],face.indices[i+1],face.indices[i]}) triangles.push_back(vertex(index));
                }
            }
        };
        std::vector<std::pair<unsigned,std::size_t>> caster_poses;
        caster_poses.reserve(course_shadow_poses.size());
        // Sky Ramp's horizontal cloud tiles belong behind the elevated road.
        // The course fog controls visibility when driving through the bank.
        if(sky_ramp_style)for(const auto& instance:world.instances) {
            if(instance.visual_descriptor!=0xb850)continue;
            const auto* model=models.at(instance.visual_descriptor);
            draw_model(*model,[&](const srf::Vertex& v){return srf::world_position(v,instance);},instance.opcode,0,0,true);
        }
        cloud_vertices=static_cast<UINT>(triangles.size());
        for(const auto& instance:world.instances) {
            if(sky_ramp_style && instance.visual_descriptor==0xb850)continue;
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
                if(course_shadow_depth || trace_course())
                    caster_poses.emplace_back(instance.command_offset,deck_frame.value_or(frame)%found->second->frames.size());
                std::span<const float> lanes;
                if(course_road_detail && instance.opcode==2 && !world.route_points.empty()) {
                    auto [it,inserted]=road_offsets.try_emplace(instance.command_offset);
                    if(inserted) {
                        for(const auto& v:found->second->frames.front())
                            it->second.push_back(n64::road_offset(srf::world_position(v,instance),world.route_points,night_style || white_land_style || lake_style || ravine_style || sky_ramp_style));
                    }
                    lanes=it->second;
                }
                draw_model(*found->second,[&](const srf::Vertex& v){return srf::world_position(v,instance);},
                           instance.opcode,signed_bias,painter_depth,true,deck_frame,instance.sign_phase,false,lanes);
            }
        }
        // Camera-facing sprites and moving racers follow the stationary course mesh.
        course_shadow_vertices=static_cast<UINT>(triangles.size());
        if(caster_poses!=course_shadow_poses) {
            if(trace_course()) {
                const auto geometry=std::span<const GpuVertex>(triangles).subspan(cloud_vertices);
                const ray_shadows::Tree tree(geometry,landscape.empty()?ray_landscape:landscape);
                ray_nodes=ray_shadows::upload(device.Get(),tree.nodes);
                ray_faces=ray_shadows::upload(device.Get(),tree.faces);
            }
            course_shadow_dirty=true;
            probe_dirty=true;
            course_shadow_poses=std::move(caster_poses);
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
            // Both wooded courses use this native trunk and crown pair.
            if(course_scenery_detail &&
               (sprite.table==0x9971 || sprite.table==0x9973))continue;
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
        // From the banked descent the road crosses in front of the lake creature.
        // Depth-test lake scenery against the deck before applying racer priority.
        if(lake_style)draw_actors(scenery);
        const auto draw_shell=[&](const RenderVehicle& vehicle,bool open_cockpit=false) {
            const auto found=models.find(vehicle.descriptor);
            if(found==models.end()) throw std::runtime_error("Original vehicle visual descriptor missing from generated world");
            draw_model(*found->second,[&](const srf::Vertex& v) {
                const std::array<float,3> local{float(v.x),float(v.y),float(v.z)};
                auto p=vehicle.position;
                for(unsigned a=0;a<3;++a) for(unsigned b=0;b<3;++b) p[a]+=local[b]*vehicle.orientation[b*3+a];
                return p;
            },0,0,0,false,std::nullopt,0,open_cockpit);
        };
        const bool mesh_rivals=enhanced_scene && !n64_mode;
        const auto rival_mesh=[&](const RenderVehicle& car){return mesh_rivals || (enhanced_scene && car.remote_rally_truck);};
        rival_start=static_cast<UINT>(triangles.size());
        if(rendered_camera()==RaceCamera::isometric) {
            car_centers={};
            for(std::size_t i=0;i<std::min(vehicles.size(),car_centers.size());++i) {
                const auto p=normalize_world(vehicles[i].position);
                car_centers[i]={p[0],p[1],p[2],1};
            }
        }
        // Draw the entire rival against geometric depth. The later actor pass
        // uses the SNES road priority, which can put a hill behind a hidden car.
        if(mesh_rivals)for(const auto& vehicle:vehicles)if(!vehicle.player && !vehicle.source_model && !vehicle.remote_rally_truck)draw_shell(vehicle);
        two_sided_world_start=static_cast<UINT>(triangles.size());
        for(const auto& vehicle:vehicles)if(rival_mesh(vehicle) && !vehicle.player && !vehicle.source_model) {
            n64::draw_car(triangles,vehicle,n64_mode,[this](const auto& p){return normalize_world(p);});
            if(!vehicle.remote_rally_truck)for(const auto& sprite:vehicle.sprites)if(!sprite.wheel)draw_sprite(sprite,vehicle_winding);
        }
        world_vertex_count=static_cast<UINT>(triangles.size());
        world_line_count=static_cast<UINT>(lines.size());
        vehicle_depth_pass=rom && !vehicles.empty();
        if(!lake_style)draw_actors(scenery);
        for(const auto actors:{extra_actors,cones,props,pickups,bonus})draw_actors(actors);
        for(const auto& vehicle:vehicles)if(vehicle.source_model || !enhanced_scene || (!vehicle.player && !rival_mesh(vehicle)))draw_shell(vehicle);
        // Wheel and eye sprites draw over the rebuilt actor depth. The trees
        // above belong to the world and occlude cars only at opaque texels.
        const auto surface_count=static_cast<UINT>(triangles.size());
        if(enhanced_scene) for(const auto& vehicle:vehicles) if(vehicle.player && !vehicle.source_model) {
            const auto p=normalize_world(vehicle.position);
            shadow_origin={p[0],p[1],p[2],float(world_radius/90)};
            shadow_axis={vehicle.orientation[0],vehicle.orientation[2],vehicle.orientation[6],vehicle.orientation[8]};
            if(n64::original_player_shell(vehicle.descriptor))draw_shell(vehicle);
            n64::draw_car(triangles,vehicle,n64_mode,[this](const auto& p){return normalize_world(p);});
        }
        car_shadow_vertices=static_cast<UINT>(triangles.size())-surface_count;
        if(enhanced_scene && (grid_sweep || !race_camera.mounted()))
            for(const auto& vehicle:vehicles)if(vehicle.player && !vehicle.source_model && n64::original_player_shell(vehicle.descriptor))
                for(const auto& sprite:vehicle.sprites)if(!sprite.wheel)draw_sprite(sprite,vehicle_winding);
        // The exterior mesh and moving trackside actors reflect even when the
        // player switches to a mounted camera. Exclude the cockpit overlay.
        if(trace_reflections && (weather.rain>0 || ravine_wet)) {
            reflection_actors.update_actors(std::span<const GpuVertex>(triangles).subspan(course_shadow_vertices));
            actor_nodes.update(device.Get(),context.Get(),reflection_actors.nodes);
            actor_faces.update(device.Get(),context.Get(),reflection_actors.faces);
        }
        if(mesh_rivals && rendered_camera()==RaceCamera::cockpit)
            for(const auto& vehicle:vehicles)if(vehicle.player && !vehicle.source_model) {
                if(vehicle.descriptor==n64::f_type_player_shell)draw_shell(vehicle,true);
                n64::draw_car(triangles,vehicle,false,[this](const auto& p){return normalize_world(p);},true);
            }
        for(const auto sprites:{race ? race->spray_sprites() : water_sprites,
                                race ? race->breakable_sprites() : std::span<const srf::ScenerySprite>{}}) {
            for(const auto& sprite:sprites) {
                const auto p=sprite.position;
                draw_sprite({sprite.table,sprite.frame,{float(p.x),float(p.y),float(p.z)},float(sprite.diameter)},vehicle_winding);
            }
        }
        for(const auto& vehicle:vehicles)if(vehicle.source_model || (!rival_mesh(vehicle) && (!enhanced_scene || !vehicle.player)))
            for(const auto& sprite:vehicle.sprites)draw_sprite(sprite,vehicle_winding);
        if(!landscape.empty()) {
            upload_vertices(landscape,landscape_buffer);
            landscape_vertex_count=static_cast<UINT>(landscape.size());
            if(trace_course())ray_landscape=std::move(landscape);
        }
        upload(triangles,lines,surface_count);
    }
    void sync_size(){
        if(!output_window)return;
        RECT r{};GetClientRect(output_window,&r);
        if(r.right>0 && r.bottom>0 && (desktop.width!=unsigned(r.right) || desktop.height!=unsigned(r.bottom)))
            resize(r.right,r.bottom);
    }
    void render(std::span<const std::uint32_t> demo_frame={},const std::function<void(unsigned)>& battle_camera={},
                std::span<const std::uint32_t> hud={}) {
        sync_size();
        if(game_xr && swapchain) {
            // The companion is a third camera. Give the two headset views priority.
            const auto now=std::chrono::steady_clock::now();
            if(now<next_mirror) {
                if(battle_camera)battle_camera(0);
                present({},false);return;
            }
            next_mirror=now+std::chrono::milliseconds(33);
        }
        const auto& target=sampled_scene.color?sampled_scene:desktop;
        if(demo_frame.empty() && hud.empty() && bonus_has_dashboard)demo_frame=classic_frame;
        if(battle_camera){
            if(!game_xr){
                battle_camera(0);
                render_scene(target,nullptr,{},0);
            }
            battle_camera(1);
            render_scene(target,nullptr,{},game_xr?-1:1);
            battle_camera(0); // The headset always follows player one.
        }else render_scene(target,nullptr,demo_frame);
        if(!n64_mode && coverage_samples>1) {
            context->OMSetRenderTargets(0,nullptr,nullptr);
            context->ResolveSubresource(desktop.color.Get(),0,sampled_scene.color.Get(),0,DXGI_FORMAT_R8G8B8A8_UNORM);
        }
        if(n64_mode || low_raster) {
            context->OMSetRenderTargets(1,desktop.rtv.GetAddressOf(),nullptr);
            if(resolved_scene)context->ResolveSubresource(resolved_scene.Get(),0,sampled_scene.color.Get(),0,DXGI_FORMAT_R8G8B8A8_UNORM);
            float w=float(desktop.width),h=float(desktop.height);
            if(low_raster) {
                constexpr float black[]{0,0,0,1};
                context->ClearRenderTargetView(desktop.rtv.Get(),black);
                w=std::min(w,h*4/3);h=w*3/4;
            }
            const D3D11_VIEWPORT viewport{(desktop.width-w)/2,(desktop.height-h)/2,w,h,0,1};
            context->RSSetViewports(1,&viewport);context->RSSetState(sprite_solid.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(backdrop_vs.Get(),nullptr,0);context->PSSetShader(display_ps.Get(),nullptr,0);
            context->PSSetShaderResources(0,1,scene_view.GetAddressOf());
            context->Draw(3,0);
            ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(0,1,&empty);
        }
        if(!hud.empty())render_course_map(hud,true);
        else if(!demo_frame.empty())render_course_map(demo_frame,true);
        else present();
    }
    // Measure repeated draws of the screenshot pose. Keep staging readback,
    // simulation, shader compilation and PNG encoding outside the GPU interval.
    void benchmark_offscreen() {
        if(output_window)throw std::runtime_error("GPU timing requires an offscreen render");
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;ComPtr<IDXGIAdapter1> adapter1;
        check(device.As(&dxgi),"GPU timing DXGI device");
        check(dxgi->GetAdapter(&adapter),"GPU timing adapter");
        check(adapter.As(&adapter1),"GPU timing adapter information");
        DXGI_ADAPTER_DESC1 desc{};
        check(adapter1->GetDesc1(&desc),"GPU timing adapter name");
        if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)throw std::runtime_error("GPU timing requires a hardware adapter");
        constexpr unsigned samples=48;
        std::array<ComPtr<ID3D11Query>,samples+1> stamps;
        D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP,0};
        for(auto& stamp:stamps)check(device->CreateQuery(&query,&stamp),"GPU timestamp");
        query.Query=D3D11_QUERY_TIMESTAMP_DISJOINT;
        ComPtr<ID3D11Query> frequency;
        check(device->CreateQuery(&query,&frequency),"GPU timestamp frequency");
        // Sub-millisecond scenes can finish twelve draws before the GPU leaves idle clocks.
        query.Query=D3D11_QUERY_EVENT;
        ComPtr<ID3D11Query> warm;
        check(device->CreateQuery(&query,&warm),"GPU warm-up query");
        const auto start=GetTickCount64();
        do {
            for(unsigned i=0;i<12;++i)render();
            context->End(warm.Get());context->Flush();
            HRESULT ready;
            while((ready=context->GetData(warm.Get(),nullptr,0,0))==S_FALSE) {
                if(GetTickCount64()-start>30000)throw std::runtime_error("Timed out warming the GPU");
                Sleep(1);
            }
            check(ready,"GPU warm-up");
        } while(GetTickCount64()-start<300);
        context->Begin(frequency.Get());
        context->End(stamps.front().Get());
        for(unsigned i=1;i<=samples;++i) {render();context->End(stamps[i].Get());}
        context->End(frequency.Get());
        context->Flush();
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};
        const auto deadline=GetTickCount64()+30000;
        HRESULT ready;
        while((ready=context->GetData(frequency.Get(),&result,sizeof(result),0))==S_FALSE) {
            if(GetTickCount64()>deadline)throw std::runtime_error("Timed out reading GPU timestamps");
            Sleep(1);
        }
        check(ready,"Read GPU timestamp frequency");
        if(result.Disjoint || !result.Frequency)throw std::runtime_error("GPU timestamp interval was disjoint");
        std::array<double,samples> times;
        UINT64 previous=0;
        for(unsigned i=0;i<=samples;++i) {
            UINT64 ticks{};
            if(context->GetData(stamps[i].Get(),&ticks,sizeof(ticks),0)!=S_OK || ticks<previous)
                throw std::runtime_error("Invalid frame timestamp");
            if(i)times[i-1]=1000.*(ticks-previous)/result.Frequency;
            previous=ticks;
        }
        std::sort(times.begin(),times.end());
        std::wcout<<L"GPU adapter: "<<desc.Description<<L"\n";
        std::cout<<"GPU render ("<<samples<<" frames at "<<desktop.width<<"x"<<desktop.height<<"): median "
                 <<(times[samples/2-1]+times[samples/2])/2<<" ms, p95 "<<times[(95*samples+99)/100-1]<<" ms\n";
    }
    void render_eye(const SceneTarget& target,const SceneEye& eye) {
        if(!eye_display_ps) {render_scene(target,&eye,{});return;}
        // Both eyes use the style's display-encoded materials, then submit linear color to OpenXR.
        auto& surface=eye_surfaces[{target.width,target.height}];
        if(!surface.view) {
            surface.scene=make_target(target.width,target.height,{},DXGI_FORMAT_UNKNOWN,coverage_samples,coverage_samples==1);
            if(coverage_samples>1) {
                D3D11_TEXTURE2D_DESC d{};surface.scene.color->GetDesc(&d);
                d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                check(device->CreateTexture2D(&d,nullptr,&surface.resolved),"Stereo MSAA resolve");
            }
            check(device->CreateShaderResourceView(surface.resolved?surface.resolved.Get():surface.scene.color.Get(),nullptr,&surface.view),"Stereo scene view");
        }
        render_scene(surface.scene,&eye,{});
        context->OMSetRenderTargets(1,target.rtv.GetAddressOf(),nullptr);
        if(surface.resolved)context->ResolveSubresource(surface.resolved.Get(),0,surface.scene.color.Get(),0,DXGI_FORMAT_R8G8B8A8_UNORM);
        context->RSSetState(sprite_solid.Get());
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(backdrop_vs.Get(),nullptr,0);context->PSSetShader(eye_display_ps.Get(),nullptr,0);
        context->PSSetShaderResources(0,1,surface.view.GetAddressOf());
        context->Draw(3,0);
        ID3D11ShaderResourceView* empty=nullptr;context->PSSetShaderResources(0,1,&empty);
    }
private:
    void render_scene(const SceneTarget& target,const SceneEye* eye,std::span<const std::uint32_t> demo_frame,int split=-1) {
        const auto& [width,height,color,rtv,dsv,depth_srv]=target;
        const unsigned sky=world_palette[0];
        float background[]{controls.world?float(sky&31)/31:.035f,
            controls.world?float((sky>>5)&31)/31:.055f,controls.world?float((sky>>10)&31)/31:.075f,1};
        if(headset_output && !enhanced_scene)for(int i=0;i<3;++i){
            auto& c=background[i];c=c<=.04045f?c/12.92f:std::pow((c+.055f)/1.055f,2.4f);
        }
        if(split!=1)context->ClearRenderTargetView(rtv.Get(),background);
        context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        context->OMSetDepthStencilState(reverse_depth.Get(),0);
        const auto top=split==1?height/2:0u;
        const auto view_height=split<0?height:split==1?height-top:height/2;
        const float panel_scale=!eye && bonus_has_dashboard?std::min(width/256.f,view_height/168.f):0.f;
        const float play_height=std::max(1.f,float(view_height)-std::round(panel_scale*40));
        D3D11_VIEWPORT viewport{0,float(top),float(width),play_height,0,1};
        float aspect=float(width)/std::max(1.f,viewport.Height);
        if(low_raster && !n64_mode && !eye)aspect*=7.f/6.f;
        const bool native_aperture=low_raster && !n64_mode && !demo_frame.empty();
        if(!demo_frame.empty()) {
            // Its 208x128 playfield sits inside the same 4:3 raster as the title.
            const float w=native_aperture?float(width):std::min(float(width),height*(4.f/3)),h=native_aperture?float(height):w*.75f;
            viewport={(width-w)*.5f+w*24/256,(height-h)*.5f+h*32/224,w*208/256,h*128/224,0,1};
            aspect=208.f/128;
            constexpr float black[]{0,0,0,1};
            context->ClearRenderTargetView(rtv.Get(),native_aperture?background:black);
        }
        Constants value{std::cos(controls.yaw),std::sin(controls.yaw),std::cos(controls.pitch),std::sin(controls.pitch),
            aspect,controls.zoom,controls.distance,0,controls.target_x,controls.target_y,controls.target_z,0};
        if(enhanced_scene) {value.shadow_origin=shadow_origin;value.shadow_axis=shadow_axis;value.sun=sun;}
        if(rendered_camera()==RaceCamera::isometric) {
            value.isometric=value.cutaway=1;
            value.car_centers=car_centers;
        }
        const bool underwater=(aqua_style && (environment.alternate_backdrop || environment.blank_backdrop)) ||
                              ((lake_style || harbor_style) && environment.fog_weight>0);
        if(atmosphere) {
            value.sun[3]=float(world_radius/90); // metres per normalized world unit, including the actor pass
            auto local_weather=weather;
            if(ravine_style) {
                // The original grey sky clears through the final-lap palette blend.
                const float storm=1.f-environment.palette_weight/31.f;
                local_weather.clouds=std::max(local_weather.clouds,.9f*storm);
                if(ravine_wet) {
                    local_weather.rain=1;
                    local_weather.extinction=std::max(local_weather.extinction,.007f);
                }
            }
            if(underwater)
                local_weather={.25f,0,0,weather.seconds};
            if(trace_indirect && local_weather.clouds!=probe_cloud_cover) {
                probe_cloud_cover=local_weather.clouds;
                probe_dirty=true;
            }
            context->UpdateSubresource(atmosphere.Get(),0,nullptr,&local_weather,0,0);
            context->PSSetConstantBuffers(2,1,atmosphere.GetAddressOf());
        }
        value.probe_step=course_probe_step;
        if(eye)value.set_eye(*eye);
        context->UpdateSubresource(constants.Get(),0,nullptr,&value,0,0);
        context->VSSetConstantBuffers(0,1,constants.GetAddressOf());
        context->PSSetConstantBuffers(0,1,constants.GetAddressOf());
        const bool shared_lighting=!eye || !eye->second;
        if(car_shadow_depth && shared_lighting) {
            ID3D11ShaderResourceView* unbound=nullptr;
            context->PSSetShaderResources(8,1,&unbound);
            context->OMSetRenderTargets(0,nullptr,car_shadow_depth.Get());
            context->ClearDepthStencilView(car_shadow_depth.Get(),D3D11_CLEAR_DEPTH,0,0);
            D3D11_VIEWPORT shadow_viewport{0,0,float(car_shadow_size),float(car_shadow_size),0,1};
            context->RSSetViewports(1,&shadow_viewport);
            context->RSSetState(sprite_solid.Get()); // open and mirrored car panels cast on both sides
            context->IASetInputLayout(layout.Get());
            const UINT stride=sizeof(GpuVertex),offset=0;
            context->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(car_shadow_vs.Get(),nullptr,0);
            context->PSSetShader(nullptr,nullptr,0);
            context->Draw(car_shadow_vertices,surface_vertex_count);
            if(course_shadow_depth && course_shadow_dirty && sun[1]>.01f) {
                context->PSSetShaderResources(9,1,&unbound);
                context->OMSetRenderTargets(0,nullptr,course_shadow_depth.Get());
                context->ClearDepthStencilView(course_shadow_depth.Get(),D3D11_CLEAR_DEPTH,0,0);
                D3D11_VIEWPORT course_viewport{0,0,float(course_shadow_size),float(course_shadow_size),0,1};
                context->RSSetViewports(1,&course_viewport);
                context->VSSetShader(course_shadow_vs.Get(),nullptr,0);
                context->PSSetShader(course_shadow_ps.Get(),nullptr,0);
                context->PSSetShaderResources(0,1,texture_view.GetAddressOf());
                context->PSSetSamplers(0,1,texture_sampler.GetAddressOf());
                context->Draw(course_shadow_vertices-cloud_vertices,cloud_vertices);
                if(landscape_buffer) {
                    context->IASetVertexBuffers(0,1,landscape_buffer.GetAddressOf(),&stride,&offset);
                    context->Draw(landscape_vertex_count,0);
                }
                course_shadow_dirty=false;
            }
        }
        context->OMSetRenderTargets(1,rtv.GetAddressOf(),dsv.Get());
        context->RSSetViewports(1,&viewport); context->RSSetState(controls.wire?wire.Get():solid.Get());
        if(trace_course()) {
            ID3D11ShaderResourceView* geometry[]{ray_nodes.Get(),ray_faces.Get(),actor_nodes.current(),actor_faces.current()};
            context->PSSetShaderResources(10,trace_reflections?4:2,geometry);
            if(trace_indirect && shared_lighting && probe_dirty) {
                ID3D11ShaderResourceView* empty=nullptr;
                context->PSSetShaderResources(14,1,&empty);
                context->CSSetShader(probe_shader.Get(),nullptr,0);
                context->CSSetConstantBuffers(0,1,constants.GetAddressOf());
                context->CSSetConstantBuffers(2,1,atmosphere.GetAddressOf());
                context->CSSetShaderResources(10,2,geometry);
                context->CSSetShaderResources(0,1,texture_view.GetAddressOf());
                if(course_road_detail)context->CSSetShaderResources(2,1,backdrop_colors.GetAddressOf());
                context->CSSetSamplers(0,1,texture_sampler.GetAddressOf());
                context->CSSetUnorderedAccessViews(0,1,probe_output.GetAddressOf(),nullptr);
                context->Dispatch(probe_width/4,probe_height/4,probe_width/4);
                probe_dirty=false;
                ID3D11UnorderedAccessView* no_output=nullptr;
                context->CSSetUnorderedAccessViews(0,1,&no_output,nullptr);
            }
            if(trace_indirect) {
                context->PSSetShaderResources(14,1,probe_view.GetAddressOf());
                context->PSSetSamplers(2,1,probe_sampler.GetAddressOf());
            }
        }
        if(car_shadow_view) {
            context->PSSetShaderResources(8,1,car_shadow_view.GetAddressOf());
            context->PSSetShaderResources(9,1,course_shadow_view.GetAddressOf());
            context->PSSetSamplers(1,1,car_shadow_sampler.GetAddressOf());
        }
        BackdropConstants surroundings{};
        if(night_style) {
            const float blend=environment.palette_weight/31.f;
            surroundings.night_lighting=harbor_style ? 1.f-blend : cruise_style ? blend : 1.f;
            // Keep low beams on across the early bonus gates, then reach full strength at night.
            if(cruise_style && course_road_detail)surroundings.night_lighting=std::lerp(.30f,1.f,blend);
        }
        if(stunt_style || forest_style || sea_style || white_land_style || lake_style || ravine_style || sky_ramp_style || harbor_style) {
            surroundings.panorama_fog=environment.fog_weight/32.f;
            const auto fog=environment.fog_rgb555;
            surroundings.panorama_fog_color={float(fog&31)/31,float((fog>>5)&31)/31,float((fog>>10)&31)/31};
            surroundings.harbor_windows=harbor_style && high_detail_scenery;
        }
        if(sunset_style) {
            surroundings.sunset_valley=1;
            surroundings.sunset_dusk=environment.palette_weight/31.f;
        }
        if(aqua_style) {
            surroundings.aqua_meters=float(world_radius/90);
            surroundings.aqua_plane_y=normalize_world({0,environment.alternate_backdrop?1280.f:-1280.f,0})[1];
        }
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
        if(backdrop.available() || enhanced_scene) {
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
        if(enhanced_scene)context->PSSetShaderResources(7,1,material_tiles.GetAddressOf());
        if(foliage_layer)context->PSSetShaderResources(15,1,foliage_mips.GetAddressOf());
        if(asphalt_sampler)context->PSSetSamplers(3,1,asphalt_sampler.GetAddressOf());
        context->PSSetSamplers(0,1,texture_sampler.GetAddressOf());
        context->IASetInputLayout(layout.Get());
        const UINT stride=sizeof(GpuVertex),offset=0;
        context->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(cloud_vertices) {
            context->Draw(cloud_vertices,0);
            context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
        }
        const auto draw_landscape=[&] {
            if(!landscape_buffer)return;
            context->IASetVertexBuffers(0,1,landscape_buffer.GetAddressOf(),&stride,&offset);
            context->RSSetState(controls.wire?wire.Get():sprite_solid.Get());
            context->Draw(landscape_vertex_count,0);
            context->RSSetState(controls.wire?wire.Get():solid.Get());
            context->IASetVertexBuffers(0,1,vertices.GetAddressOf(),&stride,&offset);
        };
        const auto draw_world=[&] {
            draw_landscape();
            context->Draw(two_sided_world_start-cloud_vertices,cloud_vertices);
            if(world_vertex_count>two_sided_world_start) {
                context->RSSetState(controls.wire?wire.Get():sprite_solid.Get());
                context->Draw(world_vertex_count-two_sided_world_start,two_sided_world_start);
                context->RSSetState(controls.wire?wire.Get():solid.Get());
            }
            if(world_line_count) {
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                context->Draw(world_line_count,vertex_count);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }
        };
        if(vehicle_depth_pass && value.isometric) {
            // Clip scenery before drawing any racer, so all four keep solid
            // silhouettes and the track beneath them still depth-tests normally.
            draw_landscape();
            context->Draw(rival_start-cloud_vertices,cloud_vertices);
            context->Draw(surface_vertex_count-world_vertex_count,world_vertex_count);
            if(world_line_count) {
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
                context->Draw(world_line_count,vertex_count);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            }
            value.cutaway=0;
            value.shadow_origin[3]=0;
            context->UpdateSubresource(constants.Get(),0,nullptr,&value,0,0);
            context->Draw(two_sided_world_start-rival_start,rival_start);
            context->RSSetState(controls.wire?wire.Get():sprite_solid.Get());
            context->Draw(world_vertex_count-two_sided_world_start,two_sided_world_start);
        } else if(vehicle_depth_pass) {
            draw_world();
            // Preserve world color and its geometric self-occlusion, then rebuild
            // the actor occluders with the original road priority. No color or
            // geometry moves, and actual wall/car depth testing stays enabled.
            context->ClearDepthStencilView(dsv.Get(),D3D11_CLEAR_DEPTH,0,0);
            context->OMSetBlendState(depth_only.Get(),nullptr,UINT_MAX);
            value.road_pass=1;
            context->UpdateSubresource(constants.Get(),0,nullptr,&value,0,0);
            draw_world();
            context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
            value.road_pass=0;
            value.shadow_origin[3]=0;
            context->UpdateSubresource(constants.Get(),0,nullptr,&value,0,0);
            context->Draw(surface_vertex_count-world_vertex_count,world_vertex_count);
        } else {
            draw_landscape();
            context->Draw(surface_vertex_count-cloud_vertices,cloud_vertices);
        }
        context->RSSetState(controls.wire?wire.Get():sprite_solid.Get());
        const UINT visible_start=surface_vertex_count+(rendered_camera()==RaceCamera::cockpit?car_shadow_vertices:0);
        if(vertex_count>visible_start)context->Draw(vertex_count-visible_start,visible_start);
        if(vehicle_depth_pass && line_count>world_line_count) {
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            context->Draw(line_count-world_line_count,vertex_count+world_line_count);
        }
        if(line_count && !vehicle_depth_pass) { context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST); context->Draw(line_count,vertex_count); }
        if(spray_count) {
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetVertexBuffers(0,1,spray_vertices.GetAddressOf(),&stride,&offset);
            context->PSSetShader(spray_shader.Get(),nullptr,0);
            context->OMSetDepthStencilState(spray_depth.Get(),0);
            composite_overlay();
            context->Draw(spray_count,0);
            context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
            context->OMSetDepthStencilState(reverse_depth.Get(),0);
        }
        if(volume_integrate && depth_srv && car_shadow_vertices && !grid_sweep && split<0) {
            float density=0;
            if(night_style && !cruise_style && !harbor_style)
                density=weather.rain>0?.009f:weather.extinction>=.02f?.023f:0.f;
            if(forest_style && sun[1]>.02f && weather.rain==0) {
                const float fog=std::max(weather.extinction>=.02f?1.f:0.f,environment.fog_weight/15.f);
                density=.007f*fog*std::clamp((.72f-sun[1])*3,0.f,1.f);
            }
            if(density>0)draw_volume(target,viewport,density);
        }
        if(eye)return; // Screen-space rain and the dashboard belong to the desktop.
        if(weather_rain_ps && !underwater && (weather.rain>0 || ravine_wet)) {
            context->OMSetRenderTargets(1,rtv.GetAddressOf(),nullptr);
            context->RSSetState(sprite_solid.Get());
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(backdrop_vs.Get(),nullptr,0);context->PSSetShader(weather_rain_ps.Get(),nullptr,0);
            composite_overlay();context->Draw(3,0);
            context->OMSetBlendState(nullptr,nullptr,UINT_MAX);
        }
        if(show_rain && (!ravine_style || n64_mode)) {
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
            const D3D11_VIEWPORT full=native_aperture?viewport:D3D11_VIEWPORT{0,float(top),float(width),float(view_height),0,1};
            context->RSSetViewports(1,&full);
            const float scale=std::min(width/208.f,play_height/128.f);
            std::array<float,12> placement{scale,(width-208.f*scale)*.5f,
                bonus_brightness/15.f,play_height,panel_scale,bonus_scene_weight,(width-256.f*panel_scale)*.5f,bonus_corner_rows,
                float(bonus_clip_top),float(bonus_clip_bottom),0.f,float(top)};
            if(native_aperture) {
                placement[0]=1;placement[1]=0;placement[3]=128;placement[4]=0;
                placement[10]=24;placement[11]=32;
            }
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
        sync_size();
        const auto& [width,height,color,rtv,dsv,depth_srv]=desktop;
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
        present(over_scene?std::span<const uint32_t>{}:pixels);
    }
    std::vector<BYTE> readback_bgra() {
        return readback_bgra(desktop);
    }
    std::vector<BYTE> readback_bgra(const SceneTarget& target,bool preserve_alpha=false) {
        const auto& [width,height,color,rtv,dsv,depth_srv]=target;
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
                pixels[p]=row[x*4+2]; pixels[p+1]=row[x*4+1]; pixels[p+2]=row[x*4]; pixels[p+3]=preserve_alpha?row[x*4+3]:255;
            }
        }
        context->Unmap(staging.Get(),0);
        return pixels;
    }
    void screenshot(const std::filesystem::path& path) {
        const auto width=desktop.width,height=desktop.height;
        auto pixels=readback_bgra();
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
using RoadTriangle=std::array<std::array<double,3>,3>;
struct RoadSample {float y,dx,dz;};
std::optional<RoadSample> road_at(std::span<const RoadTriangle> road,float x,float z,float reference_y) {
    std::optional<RoadSample> nearest;
    for(const auto& [a,b,c]:road) {
        if(x<std::min({a[0],b[0],c[0]}) || x>std::max({a[0],b[0],c[0]}) ||
           z<std::min({a[2],b[2],c[2]}) || z>std::max({a[2],b[2],c[2]}))continue;
        const double ax=a[0]-c[0],az=a[2]-c[2],bx=b[0]-c[0],bz=b[2]-c[2];
        const double det=ax*bz-bx*az;
        if(std::abs(det)<1.e-6)continue;
        const double u=((x-c[0])*bz-(z-c[2])*bx)/det,v=(ax*(z-c[2])-az*(x-c[0]))/det;
        if(u<0 || v<0 || u+v>1)continue;
        const double ay=a[1]-c[1],by=b[1]-c[1];
        const float y=float(c[1]+u*ay+v*by);
        // The road beneath the bridge and the deck can cover the same X/Z.
        if(!nearest || std::abs(y-reference_y)<std::abs(nearest->y-reference_y))
            nearest=RoadSample{y,float((ay*bz-by*az)/det),float((ax*by-bx*ay)/det)};
    }
    return nearest;
}
struct RivalWheelTravel {
    std::array<float,4> previous{},current{};
    void advance(const std::array<srf::Vehicle,4>& before,const std::array<srf::Vehicle,4>& after) {
        previous=current;
        for(unsigned i=0;i<srf::NativeRace::player_index();++i) {
            const auto& a=before[i].body;const auto& b=after[i].body;
            const float yaw=lerp_angle(a.word(0x24),b.word(0x24),.5f);
            const int dx=std::int16_t(b.word(0x1e)-a.word(0x1e)),dz=std::int16_t(b.word(0x22)-a.word(0x22));
            current[i]+=dz*std::cos(yaw)-dx*std::sin(yaw);
        }
    }
};
RenderVehicle vehicle_pose(const srf::Rom& rom,const srf::Vehicle& before,const srf::Vehicle& after,
                           bool player,float blend,std::uint8_t camera_byte,bool exterior_eyes=true,
                           std::optional<float> wheel_travel=std::nullopt,std::span<const RoadTriangle> road={},bool n64_rival=false) {
    RenderVehicle out;
    out.player=player;
    const auto& a=before.body;
    const auto& b=after.body;
    const auto definition=0x48000+(b.word(0x66)&0x7fff);
    out.descriptor=rom.u16(definition+(player?0:2));
    for(unsigned axis=0;axis<3;++axis) {
        const auto old=signed_word(a.word(0x1e + axis*2));
        const auto delta=signed_word(static_cast<std::uint16_t>(b.word(0x1e + axis*2)-a.word(0x1e + axis*2)));
        out.position[axis]=float(old)+float(delta)*blend;
    }
    out.heading=lerp_angle(a.word(0x24),b.word(0x24),blend);
    const auto yaw=-out.heading;
    float pitch=-lerp_angle(a.word(0x26),b.word(0x26),blend),roll=-lerp_angle(a.word(0x28),b.word(0x28),blend);
    const auto sy=std::sin(yaw),cy=std::cos(yaw);
    if(wheel_travel) {
        out.route_follow=std::lerp(float(bool(a.word(0x16)&0x400)),float(bool(b.word(0x16)&0x400)),blend);
        if(out.route_follow>0) {
            // The route controller moves between markers without following the
            // intervening crest. Sample both axle lines to span the summit smoothly.
            auto z=[](const auto& w){return signed_word(w.word(0x40));};
            auto [rear,front]=std::minmax_element(after.wheels.begin(),after.wheels.end(),
                [&](const auto& l,const auto& r){return z(l)<z(r);});
            const auto axle=[&](float offset){return road_at(road,out.position[0]+sy*offset,
                out.position[2]+cy*offset,out.position[1]+160);};
            const auto back=axle(float(z(*rear))),nose=axle(float(z(*front)));
            if(back && nose && z(*front)>z(*rear)) {
                const float grade=std::atan2(back->y-nose->y,float(z(*front)-z(*rear)));
                const float bank=std::atan(.5f*((back->dx+nose->dx)*cy-(back->dz+nose->dz)*sy)*std::cos(grade));
                pitch+=out.route_follow*std::remainder(grade-pitch,6.2831853f);
                roll+=out.route_follow*std::remainder(bank-roll,6.2831853f);
            }
        }
    }
    const auto sp=std::sin(pitch),cp=std::cos(pitch),sr=std::sin(roll),cr=std::cos(roll);
    out.orientation={sy*sr*sp+cy*cr,sr*cp,cy*sr*sp-sy*cr,
                     sy*cr*sp-cy*sr,cr*cp,cy*cr*sp+sy*sr,sy*cp,-sp,cy*cp};
    const bool distant=(b.word(0x16)&0x400)!=0,was_distant=(a.word(0x16)&0x400)!=0;
    const bool mounted_sprite=n64_rival && (was_distant || distant);
    for(unsigned wheel_index=0;wheel_index<4;++wheel_index) {
        const auto& wheel=after.wheels[wheel_index];
        // CPU08:BD32..BD54 hides far-controller wheel sprites. Their
        // world positions collapse to the chassis and wheel angles go stale.
        if(distant && !wheel_travel && !mounted_sprite)continue;
        const auto& prior=before.wheels[wheel_index];
        const auto& old=was_distant?wheel:prior;
        if(wheel.word(0x46)&0x200) continue; // Original08:BA70 skips hidden wheel definitions.
        const auto data=0x48000+wheel.word(0x12);
        const auto diameter_addition=rom.u16(data+4);
        const auto sprite_descriptor=rom.u16(data);
        const auto sprite_base=std::size_t(sprite_descriptor&0x7fff);
        const float diameter=float(srf::original_sprite_diameter(rom.u16(sprite_base+16),rom.bytes()[sprite_base+7],std::uint8_t(diameter_addition)));
        RenderSprite sprite;
        sprite.wheel=true;
        sprite.diameter=diameter;
        if(!wheel_travel) {
            // Distant N64 sprites face with the chassis until steering resumes.
            const auto& from=n64_rival && was_distant?a:old;
            const auto& to=n64_rival && distant?b:wheel;
            const auto wheel_yaw=srf::interpolated_angle_byte(from.word(0x24),to.word(0x24),blend);
            const auto relative=std::uint8_t(wheel_yaw-camera_byte);
            const auto roll=srf::interpolated_angle_byte(from.word(0x28),to.word(0x28),blend);
            sprite.table=rom.u16(data+2)+8+srf::wheel_sprite_bank(roll,relative);
            sprite.phase=rom.bytes()[0x44056+(relative>>2)];
        }
        if(wheel_travel || mounted_sprite) {
            sprite.position=out.position;
            for(unsigned axis=0;axis<3;++axis) {
                float mount=0;
                for(unsigned local=0;local<3;++local)
                    mount+=signed_word(wheel.word(0x3c+local*2))*out.orientation[local*3+axis];
                const auto pos=0x1e + axis*2;
                if(mounted_sprite && was_distant!=distant) {
                    // Blend to the live attachment at the near end of the handoff.
                    float from=was_distant?mount:signed_word(std::uint16_t(prior.word(pos)-a.word(pos)));
                    float to=distant?mount:signed_word(std::uint16_t(wheel.word(pos)-b.word(pos)));
                    mount=std::lerp(from,to,blend);
                }
                const auto spring=0x4a+axis*2;
                sprite.position[axis]+=mount+std::lerp(float(signed_word(prior.word(spring))),float(signed_word(wheel.word(spring))),blend);
            }
            sprite.position[1]-=float(diameter_addition+20);
            if(wheel_travel) {
                const auto steer=[](const auto& body,const auto& wheel) -> std::uint16_t {
                    return (body.word(0x16)&0x400)?0:std::uint16_t(body.word(0x24)-wheel.word(0x24));
                };
                sprite.steer=lerp_angle(steer(a,prior),steer(b,wheel),blend);
                sprite.spin=-*wheel_travel/n64::tire_radius(sprite);
            }
            out.sprites.push_back(sprite);
            continue;
        }
        sprite.steer=-lerp_angle(old.word(0x24),wheel.word(0x24),blend)-yaw;
        sprite.spin=lerp_angle(old.word(0x26),wheel.word(0x26),blend);
        // Original08:BF53..BF8A adds contact displacement, then raises
        // the center by the definition radius plus20 in original Y-down.
        for(unsigned axis=0;axis<3;++axis) {
            const auto value=[&](const srf::VehicleState& state) {
                return std::uint16_t(state.word(0x1e + axis*2)+state.word(0x4a+axis*2));
            };
            const auto start=value(old),end=value(wheel);
            sprite.position[axis]=float(signed_word(start))+float(signed_word(std::uint16_t(end-start)))*blend;
            if(was_distant)sprite.position[axis]+=out.position[axis]-float(signed_word(b.word(0x1e + axis*2)));
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
    std::size_t player,float blend,bool show_player_eyes=true,const RivalWheelTravel* travel=nullptr,std::span<const RoadTriangle> road={},bool n64_rivals=false,
    std::optional<std::uint8_t> view_angle=std::nullopt,const std::array<srf::Vehicle,4>* midpoint=nullptr,bool native_motion=false) {
    const auto camera_byte=view_angle.value_or(srf::interpolated_angle_byte(before[player].body.word(0x24),after[player].body.word(0x24),blend));
    std::array<RenderVehicle,4> result{};
    for(unsigned i=0;i<result.size();++i) {
        std::optional<float> distance;
        if(travel && i!=player)distance=std::lerp(travel->previous[i],travel->current[i],blend);
        const bool paired=midpoint && (before[i].body.word(0x1c)&8);
        const auto& from=paired && blend>=.5f?(*midpoint)[i]:before[i];
        const auto& to=paired && blend<.5f?(*midpoint)[i]:after[i];
        const float t=paired?(blend<.5f?2*blend:2*blend-1):(native_motion?std::floor(blend):blend);
        result[i]=vehicle_pose(rom,from,to,i==player,t,camera_byte,i!=player || show_player_eyes,distance,road,n64_rivals && i!=player);
    }
    return result;
}
struct PresentationRoad {
    struct Deck {
        unsigned command;
        std::size_t first;
        std::vector<std::vector<RoadTriangle>> frames;
    };
    std::vector<RoadTriangle> triangles;
    std::vector<Deck> decks;

    std::span<const RoadTriangle> sample(std::span<const srf::SceneryPose> bridges) {
        for(const auto& deck:decks) {
            auto pose=std::ranges::find(bridges,deck.command,&srf::SceneryPose::command_offset);
            const auto& frame=deck.frames.at(pose==bridges.end()?0:pose->frame);
            std::copy(frame.begin(),frame.end(),triangles.begin()+deck.first);
        }
        return triangles;
    }
};
bool rival_driving_surface(srf::CourseId course,const srf::WorldInstance& instance,unsigned material) {
    switch(course) {
    case srf::CourseId::big_ravine:
        if(instance.visual_descriptor==0xe368)return material==0xc9; // moving plank tops
        return n64::graded_road(instance.opcode,material);
    case srf::CourseId::harbor_city:
        if(material==0xdf)return true; // submerged roadbed
        [[fallthrough]];
    case srf::CourseId::sea_breeze:
        return n64::sea_breeze_road(instance.opcode,material);
    case srf::CourseId::kings_forest:
        return n64::forest_bridge_surface(instance.visual_descriptor,material) || n64::graded_road(instance.opcode,material);
    case srf::CourseId::lake_side:
        if(instance.visual_descriptor==0xc404)return material==0xd7; // textured deck tops
        [[fallthrough]];
    case srf::CourseId::sky_ramp:
    case srf::CourseId::white_land:
    case srf::CourseId::night_cruise:
    case srf::CourseId::night_owl:
    case srf::CourseId::sunset_valley:
        return n64::graded_road(instance.opcode,material);
    case srf::CourseId::aqua_tunnel:
        return n64::aqua_driving_surface(instance.visual_descriptor,instance.opcode,material);
    default:
        return n64::asphalt_material(instance.visual_descriptor,material);
    }
}
PresentationRoad presentation_surfaces(const srf::World& world,srf::CourseId course,bool seat_rivals) {
    PresentationRoad road;
    const bool road_course=course==srf::CourseId::sky_ramp || course==srf::CourseId::harbor_city;
    if(course!=srf::CourseId::lake_side && course!=srf::CourseId::big_ravine && !road_course && !seat_rivals)return road;
    for(const auto& instance:world.instances) {
        // Lake Side uses its banked descent. Big Ravine's two D54C hairpins
        // can put the eye behind the inner cliff while the car is on the road.
        if(!seat_rivals && course==srf::CourseId::lake_side && instance.visual_descriptor!=0xf1d8 && instance.visual_descriptor!=0xf1bc)continue;
        if(!seat_rivals && course==srf::CourseId::big_ravine && instance.visual_descriptor!=0xd54c)continue;
        if(road_course && instance.opcode!=2)continue;
        const auto model=std::find_if(world.models.begin(),world.models.end(),[&](const auto& m) {
            return m.descriptor_address==instance.visual_descriptor;
        });
        if(model==world.models.end())continue;
        const bool moving=(course==srf::CourseId::big_ravine && instance.visual_descriptor==0xe368) ||
                          (course==srf::CourseId::kings_forest && instance.visual_descriptor==0xfa0c) ||
                          (course==srf::CourseId::lake_side && instance.visual_descriptor==0xc404);
        if(moving)road.decks.push_back({instance.command_offset,road.triangles.size(),
                                        std::vector<std::vector<RoadTriangle>>(model->frames.size())});
        for(const auto& face:model->primitives) {
            const auto material=face.attributes[2];
            if(seat_rivals && !rival_driving_surface(course,instance,material))continue;
            if(!seat_rivals && course==srf::CourseId::lake_side && material!=44 && material!=45)continue;
            if(!seat_rivals && course==srf::CourseId::big_ravine && material!=0x5b && material!=0x5c)continue;
            if(course==srf::CourseId::sky_ramp && (material<43 || material>45))continue;
            // Include both outer lanes of the banks and the submerged roadbed.
            if(course==srf::CourseId::harbor_city && (material<0x29 || material>0x2f) && material!=0xdf)continue;
            const auto append=[&](std::vector<RoadTriangle>& triangles,const std::vector<srf::Vertex>& vertices) {
                const auto point=[&](std::size_t i) {return srf::world_position(vertices.at(face.indices[i]),instance);};
                for(std::size_t i=2;i<face.indices.size();++i)triangles.push_back({point(0),point(i-1),point(i)});
            };
            append(road.triangles,model->frames.front());
            if(moving)for(std::size_t i=0;i<model->frames.size();++i)
                append(road.decks.back().frames[i],model->frames[i]);
        }
    }
    return road;
}

// The simulated tire contact includes the original sprite's vertical overlap.
// Nearby suspension can leave the road. Route-driven rivals use the actual
// road height in place of the route markers' fixed 160-unit body offset.
void seat_opponents(std::span<RenderVehicle> cars,std::span<const RoadTriangle> road) {
    for(auto& car:cars) {
        if(car.player)continue;
        std::optional<float> lift;
        for(const auto& wheel:car.sprites)if(wheel.wheel) {
            const auto [x,y,z]=wheel.position;
            const float radius=n64::tire_radius(wheel);
            if(const auto deck=road_at(road,x,z,y+radius)) {
                const float depth=y+radius-deck->y;
                const float suspension=depth<2*radius?std::max(0.f,depth):0.f;
                const float correction=std::lerp(suspension,depth,car.route_follow);
                lift=std::max(lift.value_or(correction),correction);
            }
        }
        if(!lift)continue;
        car.position[1]-=*lift;
        for(auto& sprite:car.sprites)sprite.position[1]-=*lift;
    }
}
// Slide the eye along the ravine wall. Keeping the target on the car lets it
// remain visible even when driving parallel to a cliff at a tight bend.
void clear_cliff_camera(const Renderer& renderer,const RenderVehicle& vehicle,float yaw,std::span<const RoadTriangle> cliffs,float distance=900.f) {
    const float behind=distance*std::cos(controls.pitch),lift=-distance*std::sin(controls.pitch);
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
    float distance=900.f;
    controls.distance=renderer.normalized_distance(distance);
    if(race_camera.enabled) {
        const bool far_chase=race_camera.view==RaceCamera::far_chase;
        controls.yaw+=race_camera.heading;
        if(race_camera.mounted()) {
            const bool cockpit=race_camera.view==RaceCamera::cockpit;
            const bool coupe=vehicle.descriptor==n64::coupe_player_shell;
            auto eye=cockpit ? (coupe?std::array{-12.f,-89.f,-41.f}:std::array{-18.f,-85.f,-23.f})
                                   : (coupe?std::array{0.f,-72.f,41.f}:std::array{0.f,-81.f,66.f});
            if(vehicle.descriptor==n64::f_type_player_shell)
                eye=cockpit?std::array{0.f,-80.f,-2.f}:std::array{0.f,-63.f,65.f};
            auto mount=vehicle.position;
            for(unsigned axis=0;axis<3;++axis)for(unsigned local=0;local<3;++local)
                mount[axis]+=eye[local]*vehicle.orientation[local*3+axis];
            const auto seat=renderer.normalize_world(mount);
            controls.target_x=seat[0];controls.target_y=seat[1];controls.target_z=seat[2];
            controls.pitch=std::asin(std::clamp(-vehicle.orientation[7],-1.f,1.f))-.10f+race_camera.tilt;
            controls.distance=0;
            controls.zoom=(cockpit?1.65f:1.35f)*race_camera.magnification;
        } else {
            controls.pitch=(far_chase?-.25f:-.18f)+race_camera.tilt;
            distance=race_camera.starting_distance()/race_camera.magnification;
            controls.distance=renderer.normalized_distance(distance);
            if(course==srf::CourseId::big_ravine && race_camera.view!=RaceCamera::isometric)
                clear_cliff_camera(renderer,vehicle,controls.yaw,road,distance);
            if(race_camera.view==RaceCamera::isometric) {
                controls.yaw=.78539816f;
                controls.pitch=-.61547971f; // equal foreshortening of the three world axes
            }
        }
        // Chase views need the same ramp clearance at the selected orbit and zoom.
        if((course!=srf::CourseId::sky_ramp && course!=srf::CourseId::harbor_city) || race_camera.mounted() || race_camera.view==RaceCamera::isometric)return;
    }
    if(road.empty())return;
    if(course==srf::CourseId::big_ravine){clear_cliff_camera(renderer,vehicle,yaw,road);return;}
    const float behind=distance*std::cos(controls.pitch);
    const float x=vehicle.position[0]+behind*std::sin(controls.yaw),z=vehicle.position[2]-behind*std::cos(controls.yaw);
    const float target_y=vehicle.position[1]-30.f;
    const float original_lift=-distance*std::sin(controls.pitch);
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
    const auto award=race.time_award();
    const std::wstring signal=held_on_grid || race.countdown_active()?L"START LIGHTS  -  HOLD":
        race.time_over_pending()?L"TIME OVER":award?
        std::wstring(award->checkpoint?L"CHECKPOINT  +":L"LAP EXTEND  +")+std::to_wstring(award->seconds)+L" s":L"GO";
    text<<L"  "<<std::wstring(name.begin(),name.end())<<L" — NATIVE RACE    "<<simulation_hz<<L" Hz prototype pace\r\n"
        <<L"  Lap "<<std::min(3u,unsigned(body.word(0x7a))+1)<<L" / 3    Position "<<race.standings()[race.player_index()]<<L" / 4"
        <<L"    Time left "<<grid_seconds.value_or(race.remaining_seconds())<<L" s"
        <<L"    Time "<<race.elapsed_centiseconds()/6000<<L":"<<std::setfill(L'0')<<std::setw(2)<<(race.elapsed_centiseconds()/100)%60
        <<L"."<<std::setw(2)<<race.elapsed_centiseconds()%100<<std::setfill(L' ')
        <<L"\r\n  Speed (native) "<<signed_word(body.word(0x4a))<<L"    Boost "<<body.word(0x6a)
        <<L"    Health "<<std::max(0,512-int(body.word(0x82)))<<L" / 512"
        <<L"    "<<signal
        <<L"    "<<(race.completion().finished?L"FINISHED — R TO RESTART":controls.paused?L"PAUSED":L"")
        <<L"\r\n  W / UP accelerate   S / DOWN or X brake/reverse   A/D steer   SPACE boost   "
        <<(championship?L"R retire":L"R restart")<<L"   P pause   H status   F11 fullscreen   ESC back/exit";
    if(race.failure()==srf::RaceFailure::wrecked) text<<L"\r\n  WRECKED - R TO RESTART";
    if(race.failure()==srf::RaceFailure::time_out) text<<L"\r\n  TIME OUT - R TO RESTART";
    if(race.failure()==srf::RaceFailure::out_of_bounds) text<<L"\r\n  OUT OF COURSE - R TO RESTART";
    return text.str();
}
}

#include "boot_window_input.inc"
#include "game_host.inc"
#include "speed_bonus_window.inc"
#include "course_map_window.inc"
#include "grid_countdown_art.hpp"
#include "race_hud.inc"
#include "grid_countdown_view.inc"
#include "rock_field_window.inc"
#include "local_race_window.inc"
#include "lan_race_window.inc"
#include "boot_window.inc"
#include "bonnet_drive.inc"
#include "speed_finish_window.inc"

int wmain(int argc,wchar_t** argv) {
    bool launch_game=false,vr=false,native_vr_motion=false,n64_low_raster=false,classic_snes=false;
    RaceLook look=RaceLook::classic;
    std::optional<unsigned> shadow_rays;
    bool ray_shadows_enabled=false,ray_reflections_enabled=false,indirect_enabled=false,roughness_enabled=false,coat_enabled=false,road_detail_enabled=false,scenery_detail_enabled=false,wet_spray_enabled=false,volume_enabled=false;
    std::optional<unsigned> time_of_day,msaa;
    std::optional<RaceWeather> weather;
    std::optional<RaceCamera> camera;
    std::optional<unsigned> camera_max_distance;
    std::filesystem::path screenshot;
    try {
        std::filesystem::path assets,world_path,rom_path,trace_path,input_path,controller_path,progress_path;
        bool validate=false,restart_check=false,borderless=false,screenshot_no_hud=false,gpu_timing=false;
        std::vector<srf::PlayerCar> lan_grid;
        int lan_slot=-1;bool lan_drop_in=false,lan_capture_handoff=false;
        auto other_car=srf::PlayerCar::four_wd;
        auto player_car=srf::PlayerCar::four_wd;
        StuntReplay stunt_replay;
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
            else if(arg==L"--lan-screenshot-at-join")lan_capture_handoff=true;
            else if(arg==L"--lan-drop-in")lan_drop_in=true;
            else if(arg==L"--lan-host" || arg==L"--lan-guest")lan_slot=arg==L"--lan-host"?0:1;
            else if(arg==L"--lan-seat" && i+1<argc)lan_slot=std::stoi(argv[++i]);
            else if((arg==L"--peer-car" || arg==L"--grid-car") && i+1<argc){
                const std::wstring value=argv[++i];
                if(value==L"4wd")other_car=srf::PlayerCar::four_wd;
                else if(value==L"coupe")other_car=srf::PlayerCar::coupe;
                else if(value==L"f-type")other_car=srf::PlayerCar::f_type;
                else throw std::invalid_argument("Unknown peer car");
                if(arg==L"--grid-car")lan_grid.push_back(other_car);
            }
            else if(arg==L"--race") controls.race=true;
            else if(arg==L"--n64" || arg==L"--remastered") {
                const auto requested=arg==L"--n64"?RaceLook::n64:RaceLook::remastered;
                if(look!=RaceLook::classic && look!=requested)throw std::runtime_error("Choose one race look.");
                look=requested;
            }
            else if(arg==L"--camera" && i+1<argc) {
                const std::wstring view=argv[++i];
                if(view==L"chase")camera=RaceCamera::chase;
                else if(view==L"far-chase")camera=RaceCamera::far_chase;
                else if(view==L"cockpit")camera=RaceCamera::cockpit;
                else if(view==L"bonnet")camera=RaceCamera::bonnet;
                else if(view==L"isometric")camera=RaceCamera::isometric;
                else throw std::runtime_error("--camera requires chase, far-chase, cockpit, bonnet or isometric.");
            }
            else if(arg==L"--camera-max-distance" && i+1<argc) {
                const std::wstring meters=argv[++i];
                if(meters.empty() || meters.size()>3 || meters.find_first_not_of(L"0123456789")!=std::wstring::npos ||
                   std::stoul(meters)<40 || std::stoul(meters)>160)
                    throw std::runtime_error("--camera-max-distance requires 40 to 160 metres.");
                camera_max_distance=std::stoul(meters);
            }
            else if(arg==L"--camera-look")race_camera.look=true;
            else if(arg==L"--time-of-day" && i+1<argc) {
                const std::wstring hour=argv[++i];
                if(hour.empty() || hour.size()>2 || hour.find_first_not_of(L"0123456789")!=std::wstring::npos || std::stoul(hour)>23)
                    throw std::runtime_error("--time-of-day requires an hour from 0 to 23.");
                time_of_day=std::stoul(hour);
            }
            else if(arg==L"--weather" && i+1<argc) {
                const std::wstring preset=argv[++i];
                if(preset==L"clear")weather=RaceWeather{};
                else if(preset==L"partly-cloudy")weather=RaceWeather{.46f,.0008f};
                else if(preset==L"overcast")weather=RaceWeather{.90f,.0022f};
                else if(preset==L"fog")weather=RaceWeather{.80f,.023f};
                else if(preset==L"rain")weather=RaceWeather{1,.007f,1};
                else throw std::runtime_error("--weather requires clear, partly-cloudy, overcast, fog or rain.");
            }
            else if(arg==L"--gpu-timing")gpu_timing=true;
            else if(arg==L"--material-roughness") roughness_enabled=true;
            else if(arg==L"--volumetric-lighting") volume_enabled=true;
            else if(arg==L"--wet-tire-spray") wet_spray_enabled=true;
            else if(arg==L"--high-detail-scenery") scenery_detail_enabled=true;
            else if(arg==L"--high-detail-road") road_detail_enabled=true;
            else if(arg==L"--car-clear-coat") coat_enabled=true;
            else if(arg==L"--ray-traced-indirect") indirect_enabled=true;
            else if(arg==L"--ray-traced-reflections") ray_reflections_enabled=true;
            else if(arg==L"--ray-traced-shadows") ray_shadows_enabled=true;
            else if(arg==L"--shadow-rays" && i+1<argc) {
                const std::wstring level=argv[++i];
                if(level!=L"1" && level!=L"2" && level!=L"4")
                    throw std::runtime_error("--shadow-rays requires 1, 2 or 4.");
                shadow_rays=std::stoul(level);
            }
            else if(arg==L"--msaa" && i+1<argc) {
                const std::wstring level=argv[++i];
                if(level!=L"0" && level!=L"2" && level!=L"4" && level!=L"8")
                    throw std::runtime_error("--msaa requires 0, 2, 4 or 8.");
                msaa=std::stoul(level);
            }
            else if(arg==L"--classic-snes") classic_snes=true;
            else if(arg==L"--n64-240p") n64_low_raster=true;
            else if(arg==L"--vr") vr=true;
            else if(arg==L"--vr-native-motion")native_vr_motion=true;
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
            else if(arg==L"--hide-race-hud")race_hud_visible=false;
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
            else if(arg==L"--stunt-replay" && i+2<argc) {
                const auto id=std::filesystem::path(argv[++i]).string();
                const auto match=std::ranges::find(srf::playable_stunt_courses,id,srf::stunt_course_id);
                if(match==srf::playable_stunt_courses.end())throw std::invalid_argument("Unknown Stunt Trax replay course");
                stunt_replay.course=*match;
                stunt_replay.inputs=read_inputs(argv[++i]);
                if(!stunt_replay.inputs.front().requested ||
                   std::ranges::any_of(stunt_replay.inputs,[](auto row){return row.ticks!=0;}))
                    throw std::invalid_argument("Stunt replay requires requested buttons and zero clock ticks (the course owns its clock)");
            }
            else if(arg==L"--controller-config" && i+1<argc) controller_path=argv[++i];
            else if(arg==L"--progress-file" && i+1<argc) progress_path=argv[++i];
            else if(arg==L"--trace" && i+1<argc) trace_path=argv[++i];
            else if(arg==L"--yaw" && i+1<argc) controls.yaw=std::stof(argv[++i]);
            else if(arg==L"--pitch" && i+1<argc) controls.pitch=std::stof(argv[++i]);
            else if(arg==L"--zoom" && i+1<argc) controls.zoom=std::stof(argv[++i]);
            else if(arg==L"--width" && i+1<argc) controls.width=std::stoul(argv[++i]);
            else if(arg==L"--height" && i+1<argc) controls.height=std::stoul(argv[++i]);
            else if(arg==L"--validate-assets") validate=true;
            else throw std::runtime_error("Usage: srf_preview --assets <models.srfmesh> [--world <world.srfworld>] [--game | --race] [--remastered [--time-of-day 0..23] [--ray-traced-shadows [--shadow-rays 1|2|4]] [--ray-traced-reflections] [--ray-traced-indirect] [--material-roughness] [--car-clear-coat] [--high-detail-road] [--high-detail-scenery] [--wet-tire-spray] [--volumetric-lighting] [--msaa 0|2|4|8] [--weather preset] | --n64 [--n64-240p] [--classic-snes]] [--rom <original.sfc> --course easy-ride|aqua-tunnel|sunset-valley|night-owl|kings-forest|sea-breeze|white-land|night-cruise|lake-side|big-ravine|sky-ramp|harbor-city --car 4wd|coupe|f-type] [--borderless] [--validate-assets | --screenshot <new.png>] [--steps count --control native_word --trace new.jsonl] [--replay-input controls.csv] [--restart-check] [--simulation-hz 14] [--controller-config path] [--model index --descriptor address --yaw radians --pitch radians --zoom factor --width pixels --height pixels]");
        }
        race_camera.enabled=look==RaceLook::remastered;

        if((camera || camera_max_distance || race_camera.look) && !race_camera.enabled)throw std::runtime_error("Camera settings require --remastered.");
        if(camera_max_distance)race_camera.max_distance_m=*camera_max_distance;
        race_camera.view=vr?RaceCamera::bonnet:camera.value_or(RaceCamera::chase);
        if(vr)race_camera.look=false;
        if(gpu_timing && (screenshot.empty() || !controls.race || launch_game || vr || look!=RaceLook::remastered))throw std::runtime_error("--gpu-timing requires an offscreen --race --remastered --screenshot.");
        if(volume_enabled && look!=RaceLook::remastered)throw std::runtime_error("Volumetric lighting requires --remastered.");
        if(wet_spray_enabled && look!=RaceLook::remastered)throw std::runtime_error("Wet tire spray requires --remastered.");
        if(scenery_detail_enabled && look!=RaceLook::remastered)throw std::runtime_error("High-detail scenery requires --remastered.");
        if(road_detail_enabled && look!=RaceLook::remastered)throw std::runtime_error("High-detail roads require --remastered.");
        if((roughness_enabled || coat_enabled) && look!=RaceLook::remastered)throw std::runtime_error("Material roughness and car clear coat require --remastered.");
        if(indirect_enabled && look!=RaceLook::remastered)throw std::runtime_error("--ray-traced-indirect requires --remastered.");
        if(ray_reflections_enabled && look!=RaceLook::remastered)throw std::runtime_error("--ray-traced-reflections requires --remastered.");
        if(shadow_rays && !ray_shadows_enabled)throw std::runtime_error("--shadow-rays requires --ray-traced-shadows.");
        if(ray_shadows_enabled && look!=RaceLook::remastered)throw std::runtime_error("--ray-traced-shadows requires --remastered.");
        if((msaa || weather) && look!=RaceLook::remastered)throw std::runtime_error("--msaa and --weather require --remastered.");
        if(time_of_day && look!=RaceLook::remastered)throw std::runtime_error("--time-of-day requires --remastered.");
        if(classic_snes && (look!=RaceLook::classic || vr))throw std::runtime_error("--classic-snes requires desktop Classic.");
        if(n64_low_raster && look!=RaceLook::n64)throw std::runtime_error("--n64-240p requires --n64.");
        if(native_vr_motion && !vr)throw std::runtime_error("--vr-native-motion requires VR.");
        if(vr && n64_low_raster)throw std::runtime_error("Use --vr-scale to set headset resolution.");
        if(look!=RaceLook::classic && (!controls.race || validate))
            throw std::runtime_error("Enhanced styles require a race.");
        if(vr && (!controls.race || !screenshot.empty() || validate || steps || restart_check || !trace_path.empty() || fixed_control))
            throw std::runtime_error("VR requires interactive play on a supported course.");
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
        if(launch_game && (world_path.empty() || rom_path.empty() || descriptor || validate || steps || fixed_control || restart_check || (!screenshot.empty() && (!trace_path.empty() || !input_path.empty()))))
            throw std::runtime_error("Launch Game requires a world and ROM; bounded race captures require --race");
        const auto package_root=course==srf::CourseId::easy_ride?assets.parent_path():assets.parent_path().parent_path();
        if(lan_drop_in && lan_slot<1)throw std::invalid_argument("LAN catch-up requires a guest");
        if(lan_slot>=0 && (!controls.race || vr || launch_game || !srf::supports_lan_course(static_cast<unsigned>(course)) || rom_path.empty() || world_path.empty()))
            throw std::invalid_argument("LAN requires a supported desktop Quick Race course");
        const auto launch_controls=controls;
        if(stunt_replay.course && !launch_game)throw std::invalid_argument("--stunt-replay requires --game");
        stunt_replay.car=player_car;
        const RaceGraphics graphics{look,n64_low_raster || classic_snes,time_of_day.value_or(12),msaa.value_or(0),weather.value_or(RaceWeather{}),ray_shadows_enabled,ray_reflections_enabled,indirect_enabled,roughness_enabled,coat_enabled,road_detail_enabled,scenery_detail_enabled,shadow_rays.value_or(4),wet_spray_enabled,volume_enabled};
        struct ComScope {HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);~ComScope(){if(SUCCEEDED(hr))CoUninitialize();}} com;
        check(com.hr,"Game COM initialization");
        std::ofstream xr_log;
        std::optional<srf::vr::Drive> xr_session;
        std::optional<GameHost> host;
        if(screenshot.empty() && !validate && (launch_game || controls.race)){
            SetProcessDPIAware();
            if(vr){xr_log.open(L"openxr-last-drive.log");xr_session.emplace(xr_log,vr_refresh,vr_scale);game_xr=&*xr_session;}
            host.emplace();host->open(borderless,graphics);
        }
        const auto present_result=[&](const srf::GameResultPage& page){return game_result(*host,page);};
        auto driving_camera=race_camera;
        const auto replay_course=course;
        const auto replay_car=player_car;
        std::ofstream trace;
        if(progress_path.empty()) {
            const wchar_t* local=_wgetenv(L"LOCALAPPDATA");
            progress_path=(local?std::filesystem::path(local)/"StuntRaceFXPC":package_root)/"speed-trax.sav";
        }
        srf::ChampionshipProgress progress;
        if(host) {
            SetProcessDPIAware();
            try {progress=srf::ChampionshipProgress::load(progress_path);}
            catch(const std::exception& error) {
                MessageBoxA(nullptr,error.what(),"Speed Trax could not read your class records",MB_OK|MB_ICONWARNING);
            }
        }
        std::optional<srf::Championship> championship;
        std::unique_ptr<srf::AudioDevice> next_grid_audio;
        srf::GameSessionChoice selection;
        bool cold_boot=true,choose_quick=false;
        std::array<std::vector<srf::StuntRecord>,srf::playable_stunt_courses.size()> stunt_scores;
        if(host) {
            for(const auto course:srf::playable_stunt_courses) {
                try {stunt_scores[static_cast<unsigned>(course)]=srf::load_stunt_scores(stunt_scores_path(progress_path.parent_path(),course),course);}
                catch(const std::exception& error){MessageBoxA(nullptr,error.what(),"Stunt Trax could not read your records",MB_OK|MB_ICONWARNING);}
            }
        }
        const auto save_progress=[&] {
            try {progress.save(progress_path);}
            catch(const std::exception& error) {
                MessageBoxA(nullptr,error.what(),"Speed Trax could not save progress",MB_OK|MB_ICONWARNING);
            }
        };
        const auto complete_bonus=[&](const srf::Rom& rom,bool skip)->std::optional<srf::CourseMapEntry> {
            auto entry=srf::CourseMapEntry::skip;
            srf::BonusReward reward{};
            if(!skip) {
                auto finish=srf::play_speed_bonus(*host,rom,*championship,controller_path);
                if(!finish)return std::nullopt;
                entry=finish->entry;
                reward=finish->reward;
                next_grid_audio=std::move(finish->audio);
            }
            championship->finish_bonus(reward.extra_seconds,reward.extra_cars);
            return entry;
        };
        for(;;) {
        if(game_closing)return 0;
        if(choose_quick){
            choose_quick=false;
            if(!quick_board(*host,course,player_car)){
                if(game_closing)return 0;
                launch_game=true;cold_boot=false;
            }else{
                const auto directory=course==srf::CourseId::easy_ride?package_root:package_root/std::string(srf::course_definition(course).slug);
                assets=directory/"models.srfmesh";world_path=directory/"world.srfworld";
                controls=launch_controls;
            }
        }
        if(launch_game) {
            if(!championship) {
                race_camera={};
                const auto rom=srf::Rom::load(rom_path);
                if(!screenshot.empty()){capture_boot_title(rom,screenshot);return 0;}
                const auto next=run_boot_menus(*host,rom,selection,cold_boot,controller_path,next_grid_audio,
                                               driving_camera,progress_path.parent_path(),stunt_scores,stunt_replay,progress.checkpoint());
                cold_boot=false;
                if(next==BootExit::quit)return 0;
                if(next==BootExit::quick){
                    next_grid_audio.reset();launch_game=false;choose_quick=true;continue;
                }
                if(next==BootExit::resume) {
                    const auto& saved=progress.checkpoint();
                    const bool bonus=saved->outcome()==srf::ChampionshipOutcome::bonus;
                    championship=*saved;
                    selection.car=saved->car();
                    selection.league=saved->league();
                    selection.initials=saved->initials();
                    auto entry=std::optional{srf::CourseMapEntry::race_result};
                    if(bonus) {
                        const bool play=present_result({L"Bonus Trax",L"",L"",L"Play bonus",L"Skip bonus"});
                        if(game_closing)return 0;
                        entry=complete_bonus(rom,!play);
                        if(entry) {progress.set_checkpoint(championship);save_progress();}
                    }
                    if(!entry || !show_course_map(*host,rom,*championship,progress,*entry,controller_path,next_grid_audio)) {
                        next_grid_audio.reset();championship.reset();continue;
                    }
                }
                if(game_closing)return 0;
                if(!championship) {
                    const auto league=show_initial_course_map(*host,rom,progress,selection.car,controller_path,
                        next_grid_audio,selection.league);
                    if(!league){if(game_closing)return 0;continue;}
                    selection.league=*league;
                    championship.emplace(*league,selection.car,selection.initials);
                    if(progress.checkpoint()) {progress.set_checkpoint(std::nullopt);save_progress();}
                }
            }
            course=championship->course();
            player_car=selection.car;
            const auto directory=course==srf::CourseId::easy_ride?package_root:package_root/std::string(srf::course_definition(course).slug);
            assets=directory/"models.srfmesh";world_path=directory/"world.srfworld";
            controls=launch_controls;
            race_camera=driving_camera;
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
        if(lan_slot>=0){
            auto rom=srf::Rom::load(rom_path);
            std::optional<Renderer> offscreen;
            if(!host)offscreen.emplace(nullptr,nullptr,0,graphics,course);
            auto& renderer=host?*host->presenter:*offscreen;
            renderer.select_course(course);
            renderer.set_course_colors(course_package);
            renderer.load_textures(sprites,dusk_sprites);
            renderer.load_backdrop(srf::extract_course_backdrop(rom,course));
            if(classic_snes)renderer.use_classic_frame(rom);
            if(lan_grid.empty())lan_grid=lan_slot?std::vector{other_car,player_car}:std::vector{player_car,other_car};
            if(lan_grid.size()<2 || lan_grid.size()>srf::max_lan_drivers || unsigned(lan_slot)>=lan_grid.size())
                throw std::invalid_argument("Invalid LAN seat or grid");
            return play_lan_race(host?&*host:nullptr,renderer,rom,world,course,lan_slot,lan_grid,controller_path,screenshot,lan_drop_in,lan_capture_handoff);
        }
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
        auto road=controls.race?presentation_surfaces(world,course,race_camera.enabled):PresentationRoad{};
        // Ravine cliff faces constrain the chase eye. Only deck faces support rival tires.
        const auto cliffs=race_camera.enabled && course==srf::CourseId::big_ravine
            ?presentation_surfaces(world,course,false).triangles:std::vector<RoadTriangle>{};
        srf::RaceAudioState race_audio_state;
        srf::RaceAudioTuning race_audio_tuning;
        std::array<srf::Vehicle,4> previous{};
        RivalWheelTravel rival_travel;
        const bool replay_here=!input_path.empty() && (!championship || (course==replay_course && player_car==replay_car));
        const auto recorded=replay_here?read_inputs(input_path):std::vector<RecordedInput>{};
        std::size_t input_index=0;
        std::array<std::uint32_t,3> lap_times{};
        std::deque<FinishFrame> finish_replay;
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
            if(!trace_path.empty() && !trace.is_open()) {
                if(std::filesystem::exists(trace_path)) throw std::runtime_error("Trace exists; select a new path");
                trace.open(trace_path);
                if(!trace) throw std::runtime_error("Cannot write native race trace");
            }
            if(championship && trace.is_open()) {
                trace<<"{\"event\":\"grid\",\"course\":"<<srf::json_string(std::string(course_definition.slug))
                     <<",\"round\":"<<championship->round()<<",\"spares\":"<<championship->credits()
                     <<",\"carry\":"<<championship->carry_seconds()<<",\"seconds\":"<<race->remaining_seconds()<<",\"positions\":[";
                for(unsigned i=0;i<previous.size();++i) {
                    const auto& body=previous[i].body;
                    trace<<(i?",[":"[")<<signed_word(body.word(0x1e))<<','<<signed_word(body.word(0x20))<<','<<signed_word(body.word(0x22))<<']';
                }
                trace<<"]}\n"<<std::flush;
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
        std::optional<WetTireSpray> spray;
        if(graphics.wet_tire_spray && (graphics.weather.rain>0 || course==srf::CourseId::big_ravine))spray.emplace();
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
            if(race_camera.enabled)rival_travel.advance(previous,race->racers());
            if(host && race->completion().acknowledged_laps>previous_lap && previous_lap<lap_times.size()) {
                const auto elapsed=race->elapsed_centiseconds();
                lap_times[previous_lap]=elapsed-last_lap_end;
                last_lap_end=elapsed;
            }
            if(previous_phase!=race->phase()) {
                if(spray) {
                    const auto phase=static_cast<unsigned>(race->phase());
                    spray->begin_phase(phase);
                    const bool raining=graphics.weather.rain>0 || (race->racers()[race->player_index()].body.word(0x8a)&1);
                    if(raining)for(unsigned i=0;i<previous.size();++i) {
                        const auto& car=race->racers()[i];
                        if(spray_exposed(course,*environment,car.body))spray->emit(phase,i,previous[i],car);
                    }
                }
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
            if(host && race->completion().acknowledged_laps>=2 &&
               (finish_replay.empty() || finish_replay.back().phase!=race->phase())) {
                if(finish_replay.size()==56)finish_replay.pop_front();
                finish_replay.emplace_back(*race,*scenery,*environment,rival_travel);
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
                        !race->ended(),(grid_fanfare || !race->countdown_active()) &&
                        (!race->ended() || (host && race->completion().finished)));
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
        HWND window=host?host->window:nullptr;
        std::optional<GameScreen> race_screen;
        if(host){
            race_screen.emplace(*host,L"Stunt Race FX",race_screen_proc,nullptr);
            RECT r{};GetClientRect(window,&r);controls.width=r.right;controls.height=r.bottom;controls.back=false;controls.resize=false;
        }else if(screenshot.empty()) {
            WNDCLASSW wc{}; wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=window_proc;
            wc.lpszClassName=L"StuntRaceFXMeshInspector"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Window registration failed");
            RECT window_rect{0,0,static_cast<LONG>(controls.width),static_cast<LONG>(controls.height)};
            AdjustWindowRect(&window_rect,WS_OVERLAPPEDWINDOW,FALSE);
            window=CreateWindowExW(0,wc.lpszClassName,L"Stunt Race FX native model research",WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,CW_USEDEFAULT,window_rect.right-window_rect.left,window_rect.bottom-window_rect.top,nullptr,nullptr,wc.hInstance,nullptr);
            if(!window) throw std::runtime_error("Window creation failed");
        }
        if(race && !grid_art) {
            grid_art.emplace(*race_rom,course,player_car);
            grid_pixels.resize(208*128);
        }
        const auto quick_finish=[&] {
            race_screen.reset();
            const auto camera=race_camera;
            race_camera={};
            const auto result=srf::championship_result(*race);
            const auto best_lap=*std::min_element(lap_times.begin(),lap_times.end());
            const auto choice=show_speed_finish(*host,*race_rom,nullptr,player_car,result,lap_times,best_lap,std::nullopt,
                finish_replay,world,road,race->player_index(),look,controller_path,std::move(audio));
            host->presenter->grid_sweep=false;
            host->presenter->clear_bonus_overlay();
            race_camera=camera;
            const bool again=choice==FinishChoice::race_again || (choice==FinishChoice::advance &&
                present_result({L"Quick Race",course_name,L"",L"Race again",L"Choose course"}));
            if(vr && again)xr_log<<"Restart at phase "<<race->phase()<<std::endl;
            choose_quick=!again;
            controls=launch_controls;
        };
        if(vr && !launch_game) {
            SetWindowTextW(window,(L"Stunt Race FX — "+course_name+L" VR").c_str());
            auto& renderer=*host->presenter;
            renderer.select_course(course);
            renderer.set_course_colors(course_package);renderer.load_textures(sprites,dusk_sprites);
            renderer.load_backdrop(srf::extract_course_backdrop(*race_rom,course));
            renderer.load_world(world,0);
            try {audio=std::make_unique<srf::AudioDevice>(srf::extract_course_audio(*race_rom,course));
                audio->update({},false,false,true,false);}
            catch(const std::exception& e){xr_log<<"Audio: "<<e.what()<<std::endl;audio.reset();}
            const auto companion=CreateWindowW(L"STATIC",L"Race running in the headset.  R: restart    F2: recenter    Esc: course selection",
                WS_CHILD|WS_VISIBLE,12,12,600,60,window,nullptr,GetModuleHandleW(nullptr),nullptr);
            run_bonnet_drive(window,*xr_session,renderer,*race_rom,*race,*scenery,*environment,world,previous,road,rival_travel,look,native_vr_motion,spray?&*spray:nullptr,controller_path,
                *grid_art,grid_pixels,audio,step_race,[&]{
                    race->reset();previous=race->racers();countdown_clock=0;race_audio_state={};input_index=0;
                    rival_travel={};if(spray)*spray=WetTireSpray{};
                    lap_times={};last_lap_end=0;finish_replay.clear();
                    scenery->reset();environment->reset();phase_video_ticks=environment_video_ticks=0;
                    if(audio){audio->restart();audio->update({},false,false,true,false);}
                },xr_log);
            if(companion)DestroyWindow(companion);
            if(!game_closing && !controls.back && race->completion().finished) {quick_finish();continue;}
            if(game_closing || !controls.back)return 0;
            choose_quick=true;controls.back=false;continue;
        }
        std::optional<Renderer> standalone;
        if(!host)standalone.emplace(window,nullptr,0,graphics,course);
        Renderer& renderer=host?*host->presenter:*standalone;
        renderer.select_course(course);
        if(classic_snes && race)renderer.use_classic_frame(*race_rom);
        const auto update_driving_hud=[&](bool show) {
            draw_driving_hud(renderer,*grid_art,grid_pixels,*race,championship?championship->credits():0,show);
        };
        renderer.set_course_colors(course_package);
        renderer.load_textures(sprites,dusk_sprites);
        if(race)renderer.load_backdrop(srf::extract_course_backdrop(*race_rom,course));
        if(controls.world) renderer.load_world(world,0); else renderer.load(models[controls.model],0);
        if(!screenshot.empty()) {
            if(race) {
                for(unsigned i=0;i<steps;++i) step_race(static_cast<std::uint16_t>(fixed_control));
                const auto camera_road=road.sample(race->bridge_poses());
                auto poses=vehicle_poses(*race_rom,previous,race->racers(),race->player_index(),1,true,race_camera.enabled?&rival_travel:nullptr,camera_road,look==RaceLook::n64);
                if(race_camera.enabled)seat_opponents(poses,camera_road);
                const auto& b=race->racers()[race->player_index()].body;
                chase_camera(renderer,poses[race->player_index()],lerp_angle(b.word(0x24),b.word(0x24),1),cliffs.empty()?camera_road:std::span<const RoadTriangle>(cliffs),course);
                renderer.set_environment(environment->presentation());
                renderer.load_race_world(world,poses,*race_rom,*race,*scenery,0,spray?&*spray:nullptr);
                if(rain)renderer.set_rain(rain->pixels());
            }
            if(race && !screenshot_no_hud)update_driving_hud(race_hud_visible);
            renderer.render();
            if(gpu_timing)renderer.benchmark_offscreen();
            renderer.screenshot(screenshot);
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
                    const auto standings=race->standings();
                    const auto elapsed=race->elapsed_centiseconds();
                    const auto remaining=race->remaining_seconds();
                    std::vector<srf::RainPixel> final_rain;
                    if(rain){final_rain.assign(rain->pixels().begin(),rain->pixels().end());rain->reset();}
                    race->reset();input_index=0;countdown_clock=0;previous=race->racers();race_audio_state={};
                    rival_travel={};
                    scenery->reset();environment->reset();
                    phase_video_ticks=environment_video_ticks=0;
                    if(race->phase()!=0 || race->ended()) throw std::runtime_error("Native restart failed to reset race lifecycle");
                    if(trace) trace.close();
                    for(unsigned i=0;i<steps;++i) step_race(static_cast<std::uint16_t>(fixed_control));
                    if(race->phase()!=final_phase || race->completion().finished!=finished || race->failure()!=failure)
                        throw std::runtime_error("Native restart repeat changed race outcome");
                    if(race->standings()!=standings || race->elapsed_centiseconds()!=elapsed || race->remaining_seconds()!=remaining)
                        throw std::runtime_error("Native restart repeat changed finishing order or race clock");
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
            bool audio_paused=false;
            std::unique_ptr<srf::Gamepad> gamepad;
            std::string controller_failure;
            if(race) {
                // The navigation harness reads this status through EnumChildWindows.
                race_hud=CreateWindowExW(0,L"STATIC",L"",WS_CHILD,0,0,0,0,window,nullptr,GetModuleHandleW(nullptr),nullptr);
                SetWindowTextW(window,(L"Stunt Race FX — "+course_name+(look==RaceLook::n64?L" (N64 mode)":look==RaceLook::remastered?L" Remastered":L"")).c_str());
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
            if(borderless && !host)SendMessageW(window,WM_KEYDOWN,VK_F11,0);
            LARGE_INTEGER frequency,last; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&last);
            bool running=true;
            double accumulator=0;
            double grid_fraction=0;
            while(running && !game_closing && !controls.back) {
                MSG message;
                while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    if(message.message==WM_QUIT) running=false;
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                if(!running || controls.back || game_closing) break;
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
                    const bool focused=game_focused(window);
                    const auto pad=gamepad?gamepad->poll(focused,race_camera.look):srf::GamepadInput{};
                    if(race_camera.look) {
                        race_camera.rotate(pad.look_x*elapsed*2.3f,pad.look_y*elapsed*1.7f);
                        if(controls.recenter || pad.recenter)race_camera.center();
                        controls.recenter=false;
                    }
                    if(pad.pause)controls.paused=!controls.paused;
                    if(pad.disconnected)controls.paused=true;
                    if(game_xr && pad.recenter)controls.recenter=true;
                    if(controls.paused && pad.restart){controls.back=true;continue;}
                    if(pad.restart)controls.restart=true;
                    if(race->time_over_pending())controls.restart=false;
                    if(controls.restart && championship) {
                        controls.restart=false;
                        if(audio && !audio_paused) {audio->pause(true);audio_paused=true;}
                        if(present_result({L"RETIRE FROM RACE?",course_name,
                                L"Retiring ends this attempt. Retrying the course uses one spare car.",
                                L"Retire",L"Resume race"})) {
                            retired=true;
                            running=false;
                            continue;
                        }
                        ShowWindow(window,SW_SHOW);
                        QueryPerformanceCounter(&last);
                        continue;
                    }
                    if(controls.restart) {
                        race->reset();previous=race->racers();accumulator=0;countdown_clock=0;input_index=0;controls.restart=false;controls.paused=false;race_audio_state={};
                        lap_times={};last_lap_end=0;finish_replay.clear();
                        rival_travel={};
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
                    if(race->ended() && (championship || (host && race->completion().finished))) {
                        running=false;
                        continue;
                    }
                    const float blend=controls.paused?1.f:float(accumulator*simulation_hz);
                    const auto camera_road=road.sample(race->bridge_poses());
                    auto poses=vehicle_poses(*race_rom,previous,race->racers(),race->player_index(),blend,!game_xr,race_camera.enabled?&rival_travel:nullptr,camera_road,look==RaceLook::n64);
                    if(race_camera.enabled)seat_opponents(poses,camera_road);
                    const auto& a=previous[race->player_index()].body;
                    const auto& b=race->racers()[race->player_index()].body;
                    chase_camera(renderer,poses[race->player_index()],lerp_angle(a.word(0x24),b.word(0x24),blend),cliffs.empty()?camera_road:std::span<const RoadTriangle>(cliffs),course);
                    std::optional<unsigned> grid_seconds;
                    bool held_on_grid=false;
                    renderer.grid_sweep=false;
                    if(grid_countdown && !grid_countdown->finished()) {
                        const auto& frame=grid_countdown->frame();
                        const float chase_weight=std::clamp((int(frame.tick)-int(grid_countdown->drive_tick()))/14.f,0.f,1.f);
                        if(chase_weight<1) {
                            const auto view=grid_camera(renderer,grid_origin,grid_heading,frame.scene_phase,chase_weight);
                            renderer.grid_sweep=true;
                            poses=vehicle_poses(*race_rom,previous,race->racers(),race->player_index(),blend,true,
                                race_camera.enabled?&rival_travel:nullptr,camera_road,look==RaceLook::n64,view);
                            if(race_camera.enabled)seat_opponents(poses,camera_road);
                        }
                        held_on_grid=!frame.driving;
                        grid_seconds=frame.clock_seconds;
                        grid_art->draw(grid_pixels,frame,*race,race_hud_visible);
                        renderer.set_bonus_overlay(grid_pixels,frame.brightness,1.f,40.f,race_hud_visible?grid_art->dashboard(frame,*race):std::span<const std::uint32_t>{},
                                                   0,224,frame.banner_step>=0);
                    } else {
                        update_driving_hud(race_hud_visible);
                    }
                    if(game_xr){
                        if(!held_on_grid)bonnet_camera(renderer,poses[race->player_index()],lerp_angle(a.word(0x24),b.word(0x24),blend));
                    }
                    renderer.set_environment(environment->presentation());
                    renderer.load_race_world(world,poses,*race_rom,*race,*scenery,race->ended()?0.f:blend-1.f,spray?&*spray:nullptr);
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
                    if(!recorded.empty()) status+=championship?L"\r\n  RECORDED BUTTON INPUT - retry rewinds this course"
                        :L"\r\n  RECORDED BUTTON INPUT - native simulation (R restarts input sequence)";
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
                if(!game_xr && IsIconic(window)) { Sleep(50); continue; }
                if(rain_changed){renderer.set_rain(rain->pixels());rain_changed=false;}
                renderer.render();
            }
            if(race_hud){DestroyWindow(race_hud);race_hud=nullptr;}
        }
        if(!game_closing && !launch_game && host && !controls.back && race && race->completion().finished) {
            quick_finish();
            continue;
        }
        if(!championship || retired || !race->completion().finished)audio.reset();
        race_screen.reset();
        if(game_closing)return 0;
        if(!launch_game){
            if(!host || !controls.back)return 0;
            choose_quick=true;controls.back=false;continue;
        }
        driving_camera=race_camera;
        race_camera={};
        if(!championship) continue;
        if(!race->ended() && !retired) { championship.reset(); continue; }
        const auto result=srf::championship_result(*race,retired);
        if(trace.is_open())
            trace<<"{\"event\":\"round-result\",\"place\":"<<result.place<<",\"centiseconds\":"<<result.elapsed_centiseconds
                 <<",\"seconds\":"<<result.remaining_seconds<<",\"retired\":"<<(retired?"true":"false")<<"}\n"<<std::flush;
        const auto prior_best=progress.course_best(result.course);
        const auto outcome=championship->finish(result,progress);
        if(outcome==srf::ChampionshipOutcome::next_race || outcome==srf::ChampionshipOutcome::bonus)
            progress.set_checkpoint(championship);
        else if(outcome==srf::ChampionshipOutcome::class_complete || outcome==srf::ChampionshipOutcome::game_over)
            progress.set_checkpoint(std::nullopt);
        if((!retired && race->completion().finished) || outcome==srf::ChampionshipOutcome::game_over)
            save_progress();
        auto finish_choice=FinishChoice::advance;
        if(!retired && race->completion().finished) {
            finish_choice=show_speed_finish(*host,*race_rom,&*championship,player_car,result,lap_times,progress.course_best(result.course),prior_best,
                                      finish_replay,world,road,race->player_index(),look,controller_path,std::move(audio));
            renderer.grid_sweep=false;
            renderer.clear_bonus_overlay();
        }
        bool proceed=finish_choice!=FinishChoice::back;
        if(proceed && (retired || !race->completion().finished || result.place==4))
            proceed=srf::show_round_result(*championship,result,lap_times,present_result);
        if(game_closing)return 0;
        if(outcome==srf::ChampionshipOutcome::class_complete) {
            if(proceed) srf::show_class_result(*championship,progress,present_result);
            if(selection.league!=srf::SpeedClass::master)
                selection.league=static_cast<srf::SpeedClass>(static_cast<unsigned>(selection.league)+1);
            championship.reset();
        } else if(!proceed || outcome==srf::ChampionshipOutcome::game_over) {
            if(proceed && !championship->results().empty()) srf::show_class_result(*championship,progress,present_result);
            championship.reset();
        } else {
            std::optional<srf::CourseMapEntry> entry;
            if(outcome==srf::ChampionshipOutcome::bonus) {
                entry=complete_bonus(*race_rom,finish_choice==FinishChoice::skip_bonus);
                if(!entry){championship.reset();continue;}
            } else {
                if(outcome==srf::ChampionshipOutcome::next_race)
                    entry=srf::CourseMapEntry::race_result;
                championship->advance();
            }
            if((outcome==srf::ChampionshipOutcome::bonus || outcome==srf::ChampionshipOutcome::retry) &&
               !championship->results().empty()) {
                progress.set_checkpoint(championship);
                save_progress();
            }
            if(entry && !show_course_map(*host,*race_rom,*championship,progress,*entry,controller_path,next_grid_audio)) {
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
