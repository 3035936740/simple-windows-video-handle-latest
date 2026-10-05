#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <filesystem>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Ole32.lib")

namespace fs = std::filesystem;

static HINSTANCE gInst;
static HWND gWnd, gLog, gProgress, gStartBtn;
static std::atomic<bool> gStop{false};
static std::mutex gProcMutex;
static HANDLE gCurrentProcess = nullptr;

static const UINT WM_APP_LOG = WM_APP + 1;
static const UINT WM_APP_PROGRESS = WM_APP + 2;
static const UINT WM_APP_DONE = WM_APP + 3;

// Control IDs
enum {
    IDC_INPUT=1001, IDC_INPUT_BROWSE, IDC_OUTPUT, IDC_OUTPUT_BROWSE,
    IDC_WIDTH, IDC_HEIGHT, IDC_MODE, IDC_ENCODER, IDC_QUALITY, IDC_PRESET,
    IDC_FPS, IDC_AUDIO_COPY, IDC_MIRROR, IDC_NOISE, IDC_COLOR,
    IDC_RECURSIVE, IDC_METADATA, IDC_OVERLAY, IDC_OVERLAY_BROWSE,
    IDC_OVERLAY_SCALE, IDC_WMTEXT, IDC_WM_ENABLE, IDC_START, IDC_STOP,
    IDC_OPEN_OUTPUT
};

struct Config {
    std::wstring inputDir, outputDir, overlayPath, watermarkText;
    int width=720, height=1080, mode=1, encoder=0, quality=22, fps=0, overlayScale=20;
    bool audioCopy=true, mirror=false, noise=false, color=false, recursive=true, stripMetadata=true, wmEnable=false;
    std::wstring preset=L"p4";
};

static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}
static void SetText(HWND h, const std::wstring& s) { SetWindowTextW(h, s.c_str()); }
static int GetInt(HWND h, int def=0) { try { return std::stoi(GetText(h)); } catch(...) { return def; } }
static bool Checked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0)==BST_CHECKED; }
static void PostLog(const std::wstring& s) { PostMessageW(gWnd, WM_APP_LOG, 0, (LPARAM)new std::wstring(s)); }

static std::wstring Quote(const std::wstring& s) {
    std::wstring r=L"\"";
    for (wchar_t c: s) { if (c==L'\"') r+=L'\\'; r+=c; }
    r+=L"\""; return r;
}
static std::wstring Lower(std::wstring s) { std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return (wchar_t)towlower(c);}); return s; }
static bool IsVideo(const fs::path& p) {
    static const std::vector<std::wstring> exts={L".mp4",L".mov",L".mkv",L".avi",L".webm",L".m4v",L".ts",L".mts",L".m2ts",L".flv",L".wmv"};
    auto e=Lower(p.extension().wstring()); return std::find(exts.begin(),exts.end(),e)!=exts.end();
}

static std::wstring ExeDir() {
    wchar_t buf[32768]; GetModuleFileNameW(nullptr,buf,32768); return fs::path(buf).parent_path().wstring();
}
static std::wstring FindFFmpeg() {
    fs::path local=fs::path(ExeDir())/L"ffmpeg.exe"; if(fs::exists(local)) return local.wstring();
    wchar_t buf[32768]; DWORD n=SearchPathW(nullptr,L"ffmpeg.exe",nullptr,32768,buf,nullptr); if(n>0 && n<32768) return std::wstring(buf,n);
    return L"";
}
static bool ProgramExists(const wchar_t* name) {
    wchar_t buf[32768]; return SearchPathW(nullptr,name,nullptr,32768,buf,nullptr)>0;
}

static std::wstring EscapeDrawText(std::wstring s) {
    std::wstring r;
    for(wchar_t c:s) {
        if(c==L'\\') r+=L"\\\\";
        else if(c==L'\'') r+=L"\\'";
        else if(c==L':') r+=L"\\:";
        else if(c==L'%') r+=L"\\%";
        else r+=c;
    }
    return r;
}

