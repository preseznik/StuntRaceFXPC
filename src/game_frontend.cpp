#include "srf/game_frontend.hpp"
#include "srf/gamepad.hpp"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace srf {
namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t window_class[] = L"StuntRaceFXPC.GameFrontend";
constexpr int course_id = 101, car_id = 102, mode_id = 103, league_id = 104, records_id=106;
constexpr std::array<const wchar_t*,3> league_names{L"Novice",L"Expert",L"Master"};
constexpr std::array<PlayerCar,3> cars{PlayerCar::coupe,PlayerCar::four_wd,PlayerCar::f_type};
constexpr std::array<const wchar_t*,3> car_names{L"COUPE",L"4WD",L"F-TYPE"};

void check(HRESULT value, const char* operation) {
    if(FAILED(value)) throw std::runtime_error(operation);
}

std::wstring wide(std::string_view text) {
    const auto count = MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),
        static_cast<int>(text.size()),nullptr,0);
    if(count<=0) throw std::invalid_argument("Course name must be nonempty UTF-8");
    std::wstring result(static_cast<std::size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),
        result.data(),count);
    return result;
}

std::wstring race_time(std::uint32_t centiseconds) {
    std::wostringstream text;
    text<<centiseconds/6000<<L":"<<std::setfill(L'0')<<std::setw(2)<<(centiseconds/100)%60
        <<L"."<<std::setw(2)<<centiseconds%100;
    return text.str();
}

struct Menu {
    std::span<const GameCourseOption> courses;
    std::vector<std::wstring> names;
    GameSessionChoice result;
    const ChampionshipProgress* progress{};
    const GameResultPage* page{};
    std::unique_ptr<Gamepad> gamepad;
    HWND mode{},mode_label{},league{},league_label{},initials{},initials_label{},records{};
    HWND window{},course{},car{},course_label{},car_label{},description{},hint{},start{},exit{};
    HFONT font{},title_font{};
    UINT dpi = 96;
    bool finished = false;
    int scale(int value) const { return MulDiv(value,static_cast<int>(dpi),96); }
    ~Menu() {
        if(window && IsWindow(window)) DestroyWindow(window);
        if(font) DeleteObject(font);
        if(title_font) DeleteObject(title_font);
    }
};

