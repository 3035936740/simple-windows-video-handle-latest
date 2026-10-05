#include "engine.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <array>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace svh;
namespace {
HINSTANCE instance;
HWND window, tabs, queueView, logView, currentBar, totalBar, statusView, countView;
HFONT font;
int dpi=96;
std::array<std::vector<HWND>,5> pages;
std::vector<HWND> settings;
std::thread worker;
std::atomic<bool> stop{false};
Tools tools;
std::vector<std::wstring> encoders;
bool busy=false, detecting=false;
size_t totalFiles=0;
constexpr UINT WM_EVENT=WM_APP+1;
enum Id {
    Input=100,InputFile,InputFolder,Output,OutputFolder,OpenOutput,Width,Height,Sizing,Codec,Encoder,
    Quality,Bitrate,Preset,Fps,Audio,Overwrite,Metadata,Recursive,Detect,
    Mirror,Flip,Brightness,Contrast,Saturation,Noise,BlurSigma,BackgroundZoom,
    ImageEnable,ImagePath,ImageBrowse,ImageScale,ImageAlpha,ImageRotate,ImagePos,ImageX,ImageY,
    TextEnable,TextValue,FontPath,FontBrowse,FontSize,FontColor,TextAlpha,TextPos,TextX,TextY,
    Start,Stop,SaveLog
};
struct Event {
    enum Kind { Log,Queue,Progress,Detected,Done } kind=Log;
    std::wstring text;
    std::vector<fs::path> files;
    std::vector<std::wstring> encoders;
    size_t index=0;
    double fraction=0;
    int success=0,failed=0;
};
void Post(Event e) {
    auto p=std::make_unique<Event>(std::move(e));
    if(PostMessageW(window,WM_EVENT,0,reinterpret_cast<LPARAM>(p.get()))) p.release();
}
Callbacks UiCallbacks() {
    Callbacks cb;
    cb.log=[](const std::wstring& s){Event e;e.kind=Event::Log;e.text=s;Post(std::move(e));};
    cb.queue=[](const std::vector<fs::path>& p){Event e;e.kind=Event::Queue;e.files=p;Post(std::move(e));};
    cb.progress=[](size_t i,double f,int success,int failed,const std::wstring& s){Event e;e.kind=Event::Progress;e.index=i;e.fraction=f;e.success=success;e.failed=failed;e.text=s;Post(std::move(e));};
    return cb;
}
int Px(int n){return MulDiv(n,dpi,96);}
HWND Control(int page,const wchar_t* cls,const std::wstring& text,DWORD style,int x,int y,int w,int h,int id=0) {
    HWND c=CreateWindowExW((std::wstring(cls)==L"EDIT")?WS_EX_CLIENTEDGE:0,cls,text.c_str(),WS_CHILD|WS_VISIBLE|style,
        Px(x),Px(y),Px(w),Px(h),window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
    if(!c) throw std::runtime_error("Cannot create window control");
    SendMessageW(c,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    if(page>=0) pages[page].push_back(c);
    if(id>=Input && id<Start) settings.push_back(c);
    return c;
}
void Label(int page,const std::wstring& s,int x,int y,int w=160){Control(page,L"STATIC",s,0,x,y,w,23);}
void Edit(int page,Id id,const std::wstring& s,int x,int y,int w=90,int h=25,DWORD extra=0) {
    Control(page,L"EDIT",s,WS_TABSTOP|(extra?extra:ES_AUTOHSCROLL),x,y,w,h,id);
}
void Button(int page,Id id,const std::wstring& s,int x,int y,int w=95) {
    Control(page,L"BUTTON",s,WS_TABSTOP|BS_PUSHBUTTON,x,y,w,28,id);
}
void Check(int page,Id id,const std::wstring& s,int x,int y,int w,bool checked=false) {
    HWND c=Control(page,L"BUTTON",s,BS_AUTOCHECKBOX|WS_TABSTOP,x,y,w,25,id);SendMessageW(c,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
}
void Combo(int page,Id id,const std::vector<std::wstring>& a,int selected,int x,int y,int w=200) {
    HWND c=Control(page,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,x,y,w,230,id);
    for(const auto& s:a) SendMessageW(c,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(s.c_str()));SendMessageW(c,CB_SETCURSEL,selected,0);
}
std::wstring Text(Id id) {
    HWND c=GetDlgItem(window,id);int n=GetWindowTextLengthW(c);std::wstring s(n+1,L'\0');GetWindowTextW(c,s.data(),n+1);s.resize(n);return s;
}
void Set(Id id,const std::wstring& s){SetWindowTextW(GetDlgItem(window,id),s.c_str());}
bool Checked(Id id){return SendMessageW(GetDlgItem(window,id),BM_GETCHECK,0,0)==BST_CHECKED;}
int Selection(Id id){return static_cast<int>(SendMessageW(GetDlgItem(window,id),CB_GETCURSEL,0,0));}
double Numeric(Id id) {
    auto s=Text(id);size_t n=0;double value;
    try {value=std::stod(s,&n);}catch(...) {throw std::runtime_error("数字参数无效，请检查输入框。");}
    if(n!=s.size()) throw std::runtime_error("数字参数包含多余字符。");return value;
}
int Integer(Id id) {
    double value=Numeric(id);
    if(!std::isfinite(value) || value<-1000000 || value>1000000 || value!=static_cast<int>(value)) throw std::runtime_error("此参数需要输入整数。");
    return static_cast<int>(value);
}
Config ReadConfig() {
    Config c;c.input=Text(Input);c.output=Text(Output);c.width=Integer(Width);c.height=Integer(Height);
    c.mode=static_cast<Mode>(Selection(Sizing));c.codec=Selection(Codec)==0?L"h264":L"hevc";
    static const std::vector<std::wstring> families={L"auto",L"nvenc",L"qsv",L"amf",L"cpu"};c.encoder=families.at(Selection(Encoder));
    c.quality=Integer(Quality);c.bitrate=Integer(Bitrate);c.preset=Text(Preset);c.fps=Numeric(Fps);
    c.audio=Checked(Audio);c.overwrite=Checked(Overwrite);c.stripMetadata=Checked(Metadata);c.recursive=Checked(Recursive);
    c.mirror=Checked(Mirror);c.flip=Checked(Flip);c.brightness=Numeric(Brightness);c.contrast=Numeric(Contrast);c.saturation=Numeric(Saturation);c.noise=Integer(Noise);
    c.blur=Numeric(BlurSigma);c.backgroundZoom=Numeric(BackgroundZoom);
    c.imageEnabled=Checked(ImageEnable);c.image=Text(ImagePath);c.imageScale=Numeric(ImageScale);c.imageAlpha=Numeric(ImageAlpha);c.imageRotation=Numeric(ImageRotate);
    c.imagePosition=static_cast<Position>(Selection(ImagePos));c.imageX=Integer(ImageX);c.imageY=Integer(ImageY);
    c.textEnabled=Checked(TextEnable);c.text=Text(TextValue);c.font=Text(FontPath);c.fontSize=Integer(FontSize);c.fontColor=Text(FontColor);c.textAlpha=Numeric(TextAlpha);
    c.textPosition=static_cast<Position>(Selection(TextPos));c.textX=Integer(TextX);c.textY=Integer(TextY);Validate(c);return c;
}
void SwitchPage() {
    int active=TabCtrl_GetCurSel(tabs);
    for(int i=0;i<5;++i) for(HWND c:pages[i]) ShowWindow(c,i==active?SW_SHOW:SW_HIDE);
}
void AppendLog(const std::wstring& s) {
    int n=GetWindowTextLengthW(logView);
    if(n>180000) {SendMessageW(logView,EM_SETSEL,0,60000);SendMessageW(logView,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(L""));n=GetWindowTextLengthW(logView);}
    auto line=s+L"\r\n";SendMessageW(logView,EM_SETSEL,n,n);SendMessageW(logView,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(line.c_str()));
}
void SetBusy(bool value) {
    busy=value;for(HWND c:settings) EnableWindow(c,!value);
    EnableWindow(GetDlgItem(window,Start),!value && !encoders.empty());EnableWindow(GetDlgItem(window,Stop),value);
}
std::wstring PickFile(const wchar_t* filter,bool save=false) {
    wchar_t name[32768]={};OPENFILENAMEW ofn{};ofn.lStructSize=sizeof(ofn);ofn.hwndOwner=window;ofn.lpstrFile=name;ofn.nMaxFile=32768;ofn.lpstrFilter=filter;
    ofn.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save?OFN_OVERWRITEPROMPT:OFN_FILEMUSTEXIST);
    if(save) ofn.lpstrDefExt=L"txt";
    return (save?GetSaveFileNameW(&ofn):GetOpenFileNameW(&ofn))?std::wstring(name):L"";
}
std::wstring PickFolder() {
    IFileDialog* dialog=nullptr;std::wstring result;
    if(SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog)))) {
        DWORD options=0;dialog->GetOptions(&options);dialog->SetOptions(options|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_NOCHANGEDIR);
        if(SUCCEEDED(dialog->Show(window))) {
            IShellItem* item=nullptr;if(SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path=nullptr;if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&path))) {result=path;CoTaskMemFree(path);}item->Release();
            }
        }
        dialog->Release();
    }
    return result;
}
void SetInput(const std::wstring& p) {
    if(p.empty()) return;Set(Input,p);
    if(Text(Output).empty()) {fs::path in(p);Set(Output,((fs::is_directory(in)?in:in.parent_path())/L"video_output").wstring());}
}
void DetectTools() {
    if(worker.joinable()) worker.join();stop=false;detecting=true;SetBusy(true);SetWindowTextW(statusView,L"正在检测 FFmpeg 与可用编码器…");
    worker=std::thread([]{
        Event e;e.kind=Event::Detected;
        try {tools=FindTools();e.encoders=DetectEncoders(tools,stop,UiCallbacks());}
        catch(const std::exception& ex) {e.text=Wide(ex.what());}
        Post(std::move(e));
    });
}
void LayoutBottom(int width,int height) {
    int x=Px(20),w=width-Px(40),top=Px(410);
    MoveWindow(queueView,x,top,w,Px(105),TRUE);
    MoveWindow(statusView,x,Px(526),w,Px(22),TRUE);MoveWindow(countView,x,Px(554),w,Px(22),TRUE);
    MoveWindow(currentBar,Px(95),Px(581),width/2-Px(120),Px(19),TRUE);
    MoveWindow(totalBar,width/2+Px(80),Px(581),width/2-Px(100),Px(19),TRUE);
    MoveWindow(logView,x,Px(648),w,std::max(Px(50),height-Px(663)),TRUE);
}
void CreateUi() {
    dpi=GetDpiForWindow(window);font=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    tabs=Control(-1,WC_TABCONTROLW,L"",WS_TABSTOP,10,10,980,390);
    for(const auto& s:{L"基础设置",L"画面处理",L"模糊背景",L"水印",L"批处理"}) {TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=const_cast<LPWSTR>(s);TabCtrl_InsertItem(tabs,TabCtrl_GetItemCount(tabs),&item);}
    Label(0,L"输入文件 / 目录",30,53,130);Edit(0,Input,L"",160,50,590);Button(0,InputFile,L"选择文件",765,49);Button(0,InputFolder,L"选择目录",870,49);
    Label(0,L"输出目录",30,89,130);Edit(0,Output,L"",160,86,590);Button(0,OutputFolder,L"选择目录",765,85);Button(0,OpenOutput,L"打开",870,85);
    Label(0,L"输出宽度",30,131);Edit(0,Width,L"720",140,127,85);Label(0,L"高度",245,131,60);Edit(0,Height,L"1080",300,127,85);
    Label(0,L"缩放模式",410,131,90);Combo(0,Sizing,{L"拉伸尺寸",L"等比例缩放 + 黑色背景",L"等比例缩放 + 模糊背景",L"等比例铺满 + 居中裁切"},2,505,127,455);
    Label(0,L"编码格式",30,172,100);Combo(0,Codec,{L"H.264",L"H.265 / HEVC"},0,140,166,150);
    Label(0,L"编码器",315,172,85);Combo(0,Encoder,{L"自动（NVENC → QSV → AMF → CPU）",L"NVIDIA NVENC",L"Intel QSV",L"AMD AMF",L"CPU x264 / x265"},0,400,166,390);Button(0,Detect,L"重新检测",815,165,140);
    Label(0,L"质量 CQ / CRF",30,211,110);Edit(0,Quality,L"22",145,207,60);Label(0,L"码率 kbps",230,211,95);Edit(0,Bitrate,L"0",330,207,90);
    Label(0,L"preset",445,211,65);Edit(0,Preset,L"auto",515,207,130);Label(0,L"FPS（0 = 原始）",680,211,150);Edit(0,Fps,L"0",835,207,120);
    Check(0,Audio,L"保留音频（优先复制，失败转 AAC）",30,250,345,true);Check(0,Recursive,L"递归子目录",390,250,170);
    Check(0,Metadata,L"移除 metadata",575,250,180,true);Check(0,Overwrite,L"覆盖同名输出 / 原 MP4",755,250,220);
    Label(0,L"宽高为 2～8192 的偶数；码率 0 使用质量模式。输出统一为 MP4。",30,294,920);
    Label(0,L"preset：auto 自动适配；NVENC p1～p7；CPU / QSV veryfast～veryslow；AMF speed / balanced / quality。",30,325,930);
    Label(0,L"关闭覆盖时自动添加编号；覆盖仅在编码完全成功后生效。",30,356,930);
    Check(1,Mirror,L"水平镜像",30,62,180);Check(1,Flip,L"垂直翻转",240,62,180);
    Label(1,L"亮度（-1～1）",30,116);Edit(1,Brightness,L"0",220,112,130);
    Label(1,L"对比度（0～3）",30,167);Edit(1,Contrast,L"1",220,163,130);
    Label(1,L"饱和度（0～3）",30,218);Edit(1,Saturation,L"1",220,214,130);
    Label(1,L"噪点强度（0～20）",30,269,185);Edit(1,Noise,L"0",220,265,130);
    Label(1,L"建议轻微调整：亮度 0.01、对比度 1.01、饱和度 1.03、噪点 2。",390,112,565);
    Label(1,L"由 FFmpeg 滤镜执行，处理线程不会阻塞界面。",390,164,565);
    Label(2,L"在【基础设置】选择“等比例缩放 + 模糊背景”即可启用。",30,60,915);
    Label(2,L"高斯模糊强度（0.1～100）",30,123,250);Edit(2,BlurSigma,L"24",300,119,140);
    Label(2,L"背景放大倍率（1～3）",30,182,250);Edit(2,BackgroundZoom,L"1",300,178,140);
    Label(2,L"同源背景放大铺满并居中裁切；前景保持比例、完整显示、居中。",30,240,925);
    Label(2,L"例：1920×1080 → 720×1080，前景约 720×404，背景覆盖整个画布。",30,282,925);
    Label(2,L"背景先降采样再高斯模糊，以减少计算量；编码自动优先 GPU。",30,324,925);
    const std::vector<std::wstring> positions={L"左上",L"右上",L"左下",L"右下",L"居中",L"自定义 X/Y"};
    Check(3,ImageEnable,L"启用图片叠图 / Logo",30,52,240);Edit(3,ImagePath,L"",270,50,570);Button(3,ImageBrowse,L"PNG / JPG",850,49,110);
    Label(3,L"宽度 %",30,93,70);Edit(3,ImageScale,L"20",100,89,65);Label(3,L"Alpha 0～1",185,93,100);Edit(3,ImageAlpha,L"0.65",285,89,65);
    Label(3,L"旋转 °",370,93,65);Edit(3,ImageRotate,L"0",435,89,65);Combo(3,ImagePos,positions,3,525,89,175);
    Label(3,L"X",720,93,20);Edit(3,ImageX,L"24",740,89,70);Label(3,L"Y",830,93,20);Edit(3,ImageY,L"24",850,89,70);
    Check(3,TextEnable,L"启用文字水印",30,135,230);Edit(3,TextValue,L"",270,131,690,62,ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL);
    Label(3,L"字体文件",30,214,100);wchar_t winDir[32768];GetWindowsDirectoryW(winDir,32768);Edit(3,FontPath,(fs::path(winDir)/L"Fonts"/L"msyh.ttc").wstring(),130,210,710);Button(3,FontBrowse,L"选择字体",850,209,110);
    Label(3,L"字号",30,256,55);Edit(3,FontSize,L"36",85,252,65);Label(3,L"颜色",165,256,55);Edit(3,FontColor,L"white",220,252,100);
    Label(3,L"Alpha",340,256,60);Edit(3,TextAlpha,L"0.65",405,252,65);Combo(3,TextPos,positions,3,495,252,195);
    Label(3,L"X",720,256,20);Edit(3,TextX,L"24",740,252,70);Label(3,L"Y",830,256,20);Edit(3,TextY,L"24",850,252,70);
    Label(3,L"PNG 原始 Alpha 会保留；X/Y 仅在“自定义”生效。图片宽度 % 以输出宽度为基准。",30,303,925);
    Label(3,L"文字支持中文和换行；字体需包含对应字符。颜色：white / black / red / green / blue / yellow / #RRGGBB。",30,343,925);
    Label(4,L"1. 在基础设置中选择一个视频或目录，配置输出目录、尺寸和编码。",30,62,925);
    Label(4,L"2. 点击下方【开始】，队列列出所有文件并显示各自状态。",30,114,925);
    Label(4,L"3. 失败的文件会记录 FFmpeg 日志，后续文件继续处理。",30,166,925);
    Label(4,L"4. 点击【停止】终止当前 FFmpeg；已完成的输出保留，未完成临时文件清理。",30,218,925);
    Label(4,L"递归时保留目录结构，并排除输入目录内的输出子目录。",30,270,925);
    Label(4,L"可用编码器检测结果和处理详情见下方日志；【导出日志】可保存 UTF-8 文件。",30,322,925);
    queueView=Control(-1,WC_LISTVIEWW,L"",WS_BORDER|LVS_REPORT|LVS_SINGLESEL,20,410,960,105);
    ListView_SetExtendedListViewStyle(queueView,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
    LVCOLUMNW col{};col.mask=LVCF_TEXT|LVCF_WIDTH;col.cx=Px(700);col.pszText=const_cast<LPWSTR>(L"视频文件");ListView_InsertColumn(queueView,0,&col);
    col.cx=Px(220);col.pszText=const_cast<LPWSTR>(L"状态");ListView_InsertColumn(queueView,1,&col);
    statusView=Control(-1,L"STATIC",L"就绪",SS_LEFT,20,526,960,22);countView=Control(-1,L"STATIC",L"成功 0 / 失败 0",0,20,554,960,22);
    Label(-1,L"当前进度",20,581,75);currentBar=Control(-1,PROGRESS_CLASSW,L"",PBS_SMOOTH,95,581,380,19);
    Label(-1,L"总进度",510,581,70);totalBar=Control(-1,PROGRESS_CLASSW,L"",PBS_SMOOTH,580,581,400,19);
    SendMessageW(currentBar,PBM_SETRANGE32,0,1000);SendMessageW(totalBar,PBM_SETRANGE32,0,1000);
    Button(-1,Start,L"开始",20,608,130);Button(-1,Stop,L"停止",165,608,100);Button(-1,SaveLog,L"导出日志",285,608,130);
    logView=Control(-1,L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL|WS_TABSTOP,20,648,960,125);SendMessageW(logView,EM_SETLIMITTEXT,240000,0);
    SwitchPage();DetectTools();
}
LRESULT CALLBACK WindowProcedure(HWND h,UINT message,WPARAM w,LPARAM l) {
    switch(message) {
        case WM_CREATE:
            window=h;try {CreateUi();}catch(const std::exception& e) {MessageBoxW(h,Wide(e.what()).c_str(),L"界面初始化失败",MB_ICONERROR);return -1;}return 0;
        case WM_NOTIFY:
            if(reinterpret_cast<NMHDR*>(l)->hwndFrom==tabs && reinterpret_cast<NMHDR*>(l)->code==TCN_SELCHANGE) SwitchPage();return 0;
        case WM_SIZE:if(queueView) LayoutBottom(LOWORD(l),HIWORD(l));return 0;
        case WM_COMMAND:
            try {
                switch(LOWORD(w)) {
                    case InputFile:SetInput(PickFile(L"视频文件\0*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.ts;*.mts;*.m2ts;*.flv;*.wmv\0所有文件\0*.*\0"));break;
                    case InputFolder:SetInput(PickFolder());break;
                    case OutputFolder:{auto s=PickFolder();if(!s.empty()) Set(Output,s);break;}
                    case OpenOutput:{auto s=Text(Output);if(!s.empty()) ShellExecuteW(h,L"open",s.c_str(),nullptr,nullptr,SW_SHOWNORMAL);break;}
                    case ImageBrowse:{auto s=PickFile(L"图片\0*.png;*.jpg;*.jpeg\0");if(!s.empty()) Set(ImagePath,s);break;}
                    case FontBrowse:{auto s=PickFile(L"字体\0*.ttf;*.ttc;*.otf\0");if(!s.empty()) Set(FontPath,s);break;}
                    case Detect:if(!busy) DetectTools();break;
                    case Start:
                        if(!busy) {
                            Config c=ReadConfig();if(worker.joinable()) worker.join();stop=false;detecting=false;SetBusy(true);ListView_DeleteAllItems(queueView);totalFiles=0;
                            SendMessageW(currentBar,PBM_SETPOS,0,0);SendMessageW(totalBar,PBM_SETPOS,0,0);
                            SetWindowTextW(countView,L"成功 0 / 失败 0");SetWindowTextW(statusView,L"正在扫描视频文件…");
                            TabCtrl_SetCurSel(tabs,4);SwitchPage();
                            worker=std::thread([c]{RunBatch(c,tools,encoders,stop,UiCallbacks());Event e;e.kind=Event::Done;Post(std::move(e));});
                        }break;
                    case Stop:stop=true;EnableWindow(GetDlgItem(h,Stop),FALSE);SetWindowTextW(statusView,L"正在停止当前任务…");break;
                    case SaveLog:{
                        auto path=PickFile(L"文本日志\0*.txt\0",true);if(!path.empty()) {
                            int n=GetWindowTextLengthW(logView);std::wstring text(n+1,L'\0');GetWindowTextW(logView,text.data(),n+1);text.resize(n);
                            auto bytes=Utf8(text);HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
                            if(f==INVALID_HANDLE_VALUE) throw std::runtime_error("无法写入日志。");DWORD written=0;BOOL ok=WriteFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr);CloseHandle(f);
                            if(!ok || written!=bytes.size()) throw std::runtime_error("日志写入失败。");
                        }break;
                    }
                }
            }catch(const std::exception& e){MessageBoxW(h,Wide(e.what()).c_str(),L"请检查参数",MB_OK|MB_ICONWARNING);}return 0;
        case WM_EVENT:{
            std::unique_ptr<Event> e(reinterpret_cast<Event*>(l));
            if(e->kind==Event::Log) AppendLog(e->text);
            else if(e->kind==Event::Queue) {
                ListView_DeleteAllItems(queueView);totalFiles=e->files.size();
                for(size_t i=0;i<e->files.size();++i) {
                    auto name=e->files[i].wstring();LVITEMW item{};item.mask=LVIF_TEXT;item.iItem=static_cast<int>(i);item.pszText=name.data();ListView_InsertItem(queueView,&item);
                    ListView_SetItemText(queueView,static_cast<int>(i),1,const_cast<LPWSTR>(L"等待"));
                }
            } else if(e->kind==Event::Progress) {
                std::wstring row=e->fraction<1?e->text+L" "+std::to_wstring(static_cast<int>(e->fraction*100))+L"%":e->text;
                ListView_SetItemText(queueView,static_cast<int>(e->index),1,row.data());ListView_EnsureVisible(queueView,static_cast<int>(e->index),FALSE);
                SendMessageW(currentBar,PBM_SETPOS,static_cast<WPARAM>(e->fraction*1000),0);
                if(totalFiles) SendMessageW(totalBar,PBM_SETPOS,static_cast<WPARAM>((e->index+e->fraction)*1000/totalFiles),0);
                SetWindowTextW(statusView,e->text.c_str());auto counts=L"成功 "+std::to_wstring(e->success)+L" / 失败 "+std::to_wstring(e->failed)+L" / 总数 "+std::to_wstring(totalFiles);SetWindowTextW(countView,counts.c_str());
            } else if(e->kind==Event::Detected) {
                if(worker.joinable()) worker.join();encoders=std::move(e->encoders);detecting=false;SetBusy(false);
                if(!e->text.empty()) {AppendLog(e->text);SetWindowTextW(statusView,L"缺少或无法启动 FFmpeg。请放入 ffmpeg.exe、ffprobe.exe 后重新检测。");MessageBoxW(h,L"请将 ffmpeg.exe 和 ffprobe.exe 放在程序目录，或加入系统 PATH，然后点击重新检测。详细错误见日志。",L"FFmpeg 不可用",MB_ICONWARNING);}
                else SetWindowTextW(statusView,encoders.empty()?L"没有可用编码器，请检查 FFmpeg 或驱动。":L"检测完成，可以开始处理。详情见日志。");
            } else if(e->kind==Event::Done) {
                if(worker.joinable()) worker.join();SetBusy(false);SetWindowTextW(statusView,stop?L"已停止，未完成输出已清理。":L"批处理结束，成功 / 失败详情见队列和日志。");
            }
            return 0;
        }
        case WM_CLOSE:{
            stop=true;if(worker.joinable()) worker.join();
            MSG pending;while(PeekMessageW(&pending,h,WM_EVENT,WM_EVENT,PM_REMOVE)) delete reinterpret_cast<Event*>(pending.lParam);
            DestroyWindow(h);return 0;
        }
        case WM_DESTROY:if(font) DeleteObject(font);PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,message,w,l);
}
}
int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int show) {
    instance=hi;InitCommonControls();HRESULT co=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    WNDCLASSW wc{};wc.lpfnWndProc=WindowProcedure;wc.hInstance=hi;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_BTNFACE+1);wc.lpszClassName=L"SimpleVideoHandleWindow";wc.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    if(!RegisterClassW(&wc)) return 1;
    UINT initialDpi=GetDpiForSystem();RECT rect{0,0,MulDiv(1000,initialDpi,96),MulDiv(790,initialDpi,96)};
    DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX;AdjustWindowRectExForDpi(&rect,style,FALSE,0,initialDpi);
    window=CreateWindowExW(0,wc.lpszClassName,L"SimpleVideoHandle — 视频批处理",style,CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,hi,nullptr);
    if(!window) {stop=true;if(worker.joinable()) worker.join();if(SUCCEEDED(co)) CoUninitialize();return 1;}
    ShowWindow(window,show);UpdateWindow(window);MSG msg{};
    while(GetMessageW(&msg,nullptr,0,0)>0) {if(!IsDialogMessageW(window,&msg)) {TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(SUCCEEDED(co)) CoUninitialize();return static_cast<int>(msg.wParam);
}
