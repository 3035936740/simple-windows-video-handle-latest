#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
namespace svh {
namespace fs = std::filesystem;
enum class Mode { Stretch, Black, Blur, Crop };
enum class Position { TopLeft, TopRight, BottomLeft, BottomRight, Center, Custom, TileBottom, TileFull };
struct Config {
    fs::path input, output, image, font;
    int width=720, height=1080, quality=22, bitrate=0, noise=0, fontSize=36;
    Mode mode=Mode::Blur;
    std::wstring codec=L"h264", encoder=L"auto", preset=L"auto", text, fontColor=L"white";
    double fps=0, blur=24, backgroundZoom=1, brightness=0, contrast=1, saturation=1;
    double imageScale=20, imageAlpha=0.65, imageRotation=0, textAlpha=0.65;
    Position imagePosition=Position::BottomRight, textPosition=Position::BottomRight;
    int imageX=24, imageY=24, textX=24, textY=24;
    double textTileRotation=-30;
    int textTileSpacing=40;
    bool recursive=false, audio=true, overwrite=false, stripMetadata=true;
    bool mirror=false, flip=false, imageEnabled=false, textEnabled=false;
};
struct Tools { fs::path ffmpeg, ffprobe; };
struct Callbacks {
    std::function<void(const std::wstring&)> log;
    std::function<void(const std::vector<fs::path>&)> queue;
    std::function<void(size_t, double, int, int, const std::wstring&)> progress;
};
struct Summary { int success=0, failed=0; bool stopped=false; };
struct ProcessResult { DWORD code=ERROR_GEN_FAILURE; bool cancelled=false; std::string output; };
std::string Utf8(const std::wstring&);
std::wstring Wide(const std::string&);
std::wstring QuoteArgument(const std::wstring&);
Tools FindTools();
fs::path ExeDirectory();
ProcessResult RunProcess(const fs::path&, const std::vector<std::wstring>&, std::atomic<bool>&,
    const fs::path& cwd={}, const std::function<void(const std::string&)>& line={}, DWORD timeoutMs=0);
std::vector<std::wstring> DetectEncoders(const Tools&, std::atomic<bool>&, const Callbacks&);
void Validate(const Config&);
std::wstring BuildFilter(const Config&);
Summary RunBatch(const Config&, const Tools&, const std::vector<std::wstring>&, std::atomic<bool>&, const Callbacks&);
}