static std::wstring BaseVideoChain(const Config& c, std::wstring in=L"[0:v]") {
    std::wstringstream ss;
    if(c.mode==0) { // stretch
        ss << in << L"scale=" << c.width << L":" << c.height << L":flags=lanczos";
    } else if(c.mode==1) { // fit + blur
        ss << in << L"split=2[bg][fg];"
           << L"[bg]scale="<<c.width<<L":"<<c.height<<L":force_original_aspect_ratio=increase,crop="<<c.width<<L":"<<c.height
           << L",boxblur=luma_radius=min(h\\,w)/30:luma_power=1[bg2];"
           << L"[fg]scale="<<c.width<<L":"<<c.height<<L":force_original_aspect_ratio=decrease:flags=lanczos[fg2];"
           << L"[bg2][fg2]overlay=(W-w)/2:(H-h)/2";
    } else if(c.mode==2) { // fit + black
        ss << in << L"scale="<<c.width<<L":"<<c.height<<L":force_original_aspect_ratio=decrease:flags=lanczos,pad="
           << c.width<<L":"<<c.height<<L":(ow-iw)/2:(oh-ih)/2:black";
    } else if(c.mode==3) { // crop fill
        ss << in << L"scale="<<c.width<<L":"<<c.height<<L":force_original_aspect_ratio=increase:flags=lanczos,crop="<<c.width<<L":"<<c.height;
    } else {
        ss << in << L"null";
    }
    return ss.str();
}

static std::wstring BuildFilter(const Config& c, bool hasOverlay) {
    // Always use filter_complex to make combinations deterministic.
    std::wstringstream f;
    f << BaseVideoChain(c) << L"[base]";
    std::wstring last=L"[base]";
    int idx=0;
    auto appendUnary=[&](const std::wstring& expr){
        std::wstring out=L"[v"+std::to_wstring(++idx)+L"]";
        f << L";" << last << expr << out; last=out;
    };
    if(c.mirror) appendUnary(L"hflip");
    if(c.noise) appendUnary(L"noise=alls=2:allf=t+u");
    if(c.color) appendUnary(L"eq=brightness=0.01:saturation=1.03:contrast=1.01");
    if(c.wmEnable && !c.watermarkText.empty()) {
        std::wstring font=L"C\\:/Windows/Fonts/msyh.ttc";
        std::wstring expr=L"drawtext=fontfile='"+font+L"':text='"+EscapeDrawText(c.watermarkText)+L"':fontsize=36:fontcolor=white@0.55:borderw=1:bordercolor=black@0.35:x=w-tw-24:y=h-th-24";
        appendUnary(expr);
    }
    if(hasOverlay) {
        int px = std::max(1, c.width * c.overlayScale / 100);
        f << L";[1:v]scale="<<px<<L":-1:flags=lanczos[ov];" << last << L"[ov]overlay=(W-w)-24:(H-h)-24:shortest=1[vout]";
    } else {
        f << L";" << last << L"null[vout]";
    }
    return f.str();
}

static std::wstring EncoderArgs(const Config& c) {
    int enc=c.encoder;
    if(enc==0) enc = ProgramExists(L"nvidia-smi.exe") ? 1 : 5; // Auto: NVIDIA then CPU x264
    std::wstringstream ss;
    switch(enc) {
        case 1: ss << L"-c:v h264_nvenc -preset "<<c.preset<<L" -rc vbr -cq "<<c.quality<<L" -b:v 0 -pix_fmt yuv420p "; break;
        case 2: ss << L"-c:v hevc_nvenc -preset "<<c.preset<<L" -rc vbr -cq "<<c.quality<<L" -b:v 0 -pix_fmt yuv420p "; break;
        case 3: ss << L"-c:v h264_qsv -global_quality "<<c.quality<<L" -look_ahead 1 -pix_fmt nv12 "; break;
        case 4: ss << L"-c:v h264_amf -quality speed -rc cqp -qp_i "<<c.quality<<L" -qp_p "<<c.quality<<L" "; break;
        case 6: ss << L"-c:v libx265 -preset medium -crf "<<c.quality<<L" -pix_fmt yuv420p "; break;
        default:ss << L"-c:v libx264 -preset veryfast -crf "<<c.quality<<L" -pix_fmt yuv420p "; break;
    }
    return ss.str();
}

