#include "engine.h"
#include <condition_variable>
#include <cmath>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
using namespace svh;
namespace {
std::atomic<bool> cancelled{false};
BOOL WINAPI ConsoleControl(DWORD event) {
    if(event==CTRL_C_EVENT || event==CTRL_BREAK_EVENT || event==CTRL_CLOSE_EVENT) {cancelled=true;return TRUE;}return FALSE;
}
}
int wmain(int argc,wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);SetConsoleCtrlHandler(ConsoleControl,TRUE);
    try {
        Config c;bool detect=false,filter=false;int cancelMs=0;
        for(int i=1;i<argc;++i) {
            std::wstring key=argv[i];
            auto value=[&](){if(i+1>=argc) throw std::runtime_error("Missing option value");return std::wstring(argv[++i]);};
            auto number=[&](){auto s=value();size_t n=0;double v=std::stod(s,&n);if(n!=s.size()) throw std::runtime_error("Invalid number");return v;};
            auto integer=[&](){double v=number();if(!std::isfinite(v) || v<-1000000 || v>1000000 || v!=static_cast<int>(v)) throw std::runtime_error("Expected integer");return static_cast<int>(v);};
            if(key==L"--detect") detect=true;
            else if(key==L"--filter") filter=true;
            else if(key==L"--input") c.input=value();
            else if(key==L"--output") c.output=value();
            else if(key==L"--width") c.width=integer();
            else if(key==L"--height") c.height=integer();
            else if(key==L"--quality") c.quality=integer();
            else if(key==L"--bitrate") c.bitrate=integer();
            else if(key==L"--codec") c.codec=value();
            else if(key==L"--encoder") c.encoder=value();
            else if(key==L"--preset") c.preset=value();
            else if(key==L"--fps") c.fps=number();
            else if(key==L"--blur") c.blur=number();
            else if(key==L"--zoom") c.backgroundZoom=number();
            else if(key==L"--brightness") c.brightness=number();
            else if(key==L"--contrast") c.contrast=number();
            else if(key==L"--saturation") c.saturation=number();
            else if(key==L"--noise") c.noise=integer();
            else if(key==L"--recursive") c.recursive=true;
            else if(key==L"--overwrite") c.overwrite=true;
            else if(key==L"--no-audio") c.audio=false;
            else if(key==L"--keep-metadata") c.stripMetadata=false;
            else if(key==L"--mirror") c.mirror=true;
            else if(key==L"--flip") c.flip=true;
            else if(key==L"--image") {c.image=value();c.imageEnabled=true;}
            else if(key==L"--image-scale") c.imageScale=number();
            else if(key==L"--image-alpha") c.imageAlpha=number();
            else if(key==L"--image-rotate") c.imageRotation=number();
            else if(key==L"--image-x") c.imageX=integer();
            else if(key==L"--image-y") c.imageY=integer();
            else if(key==L"--text") {c.text=value();c.textEnabled=true;}
            else if(key==L"--font") c.font=value();
            else if(key==L"--font-size") c.fontSize=integer();
            else if(key==L"--font-color") c.fontColor=value();
            else if(key==L"--text-alpha") c.textAlpha=number();
            else if(key==L"--text-tile-rotation") c.textTileRotation=number();
            else if(key==L"--text-tile-spacing") c.textTileSpacing=integer();
            else if(key==L"--text-x") c.textX=integer();
            else if(key==L"--text-y") c.textY=integer();
            else if(key==L"--cancel-after-ms") cancelMs=integer();
            else if(key==L"--mode") {
                auto v=value();if(v==L"stretch") c.mode=Mode::Stretch;else if(v==L"black") c.mode=Mode::Black;else if(v==L"blur") c.mode=Mode::Blur;else if(v==L"crop") c.mode=Mode::Crop;else throw std::runtime_error("Unknown mode");
            } else if(key==L"--image-position" || key==L"--text-position") {
                auto v=value();const std::map<std::wstring,Position> positions={{L"tl",Position::TopLeft},{L"tr",Position::TopRight},{L"bl",Position::BottomLeft},{L"br",Position::BottomRight},{L"center",Position::Center},{L"custom",Position::Custom},{L"tile-bottom",Position::TileBottom},{L"tile-full",Position::TileFull}};
                if(!positions.count(v)) throw std::runtime_error("Unknown position");if(key==L"--image-position") c.imagePosition=positions.at(v);else c.textPosition=positions.at(v);
            } else if(key==L"--help") {
                std::cout<<"SimpleVideoHandleCLI --input FILE_OR_FOLDER --output FOLDER [--width 720 --height 1080]\n"
                    "--mode blur|black|stretch|crop --codec h264|hevc --encoder auto|nvenc|qsv|amf|cpu\n"
                    "--quality 22 --bitrate 0 --preset auto --fps 0 --recursive --overwrite --no-audio\n"
                    "--image PNG --image-scale 20 --image-alpha .65 --image-rotate 0 --image-position tl|tr|bl|br|center|custom\n"
                    "--text TEXT --font FONT --font-size 36 --font-color white --text-alpha .65 --text-position center\n"
                    "--text-position tl|tr|bl|br|center|custom|tile-bottom|tile-full --text-tile-rotation -30 --text-tile-spacing 40\n"
                    "--mirror --flip --brightness 0 --contrast 1 --saturation 1 --noise 0 --blur 24 --zoom 1\n"
                    "--detect (real encoding probe) | --filter (print filter graph)\n";return 0;
            } else throw std::runtime_error("Unknown option: "+Utf8(key));
        }
        if(filter) {std::cout<<Utf8(BuildFilter(c))<<"\n";return 0;}
        auto t=FindTools();Callbacks cb;cb.log=[](const std::wstring& s){std::cout<<Utf8(s)<<std::endl;};
        if(detect) {auto list=DetectEncoders(t,cancelled,cb);return list.empty()?1:0;}
        Validate(c);if(t.ffmpeg.empty() || t.ffprobe.empty()) throw std::runtime_error("Missing ffmpeg.exe / ffprobe.exe");
        std::vector<std::wstring> list;
        if(c.encoder==L"cpu") {
            auto r=RunProcess(t.ffmpeg,{L"-hide_banner",L"-encoders"},cancelled);
            for(const auto& n:{L"libx264",L"libx265"}) if(r.output.find(Utf8(n))!=std::string::npos) list.emplace_back(n);
        } else list=DetectEncoders(t,cancelled,cb);
        std::mutex mutex;std::condition_variable cv;bool finished=false;std::thread timer;
        if(cancelMs>0) timer=std::thread([&]{std::unique_lock<std::mutex> lock(mutex);if(!cv.wait_for(lock,std::chrono::milliseconds(cancelMs),[&]{return finished;})) cancelled=true;});
        auto result=RunBatch(c,t,list,cancelled,cb);
        {std::lock_guard<std::mutex> lock(mutex);finished=true;}cv.notify_all();if(timer.joinable()) timer.join();
        std::cout<<"RESULT success="<<result.success<<" failed="<<result.failed<<" stopped="<<result.stopped<<"\n";
        return result.stopped?130:result.failed?1:0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 2;}
}
