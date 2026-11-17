namespace {
// Keyboard bindings feed the same SNES button levels as the game's menus.
struct BootWindowInput {
    bool quit{},escape{};
    std::array<bool,256> down{},blocked{};
    std::uint16_t held() const {
        const auto key=[&](unsigned k){return down[k] && !blocked[k];};
        std::uint16_t pad=0;
        if(key(VK_RETURN))pad|=0x1000;
        if(key(VK_SPACE) || key('Z'))pad|=0x8000;
        if(key(VK_BACK) || key(VK_ESCAPE))pad|=0x4000;
        if(key(VK_TAB))pad|=0x2000;
        if(key(VK_UP) || key('W'))pad|=0x0800;
        if(key(VK_DOWN) || key('S'))pad|=0x0400;
        if(key(VK_LEFT) || key('A'))pad|=0x0200;
        if(key(VK_RIGHT) || key('D'))pad|=0x0100;
        if(key('X'))pad|=0x0080;
        return pad;
    }
};
LRESULT CALLBACK boot_window_proc(HWND window,UINT message,WPARAM key,LPARAM data) {
    auto* input=reinterpret_cast<BootWindowInput*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        input=static_cast<BootWindowInput*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(input));
    }
    if(input) {
        if(message==WM_CLOSE){input->quit=true;return 0;}
        if(message==WM_SETFOCUS) {
            input->down.fill(false);
            for(unsigned i=0;i<input->blocked.size();++i)input->blocked[i]=(GetAsyncKeyState(i)&0x8000)!=0;
        }
        if((message==WM_KEYDOWN || message==WM_KEYUP) && key<input->down.size()) {
            const bool pressed=message==WM_KEYDOWN;
            if(key==VK_ESCAPE && pressed && !input->down[key] && !input->blocked[key])input->escape=true;
            input->down[key]=pressed;
            if(!pressed)input->blocked[key]=false;
            return 0;
        }
    }
    return DefWindowProcW(window,message,key,data);
}
}