static bool RunProcess(const std::wstring& cmd, DWORD& exitCode) {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    HANDLE rPipe=nullptr,wPipe=nullptr;
    CreatePipe(&rPipe,&wPipe,&sa,0); SetHandleInformation(rPipe,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si{}; si.cb=sizeof(si); si.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW; si.wShowWindow=SW_HIDE;
    si.hStdOutput=wPipe; si.hStdError=wPipe; si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(),cmd.end()); buf.push_back(0);
    BOOL ok=CreateProcessW(nullptr,buf.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
    CloseHandle(wPipe);
    if(!ok){ CloseHandle(rPipe); exitCode=GetLastError(); return false; }
    {
        std::lock_guard<std::mutex> lk(gProcMutex); gCurrentProcess=pi.hProcess;
    }
    // Drain output to avoid pipe blocking; keep only meaningful last fragments.
    std::string accum; char tmp[4096]; DWORD got=0;
    while(true) {
        while(PeekNamedPipe(rPipe,nullptr,0,nullptr,&got,nullptr) && got>0) {
            DWORD rd=0; if(!ReadFile(rPipe,tmp,std::min<DWORD>(sizeof(tmp)-1,got),&rd,nullptr) || rd==0) break;
            tmp[rd]=0; accum.append(tmp,rd); if(accum.size()>16000) accum.erase(0,8000);
        }
        DWORD w=WaitForSingleObject(pi.hProcess,100);
        if(w==WAIT_OBJECT_0) break;
        if(gStop.load()) { TerminateProcess(pi.hProcess,2); break; }
    }
    while(ReadFile(rPipe,tmp,sizeof(tmp),&got,nullptr) && got) accum.append(tmp,got);
    GetExitCodeProcess(pi.hProcess,&exitCode);
    CloseHandle(rPipe); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    { std::lock_guard<std::mutex> lk(gProcMutex); gCurrentProcess=nullptr; }
    if(exitCode!=0 && !accum.empty()) {
        int needed=MultiByteToWideChar(CP_UTF8,0,accum.data(),(int)accum.size(),nullptr,0);
        std::wstring ws(needed,L'\0'); MultiByteToWideChar(CP_UTF8,0,accum.data(),(int)accum.size(),ws.data(),needed);
        if(ws.size()>3500) ws=ws.substr(ws.size()-3500);
        PostLog(L"FFmpeg 错误尾部信息:\r\n"+ws);
    }
    return true;
}

static Config ReadConfig() {
    Config c;
    c.inputDir=GetText(GetDlgItem(gWnd,IDC_INPUT)); c.outputDir=GetText(GetDlgItem(gWnd,IDC_OUTPUT));
    c.width=std::max(2,GetInt(GetDlgItem(gWnd,IDC_WIDTH),720)); c.height=std::max(2,GetInt(GetDlgItem(gWnd,IDC_HEIGHT),1080));
    c.mode=(int)SendMessageW(GetDlgItem(gWnd,IDC_MODE),CB_GETCURSEL,0,0); c.encoder=(int)SendMessageW(GetDlgItem(gWnd,IDC_ENCODER),CB_GETCURSEL,0,0);
    c.quality=std::clamp(GetInt(GetDlgItem(gWnd,IDC_QUALITY),22),0,51); c.fps=std::max(0,GetInt(GetDlgItem(gWnd,IDC_FPS),0));
    c.preset=GetText(GetDlgItem(gWnd,IDC_PRESET)); if(c.preset.empty()) c.preset=L"p4";
    c.audioCopy=Checked(GetDlgItem(gWnd,IDC_AUDIO_COPY)); c.mirror=Checked(GetDlgItem(gWnd,IDC_MIRROR)); c.noise=Checked(GetDlgItem(gWnd,IDC_NOISE)); c.color=Checked(GetDlgItem(gWnd,IDC_COLOR));
    c.recursive=Checked(GetDlgItem(gWnd,IDC_RECURSIVE)); c.stripMetadata=Checked(GetDlgItem(gWnd,IDC_METADATA));
    c.overlayPath=GetText(GetDlgItem(gWnd,IDC_OVERLAY)); c.overlayScale=std::clamp(GetInt(GetDlgItem(gWnd,IDC_OVERLAY_SCALE),20),1,100);
    c.wmEnable=Checked(GetDlgItem(gWnd,IDC_WM_ENABLE)); c.watermarkText=GetText(GetDlgItem(gWnd,IDC_WMTEXT));
    return c;
}

static void Worker(Config c) {
    gStop=false;
    std::wstring ff=FindFFmpeg();
    if(ff.empty()) { PostLog(L"未找到 ffmpeg.exe。请把 ffmpeg.exe 放到本工具同目录，或加入系统 PATH。\r\n"); PostMessageW(gWnd,WM_APP_DONE,0,0); return; }
    if(c.inputDir.empty()||c.outputDir.empty()||!fs::exists(c.inputDir)) { PostLog(L"输入/输出目录无效。\r\n"); PostMessageW(gWnd,WM_APP_DONE,0,0); return; }
    fs::create_directories(c.outputDir);
    std::vector<fs::path> files;
    try {
        if(c.recursive) for(auto& e:fs::recursive_directory_iterator(c.inputDir)) if(e.is_regular_file()&&IsVideo(e.path())) files.push_back(e.path());
        else for(auto& e:fs::directory_iterator(c.inputDir)) if(e.is_regular_file()&&IsVideo(e.path())) files.push_back(e.path());
    } catch(const std::exception&) { PostLog(L"扫描目录失败。\r\n"); PostMessageW(gWnd,WM_APP_DONE,0,0); return; }
    if(files.empty()) { PostLog(L"没有找到可处理的视频文件。\r\n"); PostMessageW(gWnd,WM_APP_DONE,0,0); return; }
    PostLog(L"找到 "+std::to_wstring(files.size())+L" 个视频。\r\n");
    int done=0,failed=0;
    for(size_t i=0;i<files.size()&&!gStop;i++) {
        const auto& in=files[i];
        fs::path rel; try{rel=fs::relative(in,c.inputDir);}catch(...){rel=in.filename();}
        fs::path out=fs::path(c.outputDir)/rel; out.replace_extension(L".mp4"); fs::create_directories(out.parent_path());
        if(fs::exists(out)) out=out.parent_path()/(out.stem().wstring()+L"_gpu.mp4");
        bool ov=!c.overlayPath.empty()&&fs::exists(c.overlayPath);
        std::wstringstream cmd;
        cmd << Quote(ff) << L" -hide_banner -y -i "<<Quote(in.wstring())<<L" ";
        if(ov) cmd << L"-i "<<Quote(c.overlayPath)<<L" ";
        cmd << L"-filter_complex "<<Quote(BuildFilter(c,ov))<<L" -map \"[vout]\" -map 0:a? ";
        if(c.fps>0) cmd << L"-r "<<c.fps<<L" ";
        cmd << EncoderArgs(c);
        if(c.audioCopy) cmd << L"-c:a copy "; else cmd << L"-c:a aac -b:a 192k ";
        if(c.stripMetadata) cmd << L"-map_metadata -1 -map_chapters -1 ";
        cmd << L"-movflags +faststart -max_muxing_queue_size 4096 "<<Quote(out.wstring());
        PostLog(L"["+std::to_wstring(i+1)+L"/"+std::to_wstring(files.size())+L"] "+in.filename().wstring()+L"\r\n");
        DWORD ec=0; bool launched=RunProcess(cmd.str(),ec);
        if(launched&&ec==0){done++;PostLog(L"  完成 -> "+out.filename().wstring()+L"\r\n");}
        else {failed++;PostLog(L"  失败，退出码: "+std::to_wstring(ec)+L"\r\n");}
        PostMessageW(gWnd,WM_APP_PROGRESS,(WPARAM)((i+1)*100/files.size()),0);
    }
    if(gStop) PostLog(L"任务已停止。\r\n");
    else PostLog(L"全部结束：成功 "+std::to_wstring(done)+L"，失败 "+std::to_wstring(failed)+L"。\r\n");
    PostMessageW(gWnd,WM_APP_DONE,0,0);
}

static std::wstring BrowseFolder(HWND owner) {
    BROWSEINFOW bi{}; bi.hwndOwner=owner; bi.lpszTitle=L"选择文件夹"; bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pid=SHBrowseForFolderW(&bi); if(!pid) return L""; wchar_t p[MAX_PATH]; std::wstring r; if(SHGetPathFromIDListW(pid,p)) r=p; CoTaskMemFree(pid); return r;
}
static std::wstring BrowseImage(HWND owner) {
    wchar_t file[32768]=L""; OPENFILENAMEW ofn{}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=owner; ofn.lpstrFile=file; ofn.nMaxFile=32768;
    ofn.lpstrFilter=L"图片文件\0*.png;*.jpg;*.jpeg;*.webp;*.bmp\0所有文件\0*.*\0"; ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
    return GetOpenFileNameW(&ofn)?std::wstring(file):L"";
}

static HWND Add(const wchar_t* cls,const wchar_t* text,DWORD style,int x,int y,int w,int h,int id=0,DWORD ex=0) {
    return CreateWindowExW(ex,cls,text,WS_CHILD|WS_VISIBLE|style,x,y,w,h,gWnd,(HMENU)(INT_PTR)id,gInst,nullptr);
}
static HWND Label(const wchar_t* t,int x,int y,int w=120,int h=22){return Add(L"STATIC",t,0,x,y,w,h);}
static HWND Edit(const wchar_t* t,int x,int y,int w,int h,int id){return Add(L"EDIT",t,WS_BORDER|ES_AUTOHSCROLL,x,y,w,h,id,WS_EX_CLIENTEDGE);}
static HWND Button(const wchar_t* t,int x,int y,int w,int h,int id,DWORD style=BS_PUSHBUTTON){return Add(L"BUTTON",t,style,x,y,w,h,id);}
static HWND Check(const wchar_t* t,int x,int y,int w,int id,bool on){HWND h=Button(t,x,y,w,22,id,BS_AUTOCHECKBOX);SendMessageW(h,BM_SETCHECK,on?BST_CHECKED:BST_UNCHECKED,0);return h;}
static void ComboItems(HWND h,const std::vector<std::wstring>& items,int sel){for(auto&s:items)SendMessageW(h,CB_ADDSTRING,0,(LPARAM)s.c_str());SendMessageW(h,CB_SETCURSEL,sel,0);}

static LRESULT CALLBACK WndProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    switch(m) {
        case WM_CREATE:{
            HFONT font=(HFONT)GetStockObject(DEFAULT_GUI_FONT);
            auto setf=[&](HWND c){SendMessageW(c,WM_SETFONT,(WPARAM)font,TRUE);};
            Label(L"输入目录",20,18,90); setf(Edit(L"",110,15,650,24,IDC_INPUT)); setf(Button(L"浏览...",770,15,85,25,IDC_INPUT_BROWSE));
            Label(L"输出目录",20,52,90); setf(Edit(L"",110,49,650,24,IDC_OUTPUT)); setf(Button(L"浏览...",770,49,85,25,IDC_OUTPUT_BROWSE)); setf(Button(L"打开",862,49,60,25,IDC_OPEN_OUTPUT));
            Label(L"目标宽度",20,95,85); setf(Edit(L"720",105,91,70,24,IDC_WIDTH)); Label(L"目标高度",190,95,85); setf(Edit(L"1080",275,91,70,24,IDC_HEIGHT));
            Label(L"缩放模式",365,95,75); HWND mode=Add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,440,91,260,160,IDC_MODE);setf(mode);
            ComboItems(mode,{L"拉伸到目标尺寸",L"等比完整显示 + 原视频模糊背景",L"等比完整显示 + 黑色背景",L"等比铺满并裁剪",L"保持原始尺寸"},1);
            Label(L"编码器",20,135,80); HWND enc=Add(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,105,131,260,180,IDC_ENCODER);setf(enc);
            ComboItems(enc,{L"自动（NVIDIA优先，否则CPU）",L"NVIDIA H.264 NVENC",L"NVIDIA H.265/HEVC NVENC",L"Intel H.264 QSV",L"AMD H.264 AMF",L"CPU H.264 x264",L"CPU H.265 x265"},0);
            Label(L"质量/CQ",385,135,75); setf(Edit(L"22",460,131,55,24,IDC_QUALITY)); Label(L"NVENC预设",530,135,85); setf(Edit(L"p4",615,131,55,24,IDC_PRESET)); Label(L"FPS(0=原)",690,135,85); setf(Edit(L"0",775,131,55,24,IDC_FPS));
            setf(Check(L"音频直接复制（最快）",20,175,170,IDC_AUDIO_COPY,true)); setf(Check(L"水平镜像",200,175,100,IDC_MIRROR,false)); setf(Check(L"轻微噪点",305,175,100,IDC_NOISE,false)); setf(Check(L"轻微颜色调整",410,175,125,IDC_COLOR,false));
            setf(Check(L"递归处理子目录",545,175,135,IDC_RECURSIVE,true)); setf(Check(L"移除元数据",690,175,120,IDC_METADATA,true));
            Label(L"叠图/Logo",20,215,85); setf(Edit(L"",105,211,550,24,IDC_OVERLAY)); setf(Button(L"浏览...",665,211,80,25,IDC_OVERLAY_BROWSE)); Label(L"宽度%",760,215,55); setf(Edit(L"20",815,211,45,24,IDC_OVERLAY_SCALE));
            setf(Check(L"文字水印",20,252,90,IDC_WM_ENABLE,false)); setf(Edit(L"",115,248,745,24,IDC_WMTEXT));
            setf(Button(L"开始批量处理",20,290,150,32,IDC_START)); setf(Button(L"停止",180,290,90,32,IDC_STOP));
            gStartBtn=GetDlgItem(h,IDC_START); EnableWindow(GetDlgItem(h,IDC_STOP),FALSE);
            gProgress=Add(PROGRESS_CLASSW,L"",PBS_SMOOTH,285,294,575,24,0); SendMessageW(gProgress,PBM_SETRANGE,0,MAKELPARAM(0,100));
            Label(L"处理日志",20,340,100); gLog=Add(L"EDIT",L"",WS_BORDER|ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL,20,365,902,295,0,WS_EX_CLIENTEDGE); setf(gLog);
            Label(L"说明：模糊背景模式会把原视频等比放大铺满后高斯/盒式模糊，再把完整前景等比缩放居中。FFmpeg 放在 exe 同目录即可。",20,675,900,40);
            return 0;}
        case WM_COMMAND:{
            switch(LOWORD(w)){
                case IDC_INPUT_BROWSE:{auto p=BrowseFolder(h);if(!p.empty()){SetText(GetDlgItem(h,IDC_INPUT),p);if(GetText(GetDlgItem(h,IDC_OUTPUT)).empty())SetText(GetDlgItem(h,IDC_OUTPUT),(fs::path(p)/L"video_output").wstring());}break;}
                case IDC_OUTPUT_BROWSE:{auto p=BrowseFolder(h);if(!p.empty())SetText(GetDlgItem(h,IDC_OUTPUT),p);break;}
                case IDC_OVERLAY_BROWSE:{auto p=BrowseImage(h);if(!p.empty())SetText(GetDlgItem(h,IDC_OVERLAY),p);break;}
                case IDC_OPEN_OUTPUT:{auto p=GetText(GetDlgItem(h,IDC_OUTPUT));if(!p.empty())ShellExecuteW(h,L"open",p.c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;}
                case IDC_START:{Config c=ReadConfig(); if(c.inputDir.empty()||c.outputDir.empty()){MessageBoxW(h,L"请选择输入和输出目录。",L"提示",MB_ICONWARNING);break;}EnableWindow(gStartBtn,FALSE);EnableWindow(GetDlgItem(h,IDC_STOP),TRUE);SetWindowTextW(gLog,L"");SendMessageW(gProgress,PBM_SETPOS,0,0);std::thread(Worker,c).detach();break;}
                case IDC_STOP:{gStop=true;{std::lock_guard<std::mutex>lk(gProcMutex);if(gCurrentProcess)TerminateProcess(gCurrentProcess,2);}break;}
            } return 0;}
        case WM_APP_LOG:{auto s=(std::wstring*)l;int len=GetWindowTextLengthW(gLog);SendMessageW(gLog,EM_SETSEL,len,len);SendMessageW(gLog,EM_REPLACESEL,FALSE,(LPARAM)s->c_str());delete s;return 0;}
        case WM_APP_PROGRESS:SendMessageW(gProgress,PBM_SETPOS,w,0);return 0;
        case WM_APP_DONE:EnableWindow(gStartBtn,TRUE);EnableWindow(GetDlgItem(h,IDC_STOP),FALSE);return 0;
        case WM_CLOSE:gStop=true;{std::lock_guard<std::mutex>lk(gProcMutex);if(gCurrentProcess)TerminateProcess(gCurrentProcess,2);}DestroyWindow(h);return 0;
        case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int show) {
    gInst=hi; InitCommonControls(); CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{};wc.lpfnWndProc=WndProc;wc.hInstance=hi;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);wc.lpszClassName=L"VideoBatchGPUWnd";wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);RegisterClassW(&wc);
    gWnd=CreateWindowExW(0,wc.lpszClassName,L"视频批量 GPU 处理工具 v1.0 (C++ / FFmpeg)",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,960,760,nullptr,nullptr,hi,nullptr);
    ShowWindow(gWnd,show);UpdateWindow(gWnd);MSG msg;while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}CoUninitialize();return (int)msg.wParam;
}
