#include <windows.h>
#include <cwchar>
#include <string>
#include <iostream>
int wmain() {
    std::wstring name=L"SRF-Navigation-"+std::to_wstring(GetCurrentProcessId());
    // Never switch the input desktop. Both the validator and game inherit this desktop.
    HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop){std::cerr<<"CreateDesktop "<<GetLastError();return 1;}
    wchar_t* raw=GetCommandLineW();
    auto rest=*raw==L'"'?wcschr(raw+1,L'"'):wcschr(raw,L' ');
    if(!rest)return 2;
    while(*++rest==L' '){}
    std::wstring command=rest;
    STARTUPINFOW si{sizeof(si)};si.lpDesktop=name.data();si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE);si.hStdError=GetStdHandle(STD_ERROR_HANDLE);si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){
        std::cerr<<"CreateProcess "<<GetLastError();return 3;
    }
    WaitForSingleObject(pi.hProcess,INFINITE);DWORD code=1;GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseDesktop(desktop);return code;
}
