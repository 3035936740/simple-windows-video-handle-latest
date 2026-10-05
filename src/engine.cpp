#include "engine.h"
#include <objbase.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <locale>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
namespace svh {
namespace {
struct Handle {
    HANDLE value=nullptr;
    Handle()=default;
    explicit Handle(HANDLE h):value(h){}
    ~Handle(){ if(value && value!=INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
};
std::wstring Lower(std::wstring s) {
    std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});return s;
}
std::wstring Number(double n) {
    std::wostringstream s;s.imbue(std::locale::classic());s<<std::setprecision(8)<<n;return s.str();
}
void Log(const Callbacks& cb,const std::wstring& s){if(cb.log) cb.log(s);}
bool Video(const fs::path& p) {
    static const std::set<std::wstring> ext={L".mp4",L".mov",L".mkv",L".avi",L".webm",L".m4v",L".ts",L".mts",L".m2ts",L".flv",L".wmv"};
    return ext.count(Lower(p.extension().wstring()))!=0;
}
std::wstring PathKey(const fs::path& p){return Lower(fs::weakly_canonical(p).wstring());}
bool Within(const fs::path& p,const fs::path& root) {
    auto a=fs::weakly_canonical(p),b=fs::weakly_canonical(root);auto i=a.begin();
    for(auto j=b.begin();j!=b.end();++j,++i) if(i==a.end() || Lower(i->wstring())!=Lower(j->wstring())) return false;
    return true;
}
struct TempDirectory {
    fs::path path;
    TempDirectory() {
        wchar_t buffer[32768];DWORD n=GetTempPathW(32768,buffer);
        if(!n || n>=32768) throw std::runtime_error("GetTempPath failed");
        GUID id{};if(FAILED(CoCreateGuid(&id))) throw std::runtime_error("CoCreateGuid failed");
        wchar_t guid[40];StringFromGUID2(id,guid,40);
        path=fs::path(buffer)/(L"SimpleVideoHandle_"+std::wstring(guid));
        if(!fs::create_directory(path)) throw std::runtime_error("Cannot create private work directory");
    }
    ~TempDirectory(){std::error_code ec;fs::remove_all(path,ec);}
};
void WriteUtf8(const fs::path& p,const std::wstring& text) {
    std::ofstream f(p,std::ios::binary);auto bytes=Utf8(text);f.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    if(!f) throw std::runtime_error("Cannot write filter/watermark file");
}
std::pair<std::wstring,std::wstring> Coordinates(Position p,bool image,int x,int y) {
    std::wstring w=image?L"W-w":L"w-tw",h=image?L"H-h":L"h-th";
    switch(p) {
        case Position::TopLeft:return {L"24",L"24"};
        case Position::TopRight:return {w+L"-24",L"24"};
        case Position::BottomLeft:return {L"24",h+L"-24"};
        case Position::BottomRight:return {w+L"-24",h+L"-24"};
        case Position::Center:return {L"("+w+L")/2",L"("+h+L")/2"};
        default:return {std::to_wstring(x),std::to_wstring(y)};
    }
}
std::wstring EncoderName(const Config& c,const std::wstring& family) {
    if(family==L"cpu") return c.codec==L"hevc"?L"libx265":L"libx264";
    return c.codec+L"_"+family;
}
bool Has(const std::vector<std::wstring>& a,const std::wstring& s){return std::find(a.begin(),a.end(),s)!=a.end();}
void AddEncoding(std::vector<std::wstring>& a,const Config& c,const std::wstring& enc) {
    auto add=[&](std::initializer_list<std::wstring> items){a.insert(a.end(),items.begin(),items.end());};
    bool nv=enc.find(L"nvenc")!=std::wstring::npos,qsv=enc.find(L"qsv")!=std::wstring::npos,amf=enc.find(L"amf")!=std::wstring::npos;
    std::wstring preset=c.preset;
    const std::vector<std::wstring> cpuPresets={L"ultrafast",L"superfast",L"veryfast",L"faster",L"fast",L"medium",L"slow",L"slower",L"veryslow"};
    const std::vector<std::wstring> qsvPresets={L"veryfast",L"faster",L"fast",L"medium",L"slow",L"slower",L"veryslow"};
    if(preset==L"auto") preset=nv?L"p4":amf?L"balanced":qsv?L"medium":L"veryfast";
    bool valid=nv?(preset.size()==2 && preset[0]==L'p' && preset[1]>=L'1' && preset[1]<=L'7'):
        amf?(preset==L"speed"||preset==L"balanced"||preset==L"quality"):Has(qsv?qsvPresets:cpuPresets,preset);
    // Auto fallback can change backend, so adapt presets that do not belong to the new backend.
    if(!valid && c.encoder==L"auto") preset=nv?L"p4":amf?L"balanced":qsv?L"medium":L"veryfast";
    else if(!valid) throw std::runtime_error("Preset does not match selected encoder; use auto");
    add({L"-c:v",enc,L"-pix_fmt",qsv?L"nv12":L"yuv420p"});add({amf?L"-quality":L"-preset",preset});
    if(c.bitrate>0) {
        if(nv) add({L"-rc",L"vbr"});if(amf) add({L"-rc",L"vbr_peak"});
        add({L"-b:v",std::to_wstring(c.bitrate)+L"k"});
    } else if(nv) add({L"-rc",L"vbr",L"-cq",std::to_wstring(c.quality),L"-b:v",L"0"});
    else if(qsv) add({L"-global_quality",std::to_wstring(std::max(1,c.quality))});
    else if(amf) add({L"-rc",L"cqp",L"-qp_i",std::to_wstring(c.quality),L"-qp_p",std::to_wstring(c.quality)});
    else add({L"-crf",std::to_wstring(c.quality)});
    if(enc==L"libx265") add({L"-x265-params",L"pools=4:log-level=error"});
    if(c.codec==L"hevc") add({L"-tag:v",L"hvc1"});
}

bool TiledText(const Config& c) {
    return c.textEnabled && (c.textPosition==Position::TileBottom || c.textPosition==Position::TileFull);
}
fs::path CreateTiledTextLayer(const Config& c,const Tools& tools,const fs::path& work,
    std::atomic<bool>& stop,const Callbacks& cb) {
    WriteUtf8(work/L"watermark.txt",c.text);
    fs::copy_file(c.font,work/L"font.ttf");
    const auto lineCount=static_cast<int>(std::count(c.text.begin(),c.text.end(),L'\n'))+1;
    const int canvasWidth=4096,canvasHeight=std::min(4096,std::max(128,c.fontSize*(lineCount+2)*2));
    // Render and measure a single transparent text asset with FFmpeg, once per batch.
    // No video frames are inspected or painted in C++.
    std::wstring graph=L"[0:v]drawtext=fontfile=font.ttf:textfile=watermark.txt:expansion=none:fontsize="+
        std::to_wstring(c.fontSize)+L":fontcolor="+c.fontColor+
        L":x=16:y=16,split[ink][alpha];[alpha]alphaextract,bbox=min_val=1[measure];[measure]nullsink;[ink]format=rgba[out]";
    WriteUtf8(work/L"text-render.txt",graph);
    auto invoke=[&](const std::vector<std::wstring>& args)->ProcessResult {
        auto r=RunProcess(tools.ffmpeg,args,stop,work,{},60000);
        if(r.cancelled || stop) throw std::runtime_error("Text watermark preparation cancelled");
        if(r.code!=0) {Log(cb,Wide(r.output));throw std::runtime_error(Utf8(L"生成文字平铺水印失败，详情见 FFmpeg 日志。"));}
        return r;
    };
    auto rendered=invoke({L"-hide_banner",L"-loglevel",L"info",L"-nostdin",L"-y",L"-f",L"lavfi",L"-i",
        L"color=c=black@0.0:s="+std::to_wstring(canvasWidth)+L"x"+std::to_wstring(canvasHeight)+L":r=1,format=rgba",
        L"-filter_complex_threads",L"1",L"-/filter_complex",L"text-render.txt",L"-map",L"[out]",
        L"-frames:v",L"1",L"-c:v",L"png",L"-pix_fmt",L"rgba",L"-update",L"1",L"glyph.png"});
    std::istringstream lines(rendered.output);std::string line,bbox;
    while(std::getline(lines,line)) if(line.find("Parsed_bbox_")!=std::string::npos && line.find("x1:")!=std::string::npos) bbox=line;
    auto metric=[&](const char* name) {
        std::smatch match;std::regex pattern(std::string("\\b")+name+":([0-9]+)");
        if(!std::regex_search(bbox,match,pattern)) throw std::runtime_error("Cannot measure watermark text bounds");
        return std::stoi(match[1].str());
    };
    int x=metric("x1"),y=metric("y1"),w=metric("w"),h=metric("h");
    if(w<=0 || h<=0 || w>canvasWidth || h>canvasHeight) throw std::runtime_error(Utf8(L"水印文字没有可见内容，请检查文本和字体。"));
    if(x+w>=canvasWidth-1 || y+h>=canvasHeight-1) throw std::runtime_error(Utf8(L"水印文本过长，请缩短、换行或降低字号。"));
    int left=std::max(0,x-2),top=std::max(0,y-2);
    w=std::min(canvasWidth-left,x+w+2-left);h=std::min(canvasHeight-top,y+h+2-top);
    double radians=c.textTileRotation*3.14159265358979323846/180;
    int rotatedWidth=std::max(1,static_cast<int>(std::ceil(w*std::abs(std::cos(radians))+h*std::abs(std::sin(radians)))));
    int rotatedHeight=std::max(1,static_cast<int>(std::ceil(w*std::abs(std::sin(radians))+h*std::abs(std::cos(radians)))));
    int cellWidth=rotatedWidth+c.textTileSpacing,cellHeight=rotatedHeight+c.textTileSpacing;
    int columns=(c.width+cellWidth-1)/cellWidth;
    int rows=c.textPosition==Position::TileFull?(c.height+cellHeight-1)/cellHeight:1;
    if(static_cast<long long>(columns)*rows>4096) throw std::runtime_error(Utf8(L"平铺数量过多，请增大字号或平铺间距（最多 4096 个）。"));
    const auto angle=Number(c.textTileRotation)+L"*PI/180";
    std::wstring cellFilter=L"crop="+std::to_wstring(w)+L":"+std::to_wstring(h)+L":"+std::to_wstring(left)+L":"+std::to_wstring(top)+
        L",format=rgba,rotate="+angle+L":ow="+std::to_wstring(rotatedWidth)+L":oh="+std::to_wstring(rotatedHeight)+
        L":c=none,pad="+std::to_wstring(cellWidth)+L":"+std::to_wstring(cellHeight)+L":"+
        std::to_wstring(c.textTileSpacing/2)+L":"+std::to_wstring(c.textTileSpacing/2)+L":color=black@0,format=rgba";
    invoke({L"-hide_banner",L"-loglevel",L"error",L"-nostdin",L"-y",L"-i",L"glyph.png",L"-vf",cellFilter,
        L"-frames:v",L"1",L"-c:v",L"png",L"-pix_fmt",L"rgba",L"-update",L"1",L"cell.png"});
    int bottomMargin=std::min(24,c.height-1);
    int layerHeight=c.textPosition==Position::TileFull?c.height:std::min(cellHeight,c.height-bottomMargin);
    std::wstring tileFilter=L"tile=layout="+std::to_wstring(columns)+L"x"+std::to_wstring(rows)+
        L":nb_frames="+std::to_wstring(columns*rows)+L":color=black@0,crop="+std::to_wstring(c.width)+L":"+
        std::to_wstring(layerHeight)+L":0:0,format=rgba";
    invoke({L"-hide_banner",L"-loglevel",L"error",L"-nostdin",L"-y",L"-loop",L"1",L"-i",L"cell.png",L"-vf",tileFilter,
        L"-frames:v",L"1",L"-c:v",L"png",L"-pix_fmt",L"rgba",L"-update",L"1",L"text-layer.png"});
    Log(cb,L"已生成文字平铺层："+std::to_wstring(columns)+L" 列 × "+std::to_wstring(rows)+L" 行，角度 "+Number(c.textTileRotation)+L"°，间距 "+std::to_wstring(c.textTileSpacing)+L" 像素（本批次复用）。");
    return work/L"text-layer.png";
}

std::vector<fs::path> Scan(const Config& c,std::atomic<bool>& stop) {
    if(fs::is_regular_file(c.input)) return {fs::absolute(c.input)};
    std::vector<fs::path> files;bool excludeOutput=PathKey(c.input)!=PathKey(c.output) && Within(c.output,c.input);
    if(c.recursive) {
        for(auto it=fs::recursive_directory_iterator(c.input,fs::directory_options::skip_permission_denied);
            it!=fs::recursive_directory_iterator() && !stop;++it) {
            if(it->is_symlink()) {if(it->is_directory()) it.disable_recursion_pending();continue;}
            if(excludeOutput && Within(it->path(),c.output)) {if(it->is_directory()) it.disable_recursion_pending();continue;}
            if(it->is_regular_file() && Video(it->path())) files.push_back(fs::absolute(it->path()));
        }
    } else {
        for(const auto& e:fs::directory_iterator(c.input,fs::directory_options::skip_permission_denied))
            if(!stop && e.is_regular_file() && !e.is_symlink() && Video(e.path())) files.push_back(fs::absolute(e.path()));
    }
    std::sort(files.begin(),files.end());return files;
}
double Duration(const Tools& t,const fs::path& input,std::atomic<bool>& stop) {
    auto p=RunProcess(t.ffprobe,{L"-v",L"error",L"-show_entries",L"format=duration",L"-of",L"default=noprint_wrappers=1:nokey=1",input.wstring()},stop,{}, {},30000);
    if(p.code!=0) throw std::runtime_error("ffprobe cannot read input: "+p.output);
    try {double n=std::stod(p.output);return std::isfinite(n) && n>0?n:0;}catch(...) {return 0;}
}
}
std::string Utf8(const std::wstring& s) {
    if(s.empty()) return {};
    int n=WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0,nullptr,nullptr);
    std::string out(n,'\0');WideCharToMultiByte(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n,nullptr,nullptr);return out;
}
std::wstring Wide(const std::string& s) {
    if(s.empty()) return {};
    int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring out(n,L'\0');MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),out.data(),n);return out;
}
std::wstring QuoteArgument(const std::wstring& s) {
    // Microsoft CRT argv rules, including trailing backslashes and embedded quotes.
    std::wstring r=L"\"";size_t slash=0;
    for(wchar_t c:s) {
        if(c==L'\\') {++slash;continue;}
        r.append(c==L'"'?slash*2+1:slash,L'\\');slash=0;r+=c;
    }
    r.append(slash*2,L'\\');r+=L'"';return r;
}
fs::path ExeDirectory() {
    wchar_t p[32768];DWORD n=GetModuleFileNameW(nullptr,p,32768);
    if(!n || n>=32768) throw std::runtime_error("Cannot locate executable");
    return fs::path(std::wstring(p,n)).parent_path();
}
Tools FindTools() {
    auto find=[](const wchar_t* name)->fs::path {
        auto local=ExeDirectory()/name;if(fs::is_regular_file(local)) return local;
        // Check application directory, then PATH only (not the process working directory).
        DWORD n=GetEnvironmentVariableW(L"PATH",nullptr,0);std::wstring env(n,L'\0');
        if(n) {GetEnvironmentVariableW(L"PATH",env.data(),n);env.resize(n-1);}
        std::wstringstream ss(env);std::wstring part;
        while(std::getline(ss,part,L';')) {
            if(part.size()>1 && part.front()==L'"' && part.back()==L'"') part=part.substr(1,part.size()-2);
            if(part.empty()) continue;
            auto file=fs::path(part)/name;std::error_code ec;if(fs::is_regular_file(file,ec)) return fs::absolute(file);
        }
        return {};
    };
    return {find(L"ffmpeg.exe"),find(L"ffprobe.exe")};
}
ProcessResult RunProcess(const fs::path& executable,const std::vector<std::wstring>& args,std::atomic<bool>& stop,
    const fs::path& cwd,const std::function<void(const std::string&)>& line,DWORD timeoutMs) {
    ProcessResult result;if(stop) {result.cancelled=true;return result;}
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};Handle read,write;
    if(!CreatePipe(&read.value,&write.value,&sa,0) || !SetHandleInformation(read.value,HANDLE_FLAG_INHERIT,0)) {result.code=GetLastError();return result;}
    Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
    Handle job(CreateJobObjectW(nullptr,nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(input.value==INVALID_HANDLE_VALUE || !job.value || !SetInformationJobObject(job.value,JobObjectExtendedLimitInformation,&limit,sizeof(limit))) {result.code=GetLastError();return result;}
    STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);si.StartupInfo.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow=SW_HIDE;si.StartupInfo.hStdOutput=write.value;si.StartupInfo.hStdError=write.value;si.StartupInfo.hStdInput=input.value;
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> storage(bytes);
    si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&bytes)) {result.code=GetLastError();return result;}
    HANDLE inherited[]={write.value,input.value};
    if(!UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)) {
        result.code=GetLastError();DeleteProcThreadAttributeList(si.lpAttributeList);return result;
    }
    std::wstring cmd=QuoteArgument(executable.wstring());for(const auto& arg:args) cmd+=L" "+QuoteArgument(arg);
    if(cmd.size()>=32767) {DeleteProcThreadAttributeList(si.lpAttributeList);result.code=ERROR_BAD_LENGTH;return result;}
    PROCESS_INFORMATION pi{};
    BOOL ok=CreateProcessW(executable.c_str(),cmd.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT,nullptr,cwd.empty()?nullptr:cwd.c_str(),&si.StartupInfo,&pi);
    DWORD creationError=ok?0:GetLastError();DeleteProcThreadAttributeList(si.lpAttributeList);CloseHandle(write.value);write.value=nullptr;
    if(!ok) {result.code=creationError;return result;}
    Handle process(pi.hProcess),thread(pi.hThread);
    if(!AssignProcessToJobObject(job.value,process.value)) {
        result.code=GetLastError();TerminateProcess(process.value,result.code);WaitForSingleObject(process.value,INFINITE);return result;
    }
    if(ResumeThread(thread.value)==static_cast<DWORD>(-1)) {result.code=GetLastError();TerminateJobObject(job.value,result.code);WaitForSingleObject(process.value,INFINITE);return result;}
    std::string pending;ULONGLONG started=GetTickCount64();
    auto drain=[&]() {
        DWORD available=0;
        while(PeekNamedPipe(read.value,nullptr,0,nullptr,&available,nullptr) && available) {
            char buf[4096];DWORD count=0;
            if(!ReadFile(read.value,buf,std::min<DWORD>(available,sizeof(buf)),&count,nullptr) || !count) break;
            result.output.append(buf,count);if(result.output.size()>262144) result.output.erase(0,result.output.size()-262144);
            if(line) {
                pending.append(buf,count);size_t end;
                while((end=pending.find_first_of("\r\n"))!=std::string::npos) {
                    if(end) line(pending.substr(0,end));pending.erase(0,end+1);
                }
                if(pending.size()>16384) {line(pending);pending.clear();}
            }
        }
    };
    while(true) {
        drain();if(WaitForSingleObject(process.value,30)==WAIT_OBJECT_0) break;
        if(stop || (timeoutMs && GetTickCount64()-started>timeoutMs)) {
            result.cancelled=stop.load();TerminateJobObject(job.value,result.cancelled?ERROR_CANCELLED:WAIT_TIMEOUT);
            WaitForSingleObject(process.value,INFINITE);break;
        }
    }
    drain();if(line && !pending.empty()) line(pending);GetExitCodeProcess(process.value,&result.code);return result;
}
std::vector<std::wstring> DetectEncoders(const Tools& t,std::atomic<bool>& stop,const Callbacks& cb) {
    if(t.ffmpeg.empty() || t.ffprobe.empty()) throw std::runtime_error("Missing ffmpeg.exe / ffprobe.exe in application directory or PATH");
    Log(cb,L"检测 FFmpeg 编码器并进行真实编码试运行…");
    auto list=RunProcess(t.ffmpeg,{L"-hide_banner",L"-encoders"},stop,{}, {},15000);
    if(list.code!=0) throw std::runtime_error("Cannot query FFmpeg encoders");
    std::vector<std::wstring> available;
    for(const auto& name:{L"h264_nvenc",L"hevc_nvenc",L"h264_qsv",L"hevc_qsv",L"h264_amf",L"hevc_amf",L"libx264",L"libx265"}) {
        if(stop) break;if(list.output.find(Utf8(name))==std::string::npos) continue;
        auto p=RunProcess(t.ffmpeg,{L"-hide_banner",L"-loglevel",L"error",L"-nostdin",L"-f",L"lavfi",L"-i",L"color=c=gray:s=128x128:r=25",L"-frames:v",L"3",L"-an",L"-c:v",name,L"-f",L"null",L"-"},stop,{}, {},15000);
        if(p.code==0) {available.emplace_back(name);Log(cb,std::wstring(name)+L"：可用");}
        else Log(cb,std::wstring(name)+L"：不可用（硬件、驱动或 FFmpeg 支持不足）");
    }
    return available;
}
void Validate(const Config& c) {
    auto check=[](const wchar_t* field,double value,double lo,double hi) {
        if(!std::isfinite(value) || value<lo || value>hi)
            throw std::runtime_error(Utf8(std::wstring(field)+L"：当前值 "+Number(value)+L"，允许范围 "+Number(lo)+L"～"+Number(hi)+L"。"));
    };
    if(c.input.empty() || (!fs::is_regular_file(c.input) && !fs::is_directory(c.input))) throw std::runtime_error("Input file/folder does not exist");
    if(fs::is_regular_file(c.input) && !Video(c.input)) throw std::runtime_error("Unsupported video extension");
    if(c.output.empty()) throw std::runtime_error("Output folder is required");
    check(L"【基础设置】输出宽度",c.width,2,8192);check(L"【基础设置】输出高度",c.height,2,8192);
    if(c.width%2 || c.height%2) throw std::runtime_error(Utf8(L"【基础设置】输出宽高必须为偶数（H.264 / H.265 4:2:0）。"));
    if(c.codec!=L"h264" && c.codec!=L"hevc") throw std::runtime_error("Unsupported codec");
    if(c.encoder!=L"auto" && c.encoder!=L"nvenc" && c.encoder!=L"qsv" && c.encoder!=L"amf" && c.encoder!=L"cpu") throw std::runtime_error("Unsupported encoder family");
    if(static_cast<int>(c.mode)<0 || static_cast<int>(c.mode)>3) throw std::runtime_error("Unsupported sizing mode");
    check(L"【基础设置】码率 kbps",c.bitrate,0,1000000);
    if(c.bitrate==0) check(L"【基础设置】质量 CQ / CRF",c.quality,0,51);
    check(L"【基础设置】FPS",c.fps,0,240);
    if(c.mode==Mode::Blur) {
        check(L"【模糊背景】高斯模糊强度",c.blur,0.1,100);
        check(L"【模糊背景】背景放大倍率",c.backgroundZoom,1,3);
    }
    check(L"【画面处理】亮度",c.brightness,-1,1);check(L"【画面处理】对比度",c.contrast,0,3);
    check(L"【画面处理】饱和度",c.saturation,0,3);check(L"【画面处理】噪点强度",c.noise,0,20);
    if(c.imageEnabled) {
        check(L"【水印】图片宽度 %",c.imageScale,1,200);check(L"【水印】图片 Alpha",c.imageAlpha,0,1);
        check(L"【水印】图片旋转角度",c.imageRotation,-360,360);
        if(static_cast<int>(c.imagePosition)<0 || static_cast<int>(c.imagePosition)>static_cast<int>(Position::Custom)) throw std::runtime_error("Unsupported image position");
        if(!fs::is_regular_file(c.image) || (Lower(c.image.extension().wstring())!=L".png" && Lower(c.image.extension().wstring())!=L".jpg" && Lower(c.image.extension().wstring())!=L".jpeg"))
            throw std::runtime_error("Logo must be an existing PNG/JPG image");
    }
    if(c.textEnabled) {
        check(L"【水印】文字 Alpha",c.textAlpha,0,1);check(L"【水印】字号",c.fontSize,8,300);
        if(static_cast<int>(c.textPosition)<0 || static_cast<int>(c.textPosition)>static_cast<int>(Position::TileFull)) throw std::runtime_error("Unsupported text position");
        if(TiledText(c)) {
            check(L"【水印】平铺旋转角度",c.textTileRotation,-360,360);
            check(L"【水印】平铺间距（像素）",c.textTileSpacing,0,1000);
        }
        if(c.text.empty() || !fs::is_regular_file(c.font)) throw std::runtime_error("Watermark text and an existing font file are required");
        bool color=Has({L"white",L"black",L"red",L"green",L"blue",L"yellow"},Lower(c.fontColor));
        if(c.fontColor.size()==7 && c.fontColor.front()==L'#') color=std::all_of(c.fontColor.begin()+1,c.fontColor.end(),[](wchar_t v){return iswxdigit(v)!=0;});
        if(!color) throw std::runtime_error("Font color must be white/black/red/green/blue/yellow or #RRGGBB");
    }
    if(c.encoder!=L"auto") {std::vector<std::wstring> dummy;AddEncoding(dummy,c,EncoderName(c,c.encoder));}
}
std::wstring BuildFilter(const Config& c) {
    std::wostringstream f;f.imbue(std::locale::classic());auto w=std::to_wstring(c.width),h=std::to_wstring(c.height);
    // Normalize sample aspect ratio before split, including anamorphic sources.
    f<<L"[0:v:0]scale=w='max(2,trunc(iw*sar/2)*2)':h='max(2,trunc(ih/2)*2)',setsar=1";
    if(c.mirror) f<<L",hflip";if(c.flip) f<<L",vflip";f<<L"[src];[src]";
    if(c.mode==Mode::Stretch) f<<L"scale="<<w<<L":"<<h<<L":flags=lanczos";
    else if(c.mode==Mode::Black) f<<L"scale="<<w<<L":"<<h<<L":force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos,setsar=1,pad="<<w<<L":"<<h<<L":(ow-iw)/2:(oh-ih)/2:color=black";
    else if(c.mode==Mode::Crop) f<<L"scale="<<w<<L":"<<h<<L":force_original_aspect_ratio=increase:force_divisible_by=2:flags=lanczos,crop="<<w<<L":"<<h<<L",setsar=1";
    else {
        int bw=static_cast<int>(std::ceil(c.width*c.backgroundZoom/2))*2,bh=static_cast<int>(std::ceil(c.height*c.backgroundZoom/2))*2;
        int sw=std::max(2,c.width/4*2),sh=std::max(2,c.height/4*2);
        // Half-resolution Gaussian blur reduces cost; foreground uses full-resolution Lanczos.
        f<<L"split=2[bg][fg];[bg]scale="<<bw<<L":"<<bh<<L":force_original_aspect_ratio=increase:force_divisible_by=2,crop="<<w<<L":"<<h
         <<L",scale="<<sw<<L":"<<sh<<L",gblur=sigma="<<Number(c.blur/2)<<L":steps=2,scale="<<w<<L":"<<h<<L",setsar=1[background];"
         <<L"[fg]scale="<<w<<L":"<<h<<L":force_original_aspect_ratio=decrease:force_divisible_by=2:flags=lanczos,setsar=1[foreground];"
         <<L"[background][foreground]overlay=x=(W-w)/2:y=(H-h)/2";
    }
    f<<L",setsar=1[base]";std::wstring last=L"[base]";int index=0;
    auto unary=[&](const std::wstring& s){auto next=L"[v"+std::to_wstring(++index)+L"]";f<<L";"<<last<<s<<next;last=next;};
    if(c.brightness!=0 || c.contrast!=1 || c.saturation!=1) unary(L"eq=brightness="+Number(c.brightness)+L":contrast="+Number(c.contrast)+L":saturation="+Number(c.saturation));
    if(c.noise) unary(L"noise=alls="+std::to_wstring(c.noise)+L":allf=t+u");
    if(c.imageEnabled) {
        int px=std::max(2,static_cast<int>(c.width*c.imageScale/100));
        f<<L";[1:v:0]format=rgba,scale="<<px<<L":-1:flags=lanczos,setsar=1,colorchannelmixer=aa="<<Number(c.imageAlpha);
        if(c.imageRotation!=0) {auto angle=Number(c.imageRotation)+L"*PI/180";f<<L",rotate="<<angle<<L":ow=rotw("<<angle<<L"):oh=roth("<<angle<<L"):c=none";}
        auto xy=Coordinates(c.imagePosition,true,c.imageX,c.imageY);
        f<<L"[logo];"<<last<<L"[logo]overlay=x="<<xy.first<<L":y="<<xy.second<<L":eof_action=repeat:repeatlast=1:shortest=0[withlogo]";last=L"[withlogo]";
    }
    if(TiledText(c)) {
        int input=c.imageEnabled?2:1;
        auto y=c.textPosition==Position::TileBottom?L"H-h-"+std::to_wstring(std::min(24,c.height-1)):L"0";
        f<<L";["<<input<<L":v:0]format=rgba,colorchannelmixer=aa="<<Number(c.textAlpha)<<L"[texttiles];"
         <<last<<L"[texttiles]overlay=x=0:y="<<y<<L":eof_action=repeat:repeatlast=1:shortest=0[withtiles]";
        last=L"[withtiles]";
    } else if(c.textEnabled) {
        auto xy=Coordinates(c.textPosition,false,c.textX,c.textY);
        unary(L"drawtext=fontfile=font.ttf:textfile=watermark.txt:expansion=none:fontsize="+std::to_wstring(c.fontSize)+
            L":fontcolor="+c.fontColor+L":alpha="+Number(c.textAlpha)+L":x="+xy.first+L":y="+xy.second);
    }
    if(c.fps>0) unary(L"fps="+Number(c.fps));f<<L";"<<last<<L"format=yuv420p[vout]";return f.str();
}
Summary RunBatch(const Config& c,const Tools& tools,const std::vector<std::wstring>& available,std::atomic<bool>& stop,const Callbacks& cb) {
    Summary summary;
    try {
        Validate(c);if(tools.ffmpeg.empty() || tools.ffprobe.empty()) throw std::runtime_error("Missing ffmpeg.exe / ffprobe.exe");
        fs::create_directories(c.output);auto files=Scan(c,stop);if(cb.queue) cb.queue(files);
        if(files.empty() && !stop) throw std::runtime_error("No supported videos found");
        std::vector<std::wstring> candidates;
        if(c.encoder==L"auto") {
            for(const auto& family:{L"nvenc",L"qsv",L"amf",L"cpu"}) {auto name=EncoderName(c,family);if(Has(available,name)) candidates.push_back(name);}
        } else {auto name=EncoderName(c,c.encoder);if(Has(available,name)) candidates.push_back(name);}
        if(candidates.empty() && !stop) throw std::runtime_error("No usable encoder for selected codec/backend");
        std::unique_ptr<TempDirectory> textAssets;
        fs::path textLayer;
        if(TiledText(c) && !stop) {
            Log(cb,L"正在生成文字平铺水印层…");
            textAssets=std::make_unique<TempDirectory>();
            textLayer=CreateTiledTextLayer(c,tools,textAssets->path,stop,cb);
        }
        std::set<std::wstring> outputs,inputs;for(const auto& p:files) inputs.insert(PathKey(p));
        for(size_t i=0;i<files.size() && !stop;++i) {
            double fraction=0;auto progress=[&](const std::wstring& status){if(cb.progress) cb.progress(i,fraction,summary.success,summary.failed,status);};
            progress(L"准备："+files[i].filename().wstring());fs::path stage;
            try {
                auto rel=fs::is_directory(c.input)?fs::relative(files[i],fs::absolute(c.input)):files[i].filename();
                auto output=fs::absolute(c.output)/rel;output.replace_extension(L".mp4");auto base=output;int suffix=0;
                // Avoid same-stem collisions and overwriting a different source in the queue.
                while(outputs.count(PathKey(output)) || (!c.overwrite && fs::exists(output)) ||
                    (inputs.count(PathKey(output)) && PathKey(output)!=PathKey(files[i])))
                    output=base.parent_path()/(base.stem().wstring()+L"_handled_"+std::to_wstring(++suffix)+L".mp4");
                outputs.insert(PathKey(output));fs::create_directories(output.parent_path());TempDirectory work;
                stage=output.parent_path()/(L".svh_"+work.path.filename().wstring()+L".mp4");WriteUtf8(work.path/L"filter.txt",BuildFilter(c));
                if(c.textEnabled && !TiledText(c)) {WriteUtf8(work.path/L"watermark.txt",c.text);fs::copy_file(c.font,work.path/L"font.ttf");}
                double duration=Duration(tools,files[i],stop);
                Log(cb,L"["+std::to_wstring(i+1)+L"/"+std::to_wstring(files.size())+L"] "+files[i].wstring());bool done=false;
                for(const auto& enc:candidates) {
                    if(stop || done) break;
                    for(int audioAttempt=0;audioAttempt<(c.audio?2:1) && !stop;++audioAttempt) {
                        fraction=0;progress(L"处理中："+files[i].filename().wstring());bool copy=c.audio && audioAttempt==0;
                        Log(cb,L"编码器 "+enc+L"；音频 "+(c.audio?(copy?L"复制":L"AAC"):L"关闭"));
                        std::vector<std::wstring> a={L"-hide_banner",L"-loglevel",L"warning",L"-nostdin",L"-nostats",L"-y",L"-progress",L"pipe:1",L"-stats_period",L"0.2",L"-i",files[i].wstring()};
                        if(c.imageEnabled) {a.push_back(L"-i");a.push_back(fs::absolute(c.image).wstring());}
                        if(!textLayer.empty()) {a.push_back(L"-i");a.push_back(textLayer.wstring());}
                        a.insert(a.end(),{L"-filter_complex_threads",L"2",L"-/filter_complex",L"filter.txt",L"-map",L"[vout]"});AddEncoding(a,c,enc);
                        if(c.audio) {a.insert(a.end(),{L"-map",L"0:a?",L"-c:a",copy?L"copy":L"aac"});if(!copy) a.insert(a.end(),{L"-b:a",L"192k"});}
                        else a.push_back(L"-an");
                        if(c.stripMetadata) a.insert(a.end(),{L"-map_metadata",L"-1",L"-map_chapters",L"-1"});
                        a.insert(a.end(),{L"-fps_mode",L"passthrough",L"-movflags",L"+faststart",L"-max_muxing_queue_size",L"4096",stage.wstring()});
                        auto result=RunProcess(tools.ffmpeg,a,stop,work.path,[&](const std::string& line){
                            if(line.rfind("out_time_us=",0)==0) {
                                try {double seconds=std::stod(line.substr(12))/1000000;if(duration>0) fraction=std::clamp(seconds/duration,0.0,0.99);}catch(...){}
                                progress(L"处理中："+files[i].filename().wstring());
                            } else {
                                auto eq=line.find('=');static const std::set<std::string> keys={"frame","fps","stream_0_0_q","bitrate","total_size","out_time_us","out_time_ms","out_time","dup_frames","drop_frames","speed","progress"};
                                if(eq==std::string::npos || !keys.count(line.substr(0,eq))) Log(cb,Wide(line));
                            }
                        });
                        if(result.cancelled || stop) break;
                        if(result.code==0 && fs::is_regular_file(stage) && fs::file_size(stage)>0) {done=true;break;}
                        Log(cb,L"FFmpeg 退出码："+std::to_wstring(result.code));if(copy) Log(cb,L"音频复制未成功，重试 AAC（保留原始时间线）。");
                    }
                    if(!done && !stop && c.encoder==L"auto") Log(cb,L"当前编码器失败，尝试下一可用编码器。");
                }
                if(stop) {std::error_code ec;fs::remove(stage,ec);progress(L"已停止");break;}
                if(!done) throw std::runtime_error("All encoding attempts failed; original file preserved");
                // Commit only after a complete successful encode. No shell, no in-place partial output.
                if(!MoveFileExW(stage.c_str(),output.c_str(),MOVEFILE_WRITE_THROUGH|(c.overwrite?MOVEFILE_REPLACE_EXISTING:0)))
                    throw std::runtime_error("Cannot commit completed output; error "+std::to_string(GetLastError()));
                ++summary.success;fraction=1;Log(cb,L"成功 → "+output.wstring());progress(L"成功");
            } catch(const std::exception& e) {
                if(!stage.empty()) {std::error_code ec;fs::remove(stage,ec);}
                if(stop) {progress(L"已停止");break;}
                ++summary.failed;fraction=1;Log(cb,L"失败："+Wide(e.what()));progress(L"失败");
            }
        }
    } catch(const std::exception& e) {if(!stop) {++summary.failed;Log(cb,L"任务错误："+Wide(e.what()));}}
    summary.stopped=stop.load();Log(cb,(summary.stopped?L"任务已停止。":L"任务结束。")+std::wstring(L"成功 ")+std::to_wstring(summary.success)+L"，失败 "+std::to_wstring(summary.failed));return summary;
}
}

