namespace {
WNDPROC screen_proc=nullptr;
LRESULT CALLBACK game_window_proc(HWND window,UINT message,WPARAM key,LPARAM data) {
#ifdef SRF_NAVIGATION_TEST
    if(message==WM_APP+1 && private_test_desktop()){capture_test_frame=true;return 0;}
#endif
    if(message==WM_CLOSE){game_closing=true;return 0;}
    if(game_xr && message==WM_KEYDOWN && key==VK_F2){controls.recenter=true;return 0;}
    if(message==WM_KEYDOWN && key==VK_F11)return window_proc(window,message,key,data);
    if(screen_proc)return screen_proc(window,message,key,data);
    return DefWindowProcW(window,message,key,data);
}
LRESULT CALLBACK race_screen_proc(HWND window,UINT message,WPARAM key,LPARAM data) {
    if(message==WM_KEYDOWN && key==VK_ESCAPE){if(!(data&(1L<<30)))controls.back=true;return 0;}
    return window_proc(window,message,key,data);
}
struct GameHost {
    HWND window{};
    std::optional<Renderer> presenter;
    ~GameHost(){presenter.reset();if(window)DestroyWindow(window);}
    void open(bool borderless,const RaceGraphics& graphics) {
        WNDCLASSW wc{};wc.hInstance=GetModuleHandleW(nullptr);wc.lpfnWndProc=game_window_proc;
        wc.lpszClassName=L"StuntRaceFXGame";wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Game window registration failed");
        RECT rect{0,0,LONG(controls.width),LONG(controls.height)};
        AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
        window=CreateWindowExW(0,wc.lpszClassName,L"Stunt Race FX",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,
                               rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
        if(!window)throw std::runtime_error("Cannot open game window");
        if(borderless)SendMessageW(window,WM_KEYDOWN,VK_F11,0);
        GetClientRect(window,&rect);controls.width=rect.right;controls.height=rect.bottom;
        presenter.emplace(window,game_xr?game_xr->device():nullptr,desktop_fps,graphics);
        ShowWindow(window,SW_SHOW);
    }
};
struct GameScreen {
    HWND window;
    WNDPROC previous_proc;
    LONG_PTR previous_data;
    explicit GameScreen(GameHost& host,const wchar_t* title,WNDPROC proc,void* data):
        window(host.window),previous_proc(screen_proc),previous_data(GetWindowLongPtrW(window,GWLP_USERDATA)) {
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(data));screen_proc=proc;
        SetWindowTextW(window,title);
        if(GetFocus()==window)SendMessageW(window,WM_SETFOCUS,0,0);
    }
    ~GameScreen(){
        if(GetCapture()==window)ReleaseCapture();
        screen_proc=previous_proc;SetWindowLongPtrW(window,GWLP_USERDATA,previous_data);
    }
};
struct ChoiceInput {
    BootWindowInput keys;
    int click=-1,scroll{};
};
LRESULT CALLBACK choice_proc(HWND window,UINT message,WPARAM key,LPARAM data){
    auto* input=reinterpret_cast<ChoiceInput*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_MOUSEWHEEL){input->scroll-=GET_WHEEL_DELTA_WPARAM(key)/WHEEL_DELTA*24;return 0;}
    if(message==WM_KEYDOWN && (key==VK_PRIOR || key==VK_NEXT)){input->scroll+=key==VK_NEXT?84:-84;return 0;}
    if(message==WM_LBUTTONUP){
        RECT r{};GetClientRect(window,&r);
        const float w=std::min(float(r.right),r.bottom*4.f/3),h=w*.75f;
        if(w>0){
            const float x=(GET_X_LPARAM(data)-(r.right-w)/2)*256/w,y=(GET_Y_LPARAM(data)-(r.bottom-h)/2)*224/h;
            if(y>=184 && y<210 && x>=8 && x<248)input->click=x<128?0:1;
        }
        return 0;
    }
    return boot_window_proc(window,message,key,data);
}
struct ResultBitmap {
    HDC dc=CreateCompatibleDC(nullptr);
    HBITMAP bitmap{};HGDIOBJ old_bitmap{},old_font{};
    HFONT font{};
    uint32_t* pixels{};
    ResultBitmap(){
        BITMAPINFO info{};auto& h=info.bmiHeader;h.biSize=sizeof(h);h.biWidth=256;h.biHeight=-224;h.biPlanes=1;h.biBitCount=32;
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,reinterpret_cast<void**>(&pixels),nullptr,0);
        font=CreateFontW(-11,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,ANTIALIASED_QUALITY,0,L"Segoe UI");
        if(!dc || !bitmap || !font)throw std::runtime_error("Cannot create game result surface");
        old_bitmap=SelectObject(dc,bitmap);old_font=SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
    }
    ~ResultBitmap(){SelectObject(dc,old_font);SelectObject(dc,old_bitmap);DeleteObject(font);DeleteObject(bitmap);DeleteDC(dc);}
    void text(const std::wstring& str,RECT rect,UINT flags,COLORREF color){
        SetTextColor(dc,color);DrawTextW(dc,str.c_str(),-1,&rect,flags|DT_NOPREFIX);
    }
    void draw(const srf::GameResultPage& page,unsigned choice,int& scroll){
        std::fill_n(pixels,256*224,0xff202020);
        text(page.heading,{10,7,246,26},DT_CENTER,RGB(255,255,255));
        text(page.subtitle,{7,26,249,53},DT_CENTER|DT_WORDBREAK,RGB(211,211,211));
        RECT bounds{10,54,246,181};
        DrawTextW(dc,page.details.c_str(),-1,&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
        scroll=std::clamp(scroll,0,int(std::max(0L,bounds.bottom-181)));
        SaveDC(dc);IntersectClipRect(dc,10,54,246,181);OffsetRect(&bounds,0,-scroll);
        text(page.details,bounds,DT_LEFT|DT_WORDBREAK,RGB(238,238,238));RestoreDC(dc,-1);
        for(unsigned i=0;i<2;++i){
            const auto& label=i?page.secondary:page.action;if(label.empty())continue;
            RECT r{i?130:8,184,i?248:126,210};
            SetDCBrushColor(dc,i==choice?RGB(206,206,206):RGB(66,66,66));FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            text(label,r,DT_CENTER|DT_SINGLELINE|DT_VCENTER,i==choice?RGB(12,12,12):RGB(255,255,255));
        }
        GdiFlush();
    }
};
bool game_result(GameHost& host,srf::GameResultPage page,
                 const std::function<void(unsigned,srf::GameResultPage&)>& navigate={}){
    if(navigate)navigate(0,page);
    ChoiceInput input;std::wstring caption=L"Stunt Race FX — "+page.heading;
    if(!navigate)caption+=L" — "+page.action;
    wchar_t previous[256]{};GetWindowTextW(host.window,previous,256);
    GameScreen screen(host,caption.c_str(),choice_proc,&input);
    ResultBitmap bitmap;std::unique_ptr<srf::Gamepad> pad;
    try{pad=std::make_unique<srf::Gamepad>();}catch(const std::exception&){}
    unsigned choice=0,held_before=input.keys.held()|(pad?pad->poll_frontend(game_focused(host.window)):0);
    const auto show_selection=[&]{
        if(navigate)SetWindowTextW(host.window,(L"Stunt Race FX — "+page.subtitle+L" — "+page.heading).c_str());
    };
    show_selection();
    bool accept=false;
    while(!game_closing && !input.keys.quit){
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)game_closing=true;TranslateMessage(&msg);DispatchMessageW(&msg);}
        bool focused=game_focused(host.window);
        unsigned held=input.keys.held()|(pad?pad->poll_frontend(focused):0),edges=held&~held_before;held_before=held;
        if(focused){
            if(input.keys.escape || (edges&0x4000))break;
            if(!navigate && (edges&0x0800))input.scroll-=24;
            if(!navigate && (edges&0x0400))input.scroll+=24;
            if(navigate && (edges&0x0f00)){navigate(edges,page);show_selection();}
            else if(!page.secondary.empty() && (edges&0x0300))choice^=1;
            if(input.click>=0 && (input.click==0 || !page.secondary.empty())){accept=input.click==0;break;}
            if(edges&0x9080){accept=choice==0;break;}
        }
        bitmap.draw(page,choice,input.scroll);
        host.presenter->render_course_map({bitmap.pixels,256*224});
    }
    SetWindowTextW(host.window,previous);
    return accept && !game_closing;
}
bool quick_board(GameHost& host,srf::CourseId& course,srf::PlayerCar& car){
    constexpr unsigned count=srf::courses.size();
    unsigned index=0,vehicle=unsigned(car);
    const auto selected=[&]{return srf::courses[index].id;};
    while(index+1<count && selected()!=course)++index;
    if(selected()!=course)index=0;
    constexpr std::array names{L"Coupe",L"4WD",L"F-Type"};
    const auto select=[&](unsigned edge,srf::GameResultPage& page){
        if(edge&0x0100)index=(index+1)%count;
        if(edge&0x0200)index=(index+count-1)%count;
        if(edge&0x0800)vehicle=(vehicle+2)%3;
        if(edge&0x0400)vehicle=(vehicle+1)%3;
        const auto name=srf::course_definition(selected()).name;
        page.subtitle=std::wstring(name.begin(),name.end())+L"  /  "+names[vehicle];
        page.details=L"Left / Right    Choose course\r\n";
        page.details+=L"Up / Down    Choose car\r\n";
        page.details+=L"Start / Enter    Race\r\nBack / Esc    Game selection";
    };
    if(!game_result(host,{L"Quick Race",L"",L"",L"Race",L"Game selection"},select))return false;
    course=selected();car=static_cast<srf::PlayerCar>(vehicle);return true;
}

}