void fonts(Menu& menu) {
    if(menu.font) DeleteObject(menu.font);
    if(menu.title_font) DeleteObject(menu.title_font);
    menu.font = CreateFontW(-menu.scale(17),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    menu.title_font = CreateFontW(-menu.scale(32),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
        DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    for(auto child : {menu.mode,menu.mode_label,menu.league,menu.league_label,menu.initials,menu.initials_label,menu.course,menu.car,menu.course_label,menu.car_label,menu.description,
                      menu.hint,menu.start,menu.exit,menu.records})
        if(child) SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(menu.font),TRUE);
}

void layout(Menu& menu) {
    RECT bounds{};
    GetClientRect(menu.window,&bounds);
    const int margin = menu.scale(32), width = bounds.right-2*margin;
    const auto place = [&](HWND child,int x,int y,int w,int h) {
        MoveWindow(child,x,menu.scale(y),w,menu.scale(h),TRUE);
    };
    const int button_y = std::max(menu.scale(381),static_cast<int>(bounds.bottom)-menu.scale(114));
    if(menu.page) {
        const int text_y=menu.scale(131);
        MoveWindow(menu.description,margin,text_y,width,button_y-text_y-menu.scale(8),TRUE);
    } else {
        place(menu.description,margin,132,width,50);
        place(menu.mode_label,margin,195,menu.scale(110),27);
        place(menu.mode,margin+menu.scale(120),191,width-menu.scale(120),150);
        for(auto child : {menu.league_label,menu.course_label}) place(child,margin,246,menu.scale(110),27);
        for(auto child : {menu.league,menu.course}) place(child,margin+menu.scale(120),242,width-menu.scale(120),265);
        place(menu.car_label,margin,297,menu.scale(110),27);
        place(menu.car,margin+menu.scale(120),293,width-menu.scale(120),150);
        place(menu.initials_label,margin,342,menu.scale(110),27);
        place(menu.initials,margin+menu.scale(120),338,menu.scale(78),29);
    }
    MoveWindow(menu.start,margin,button_y,menu.scale(230),menu.scale(44),TRUE);
    MoveWindow(menu.exit,margin+menu.scale(246),button_y,menu.scale(142),menu.scale(44),TRUE);
    if(menu.records) MoveWindow(menu.records,margin+width-menu.scale(158),button_y,menu.scale(158),menu.scale(44),TRUE);
    MoveWindow(menu.hint,margin,button_y+menu.scale(60),width,menu.scale(30),TRUE);
}

void paint(Menu& menu,HDC dc) {
    RECT client{};
    GetClientRect(menu.window,&client);
    FillRect(dc,&client,GetSysColorBrush(COLOR_WINDOW));
    RECT header = client;
    header.bottom = menu.scale(112);
    const auto brush = CreateSolidBrush(RGB(19,37,51));
    FillRect(dc,&header,brush);
    DeleteObject(brush);
    SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,RGB(245,248,250));
    auto old_font = SelectObject(dc,menu.title_font);
    RECT title{menu.scale(32),menu.scale(22),client.right-menu.scale(24),menu.scale(66)};
    DrawTextW(dc,menu.page?menu.page->heading.c_str():L"STUNT RACE FX PC",-1,&title,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(dc,menu.font);
    RECT subtitle{menu.scale(34),menu.scale(72),client.right-menu.scale(24),menu.scale(100)};
    DrawTextW(dc,menu.page?menu.page->subtitle.c_str():L"Race selection",-1,&subtitle,DT_LEFT|DT_SINGLELINE|DT_NOPREFIX);
    SelectObject(dc,old_font);
}

void update_mode(Menu& menu) {
    const bool championship = SendMessageW(menu.mode,CB_GETCURSEL,0,0)==0;
    for(auto child : {menu.league_label,menu.league}) ShowWindow(child,championship?SW_SHOW:SW_HIDE);
    for(auto child : {menu.initials_label,menu.initials,menu.records}) ShowWindow(child,championship?SW_SHOW:SW_HIDE);
    for(auto child : {menu.course_label,menu.course}) ShowWindow(child,championship?SW_HIDE:SW_SHOW);
    SetWindowTextW(menu.start,championship?L"&Start Championship":L"&Start Race");
    std::wstring description=L"Choose any course for a single race.";
    if(championship) {
        description=menu.progress->available(SpeedClass::master)?L"Finish in the top three to advance through four courses.":
            L"Finish in the top three to advance. Clear Expert to unlock Master.";
        const auto league=static_cast<SpeedClass>(SendMessageW(menu.league,CB_GETCURSEL,0,0));
        const auto records=menu.progress->records(league);
        if(!records.empty()) description+=L"\r\nClass best    "+wide(records.front().initials)+L"    "+race_time(records.front().total());
    }
    SetWindowTextW(menu.description,description.c_str());
}

void show_records(Menu& menu) {
    const auto league=static_cast<SpeedClass>(SendMessageW(menu.league,CB_GETCURSEL,0,0));
    const auto records=menu.progress->records(league);
    std::wostringstream text;
    if(records.empty()) text<<L"Complete all four races to set the first class record.\r\n";
    unsigned position=0;
    for(const auto& record:records) text<<++position<<L".     "<<wide(record.initials)<<L"     "
        <<car_names[static_cast<unsigned>(record.car)]<<L"     "<<race_time(record.total())<<L"\r\n";
    text<<L"\r\n"<<(records.empty()?L"Courses":L"Leader’s course times")<<L"\r\n";
    for(unsigned i=0;i<4;++i) {
        text<<wide(courses[4*static_cast<unsigned>(league)+i].name);
        if(!records.empty()) text<<L"     "<<race_time(records.front().course_times[i]);
        text<<L"\r\n";
    }
    ShowWindow(menu.window,SW_HIDE);
    show_game_result({L"CLASS RECORDS",wide(class_name(league)),text.str(),L"Back to selection",L""});
    ShowWindow(menu.window,SW_SHOW);
    SetFocus(menu.records);
}

void finish(Menu& menu,bool start) {
    if(start && !menu.page) {
        const auto course = SendMessageW(menu.course,CB_GETCURSEL,0,0);
        const auto car = SendMessageW(menu.car,CB_GETCURSEL,0,0);
        if(course<0 || static_cast<std::size_t>(course)>=menu.courses.size() ||
           car<0 || static_cast<std::size_t>(car)>=cars.size()) return;
        menu.result.course_id = menu.courses[static_cast<std::size_t>(course)].id;
        menu.result.car = cars[static_cast<std::size_t>(car)];
        menu.result.speed_trax = SendMessageW(menu.mode,CB_GETCURSEL,0,0)==0;
        menu.result.league = static_cast<SpeedClass>(SendMessageW(menu.league,CB_GETCURSEL,0,0));
        if(menu.result.speed_trax) {
            wchar_t letters[4]{};
            const int count=GetWindowTextW(menu.initials,letters,4);
            if(count!=3 || !std::all_of(letters,letters+count,[](wchar_t c){return c>=32 && c<=126;})) {
                SetWindowTextW(menu.description,L"Enter three initials using letters, numbers or spaces.");
                SetFocus(menu.initials);
                return;
            }
            menu.result.initials.assign(letters,letters+3);
        }
    }
    menu.result.start = start;
    menu.finished = true;
}

LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto menu = reinterpret_cast<Menu*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        menu = static_cast<Menu*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        menu->window = window;
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(menu));
    }
    if(!menu) return DefWindowProcW(window,message,wparam,lparam);
    switch(message) {
    case WM_CREATE: {
        const auto control = [&](const wchar_t* type,const wchar_t* text,DWORD style,int id=0) {
            return CreateWindowExW(0,type,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        };
        menu->description = control(L"STATIC",menu->page?menu->page->details.c_str():L"",SS_LEFT|SS_NOPREFIX);
        if(!menu->page) {
            menu->mode_label = control(L"STATIC",L"&Mode",SS_LEFT);
            menu->mode = control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,mode_id);
            menu->league_label = control(L"STATIC",L"C&lass",SS_LEFT);
            menu->league = control(L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST,league_id);
            menu->course_label = control(L"STATIC",L"&Course",SS_LEFT);
            menu->course = control(L"COMBOBOX",L"",WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,course_id);
            menu->car_label = control(L"STATIC",L"C&ar",SS_LEFT);
            menu->car = control(L"COMBOBOX",L"",WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,car_id);
            menu->initials_label = control(L"STATIC",L"&Initials",SS_LEFT);
            menu->initials = control(L"EDIT",wide(menu->result.initials).c_str(),WS_BORDER|WS_TABSTOP|ES_UPPERCASE,105);
            SendMessageW(menu->initials,EM_SETLIMITTEXT,3,0);
            if(!menu->mode || !menu->league || !menu->course || !menu->car) return -1;
            for(const auto name : {L"Speed Trax",L"Single Race"}) SendMessageW(menu->mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
            SendMessageW(menu->mode,CB_SETCURSEL,menu->result.speed_trax?0:1,0);
            for(unsigned i=0;i<league_names.size();++i) {
                const auto league=static_cast<SpeedClass>(i);
                if(!menu->progress->available(league)) continue;
                std::wstring label=league_names[i];
                if(menu->progress->cleared(league)) label+=L"  (cleared)";
                SendMessageW(menu->league,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(menu->league,CB_SETCURSEL,static_cast<unsigned>(menu->result.league),0);
            std::size_t selected_course = 0;
            for(std::size_t i=0;i<menu->courses.size();++i) {
                SendMessageW(menu->course,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(menu->names[i].c_str()));
                if(menu->courses[i].id==menu->result.course_id) selected_course = i;
            }
            SendMessageW(menu->course,CB_SETCURSEL,selected_course,0);
            for(std::size_t i=0;i<cars.size();++i) {
                SendMessageW(menu->car,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(car_names[i]));
                if(cars[i]==menu->result.car) SendMessageW(menu->car,CB_SETCURSEL,i,0);
            }
        }
        menu->start = control(L"BUTTON",menu->page?menu->page->action.c_str():L"",WS_TABSTOP|BS_DEFPUSHBUTTON,IDOK);
        menu->exit = control(L"BUTTON",menu->page?menu->page->secondary.c_str():L"E&xit",WS_TABSTOP|BS_PUSHBUTTON,IDCANCEL);
        if(!menu->page) menu->records=control(L"BUTTON",L"Class &records",WS_TABSTOP|BS_PUSHBUTTON,records_id);
        menu->hint = control(L"STATIC",menu->page?L"Enter or A to confirm":L"Tab or D-pad up/down to move     Left/right to select     Enter or A to start",SS_LEFT);
        if(!menu->description || !menu->start || !menu->exit || !menu->hint) return -1;
        if(!menu->page) update_mode(*menu);
        else if(menu->page->secondary.empty()) ShowWindow(menu->exit,SW_HIDE);
        fonts(*menu);
        layout(*menu);
        return 0;
    }
    case WM_COMMAND:
        if(LOWORD(wparam)==records_id && !menu->page) {show_records(*menu);return 0;}
        if((LOWORD(wparam)==mode_id || LOWORD(wparam)==league_id) && HIWORD(wparam)==CBN_SELCHANGE) { update_mode(*menu); return 0; }
        if(LOWORD(wparam)==IDOK) { finish(*menu,true); return 0; }
        if(LOWORD(wparam)==IDCANCEL) { finish(*menu,false); return 0; }
        break;
    case WM_TIMER:
        if(menu->gamepad) {
            const auto button=menu->gamepad->poll_menu(GetForegroundWindow()==window);
            const auto focus=GetFocus();
            if(button==MenuButton::back) finish(*menu,false);
            else if(button==MenuButton::accept && menu->records && focus==menu->records) show_records(*menu);
            else if(button==MenuButton::accept) finish(*menu,focus!=menu->exit);
            else if(button==MenuButton::up || button==MenuButton::down)
                SetFocus(GetNextDlgTabItem(window,focus,button==MenuButton::up));
            else if(button==MenuButton::left || button==MenuButton::right) {
                if(focus==menu->mode || focus==menu->league || focus==menu->car || focus==menu->course) {
                    const int index=static_cast<int>(SendMessageW(focus,CB_GETCURSEL,0,0));
                    SendMessageW(focus,WM_KEYDOWN,button==MenuButton::left?VK_UP:VK_DOWN,0);
                    if(index!=SendMessageW(focus,CB_GETCURSEL,0,0)) update_mode(*menu);
                } else SetFocus(GetNextDlgTabItem(window,focus,button==MenuButton::left));
            }
        }
        return 0;
    case WM_CLOSE:
        finish(*menu,false);
        return 0;
    case WM_SIZE:
        layout(*menu);
        return 0;
    case WM_DPICHANGED: {
        menu->dpi = HIWORD(wparam);
        fonts(*menu);
        const auto rect = reinterpret_cast<const RECT*>(lparam);
        SetWindowPos(window,nullptr,rect->left,rect->top,rect->right-rect->left,
            rect->bottom-rect->top,SWP_NOACTIVATE|SWP_NOZORDER);
        layout(*menu);
        InvalidateRect(window,nullptr,TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {menu->scale(620),menu->scale(549)};
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        auto dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc,GetSysColor(COLOR_WINDOWTEXT));
        SetBkColor(dc,GetSysColor(COLOR_WINDOW));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        paint(*menu,reinterpret_cast<HDC>(wparam));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        const auto dc = BeginPaint(window,&ps);
        paint(*menu,dc);
        EndPaint(window,&ps);
        return 0;
    }
    case DM_GETDEFID:
        return MAKELONG(IDOK,DC_HASDEFID);
    case WM_NCDESTROY:
        menu->window = nullptr;
        SetWindowLongPtrW(window,GWLP_USERDATA,0);
        break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}

void screenshot(Menu& menu,const std::filesystem::path& path) {
    if(std::filesystem::exists(path)) throw std::runtime_error("Screenshot exists; select a new path");
    RECT client{};
    GetClientRect(menu.window,&client);
    const auto width = static_cast<UINT>(client.right), height = static_cast<UINT>(client.bottom);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const auto dc = CreateCompatibleDC(nullptr);
    const auto bitmap = CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!dc || !bitmap || !bits) {
        if(bitmap) DeleteObject(bitmap);
        if(dc) DeleteDC(dc);
        throw std::runtime_error("Could not allocate menu screenshot");
    }
    const auto old = SelectObject(dc,bitmap);
    SendMessageW(menu.window,WM_PRINT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT|PRF_CHILDREN|PRF_ERASEBKGND);
    GdiFlush();
    std::vector<BYTE> pixels(static_cast<BYTE*>(bits),static_cast<BYTE*>(bits)+width*height*4);
    SelectObject(dc,old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    for(std::size_t i=3;i<pixels.size();i+=4) pixels[i] = 255;
    struct ComScope {
        HRESULT result = CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        ~ComScope() { if(SUCCEEDED(result)) CoUninitialize(); }
    } com;
    if(com.result!=RPC_E_CHANGED_MODE) check(com.result,"Menu screenshot COM initialization failed");
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"PNG factory failed");
    check(factory->CreateStream(&stream),"PNG stream failed");
    check(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG output failed");
    check(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder failed");
    check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG initialization failed");
    check(encoder->CreateNewFrame(&frame,nullptr),"PNG frame failed");
    check(frame->Initialize(nullptr),"PNG frame initialization failed");
    check(frame->SetSize(width,height),"PNG dimensions failed");
    auto format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format),"PNG pixel format failed");
    if(format!=GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("Unsupported PNG pixel format");
    check(frame->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data()),"PNG pixels failed");
    check(frame->Commit(),"PNG frame commit failed");
    check(encoder->Commit(),"PNG commit failed");
}
void run_menu(Menu& menu,const std::filesystem::path& capture) {
    SetProcessDPIAware();
    const auto screen = GetDC(nullptr);
    if(screen) {
        menu.dpi = static_cast<UINT>(GetDeviceCaps(screen,LOGPIXELSX));
        ReleaseDC(nullptr,screen);
    }
    WNDCLASSW type{};
    type.lpfnWndProc = window_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr,IDC_ARROW);
    type.hIcon = LoadIconW(nullptr,IDI_APPLICATION);
    type.lpszClassName = window_class;
    if(!RegisterClassW(&type) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Could not register the game start screen");
    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD extended = WS_EX_CONTROLPARENT|(capture.empty()?0:WS_EX_TOOLWINDOW);
    RECT bounds{0,0,menu.scale(720),menu.scale(528)};
    AdjustWindowRectEx(&bounds,style,FALSE,extended);
    const int width = bounds.right-bounds.left,height = bounds.bottom-bounds.top;
    const int x = capture.empty()?(GetSystemMetrics(SM_CXSCREEN)-width)/2:-32000;
    const int y = capture.empty()?(GetSystemMetrics(SM_CYSCREEN)-height)/2:-32000;
    const auto window = CreateWindowExW(extended,window_class,menu.page?L"Speed Trax Results":L"Stunt Race FX PC",style,
        x,y,width,height,nullptr,nullptr,type.hInstance,&menu);
    if(!window) throw std::runtime_error("Could not create the game start screen");
    if(!capture.empty()) {
        // Standard combo boxes create their selected-item display on first show.
        // The capture window is offscreen from creation and never activated.
        ShowWindow(window,SW_SHOWNOACTIVATE);
        UpdateWindow(window);
        screenshot(menu,capture);
        return;
    }
    ShowWindow(window,SW_SHOWNORMAL);
    UpdateWindow(window);
    SetFocus(menu.start);
    try {menu.gamepad=std::make_unique<Gamepad>();}
    catch(const std::exception&) {} // Mouse and keyboard remain available without SDL input.
    if(menu.gamepad) SetTimer(window,1,40,nullptr);
    MSG message{};
    while(!menu.finished) {
        const auto received = GetMessageW(&message,nullptr,0,0);
        if(received<0) throw std::runtime_error("Game start screen message loop failed");
        if(received==0) { PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if(!IsDialogMessageW(window,&message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}
}

GameSessionChoice choose_game_session(std::span<const GameCourseOption> courses,
    const GameSessionChoice& initial,const ChampionshipProgress& progress,const std::filesystem::path& capture) {
    if(courses.empty()) throw std::invalid_argument("No playable courses are installed");
    if(std::find(cars.begin(),cars.end(),initial.car)==cars.end()) throw std::invalid_argument("Unknown player car");
    Menu menu;
    menu.courses = courses;
    menu.result = initial;
    menu.result.start = false;
    menu.progress = &progress;
    for(const auto& course : courses) {
        if(course.id.empty()) throw std::invalid_argument("Course identifier is empty");
        menu.names.push_back(wide(course.name));
    }
    run_menu(menu,capture);
    return menu.result;
}

bool show_game_result(const GameResultPage& page) {
    Menu menu;
    menu.page=&page;
    run_menu(menu,{});
    return menu.result.start;
}

bool show_round_result(const Championship& championship,const RoundResult& result,
                       const std::array<std::uint32_t,3>& lap_times,const ResultPresenter& present) {
    const auto outcome=championship.outcome();
    const bool failed=outcome==ChampionshipOutcome::retry || outcome==ChampionshipOutcome::game_over;
    std::wstring heading=L"RACE RESULTS";
    if(result.retired) heading=L"RETIRED";
    else if(result.failure==RaceFailure::time_out) heading=L"TIME UP";
    else if(result.failure==RaceFailure::wrecked) heading=L"WRECKED";
    else if(result.failure==RaceFailure::out_of_bounds) heading=L"OUT OF COURSE";
    else if(failed) heading=L"RANK OUT";
    const auto name=wide(course_definition(result.course).name);
    const auto subtitle=wide(class_name(championship.league()))+L"   /   Race "+
        std::to_wstring(championship.round())+L" of 4   /   "+name;
    std::wostringstream details;
    details<<wide(championship.initials())<<L"     "<<car_names[static_cast<unsigned>(championship.car())];
    if(!result.retired && result.failure==RaceFailure::none) details<<L"     Position "<<result.place<<L" / 4";
    details<<L"\r\n\r\n";
    for(unsigned i=0;i<lap_times.size();++i)
        if(lap_times[i]) details<<L"Lap "<<i+1<<L"      "<<race_time(lap_times[i])<<L"\r\n";
    details<<L"Race time      "<<race_time(result.elapsed_centiseconds)
           <<L"\r\nClass total     "<<race_time(championship.total_centiseconds());
    if(const auto best=championship.previous_best(); !failed && best)
        details<<L"      Best through race "<<championship.round()<<L"     "<<race_time(*best);
    details<<L"\r\n\r\n";
    std::wstring action=L"Next race";
    if(outcome==ChampionshipOutcome::game_over) {
        details<<L"Game over. All spare cars have been used.";
        action=championship.results().empty()?L"Back to menu":L"Class results";
    } else if(outcome==ChampionshipOutcome::retry) {
        details<<L"Spare cars available    "<<championship.credits();
        action=L"Use spare and retry";
    } else if(outcome==ChampionshipOutcome::class_complete) {
        details<<L"All four courses cleared.";
        action=L"Class results";
    } else {
        details<<L"Time carried    "<<std::min<unsigned>(99,result.remaining_seconds)<<L" s      Spare cars    "<<championship.credits();
        if(outcome==ChampionshipOutcome::bonus) action=L"Bonus Trax";
    }
    return present({heading,subtitle,details.str(),action,
        outcome==ChampionshipOutcome::game_over?L"":L"Back to menu"});
}

void show_class_result(const Championship& championship,const ChampionshipProgress& progress,const ResultPresenter& present) {
    std::wostringstream text;
    for(const auto& race:championship.results())
        text<<wide(course_definition(race.course).name)<<L"     "<<race_time(race.elapsed_centiseconds)<<L"\r\n";
    const bool cleared=championship.outcome()==ChampionshipOutcome::class_complete;
    if(!cleared) text<<wide(course_definition(championship.course()).name)<<L"     Did not qualify\r\n";
    text<<L"\r\n"<<(cleared?L"Combined time":L"Qualified races")<<L"     "<<race_time(championship.total_centiseconds())<<L"\r\n\r\nClass records\r\n";
    unsigned position=0;
    for(const auto& record:progress.records(championship.league()))
        text<<++position<<L".     "<<wide(record.initials)<<L"     "<<race_time(record.total())<<L"\r\n";
    auto subtitle=wide(class_name(championship.league()))+(cleared?L"   /   Championship complete":L"   /   Game over");
    if(cleared && championship.league()==SpeedClass::expert) subtitle+=L"   /   Master available";
    present({L"CLASS RESULTS",subtitle,text.str(),L"Back to menu",L""});
}
}
