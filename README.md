# SimpleVideoHandle — Windows 视频批处理

原生 Win32 中文界面，C++17 / CMake / FFmpeg，Windows 10/11 x64。沿用原工程的“C++ 管任务、FFmpeg 管视频”思路，处理核心和 GUI 已分离。生产程序不使用 Python，不在 C++ 中逐帧处理视频。

## 直接运行

发布文件在 `bin/`：

```text
bin/
  SimpleVideoHandle.exe       中文图形界面
  SimpleVideoHandleCLI.exe    同一核心的命令行入口 / 自动测试
  ffmpeg.exe
  ffprobe.exe
  README.md
  THIRD_PARTY.md
  dependencies.json          EXE / DLL 导入依赖审计
  SHA256.json                发布文件校验值
  licenses/FFmpeg/           FFmpeg 上游许可证及说明
```

主程序使用 MSVC 静态运行库 `/MT`，无需 Qt、Python 或额外 VC++ 运行库。打包脚本解析 PE 导入表、递归补齐所有**非 Windows 系统 DLL**；使用静态 FFmpeg 构建时无需额外 DLL。Windows 系统 DLL 和显卡驱动随系统提供，不能从开发机随意复制。

FFmpeg 查找顺序：程序所在目录，然后系统 PATH。`ffmpeg.exe`、`ffprobe.exe` 均必须存在；缺失时界面会明确提示，可放入文件后点击“重新检测”。支持 FFmpeg 7.1 及以上，需含 drawtext、gblur、libx264/libx265 和对应硬件编码器。

## 使用步骤

1. 【基础设置】选择一个视频文件，或整个目录；按需勾选“递归子目录”。
2. 选择输出目录，输入任意偶数宽高（2～8192），例如 720×1080、1080×1920、1080×1080、1920×1080。
3. 选择缩放模式、H.264 / H.265、编码器和质量参数。
4. 在【画面处理】【模糊背景】【水印】中配置效果。
5. 点击“开始”。队列、当前进度、总进度、当前文件、成功/失败数量和 FFmpeg 日志会实时更新。
6. “停止”会终止当前 FFmpeg 子进程；窗口退出时也会取消并等待工作线程结束。失败一个视频不会退出整个队列。

支持 MP4 / MOV / MKV / AVI / WebM / M4V，也支持 TS / MTS / M2TS / FLV / WMV。输出统一为 MP4。

## 尺寸模式

| 模式 | 行为 |
|---|---|
| 拉伸尺寸 | 强制缩放到指定宽高，不保持比例 |
| 等比例缩放 + 黑色背景 | 完整视频居中，剩余区域补黑 |
| 等比例缩放 + 模糊背景 | 同源视频背景铺满、裁切、高斯模糊；前景完整、等比、居中 |
| 等比例铺满 + 居中裁切 | 等比放大覆盖画布，从中心裁掉多余区域 |

**1920×1080 → 720×1080** 推荐选择模糊背景。前景约 720×404（为 4:2:0 编码取偶数），位于画布中心；背景覆盖整个 720×1080，不添加黑边。非方形像素源先根据 SAR 归一化，输出 SAR 为 1:1。

模糊背景滤镜结构：

```text
原视频 → 校正 SAR → 镜像/翻转 → split
  背景：等比放大 → 中心 crop → 半分辨率 → gblur → 输出分辨率
  前景：等比缩小完整显示（Lanczos）
背景 + 前景 → overlay 居中 → 色彩/噪点/Logo/文字 → 编码
```

高斯强度默认 24，背景倍率默认 1，可在【模糊背景】调整。背景降采样可减少模糊计算量。

## 编码和性能

启动时先查询 FFmpeg 编码器，再对每个候选编码器执行真实试编码。不会仅凭 FFmpeg 列出编码器或存在 nvidia-smi 就认定可用。

自动选择顺序按 H.264/H.265 分开：

```text
h264_nvenc / hevc_nvenc
    → h264_qsv / hevc_qsv
    → h264_amf / hevc_amf
    → libx264 / libx265
```

手动可选 NVIDIA / Intel / AMD / CPU；手动选择不可用编码器会报错。自动模式在实际任务的编码器失败后会继续尝试下一可用编码器，日志记录真实错误。

质量 0～51：通常越小质量越高，默认 22。码率填 0 使用 CQ / CRF / QP 质量模式，填正数（kbps）使用码率模式。

| 后端 | 质量模式 | preset |
|---|---|---|
| NVENC | VBR + CQ | p1～p7，auto = p4 |
| QSV | global_quality | veryfast / faster / fast / medium / slow / slower / veryslow，auto = medium |
| AMF | CQP，I/P QP | speed / balanced / quality，auto = balanced |
| CPU | CRF | ultrafast～veryslow，auto = veryfast |

推荐保留 preset 为 auto，尤其在自动编码器模式。FPS 填 0 保留输入帧时间线，包括 VFR；自定义 FPS 通过 FFmpeg fps 滤镜完成，不修改音频速度，也不独立重置音视频起始时间。

目前 scale / gblur / overlay / drawtext 使用 FFmpeg 软件滤镜，GPU 负责硬件编码。未开启“硬件解码 → 下载到 CPU → 再上传”的额外路径，也未声称整条滤镜链零拷贝。程序不保证所有设备上都达到相同加速比例，实际速度由视频分辨率、滤镜、显卡和磁盘决定。

FFmpeg 和显卡驱动必须兼容：例如较新的 FFmpeg 可能要求更高 NVENC API。检测日志会显示 API / 驱动错误，此时选择与驱动兼容的 FFmpeg 或更新驱动。CI 使用版本固定的 FFmpeg 7.1 essentials，发布时可替换为兼容硬件的更新构建。

## 音频、输出和文件安全

默认保留所有原音轨，先 `-c:a copy`；复制失败后重试 AAC 192 kbps。静音视频也支持；取消“保留音频”则不输出音轨。

默认不覆盖：遇到已存在或同名不同扩展的视频，会添加 `_handled_1` 等编号。递归输出保留子目录结构；输出子目录位于输入目录中时会排除它，避免反复处理输出。

勾选“覆盖同名输出 / 原 MP4”后，只有完整编码成功才替换目标文件。输入和输出目录相同且源是 MP4 时允许安全替换原文件；其他源容器仍输出 MP4，不会伪装成 MOV/MKV。失败或停止会删除本次未完成的临时文件，保留原文件和已完成结果。

移除 metadata 使用 `-map_metadata -1 -map_chapters -1`，去掉用户标签和章节；MP4 必需结构、编码器/容器自动产生的基础标记不等于用户 metadata。

## 画面与水印

- 水平镜像、垂直翻转。
- 亮度 -1～1、对比度 0～3、饱和度 0～3、噪点 0～20。
- PNG/JPG 叠图，保留原 PNG Alpha；宽度百分比、透明度、旋转角度。
- 图片/文字位置：左上、右上、左下、右下、居中、自定义 X/Y。常见位置边距 24 像素，自定义坐标是输出画布中的像素坐标。
- 中文/多行文字、自选 TTF/TTC/OTF、字号、颜色和 Alpha。默认 Windows 微软雅黑，字体必须包含所需字形。
- 字体颜色支持 white / black / red / green / blue / yellow / #RRGGBB。

文字写入 UTF-8 文件，以 `expansion=none` 交给 drawtext；字体复制到独立临时工作目录，避免中文路径、引号、冒号、百分号和多层滤镜转义问题。外部进程通过 `CreateProcessW` 和独立参数列表执行，不使用 cmd / shell 拼接。

文字旋转暂未加入；图片 Logo 支持旋转。当前输出为 8-bit 4:2:0，不包含专用 HDR 色调映射或字幕烧录功能。

## 源码与编译

```text
src/main.cpp        Win32 中文界面、五个标签页和线程事件
src/engine.h/.cpp   扫描、校验、滤镜、编码器、子进程、批处理
src/cli.cpp         共享核心的命令行入口
src/app.rc          manifest / 文件版本资源
CMakeLists.txt      C++17、x64、静态运行库
scripts/package.py  bin 打包与 DLL 递归依赖检查
scripts/acquire_ffmpeg.py  下载固定版本 FFmpeg
.tests/             测试（实际目录为 tests/）
```

旧 `VideoBatchGPU.cpp` 保留作原始源码参考，已经不参与编译；所有正式输出名称为 SimpleVideoHandle。

Visual Studio 2022，安装“使用 C++ 的桌面开发”和 Windows SDK：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python scripts/package.py --build build --dest bin --ffmpeg-dir "C:/Program Files/ffmpeg/bin"
```

也可以运行 `python scripts/build_local.py` 或 `build_windows.bat`。开发脚本需要 Python 3.10+，最终 GUI 无此依赖。

本机 SDK 版本自动选择异常时可显式指定：

```powershell
python scripts/build_local.py --build build-release --sdk 10.0.19041.0
```

MinGW-w64 x64 也支持：

```powershell
python scripts/build_local.py --build build-mingw --mingw
python scripts/package.py --build build-mingw --dest bin --ffmpeg-dir "C:/Program Files/ffmpeg/bin"
```

## 测试

```powershell
python tests/integration.py --cli bin/SimpleVideoHandleCLI.exe
# 有可用 NVIDIA 编码器时增加实际 GPU 转码测试：
python tests/integration.py --cli bin/SimpleVideoHandleCLI.exe --gpu
```

测试用 Python 仅生成输入并检查 C++ 程序输出；滤镜和编码仍由正式处理核心调用 FFmpeg。

覆盖四个重点模糊背景尺寸案例、严格尺寸/SAR、独立前景参考对比、背景平滑程度、边角无补黑、原音频包哈希、音视频时间差、其他缩放模式、FPS、静音、中文字/Alpha/旋转、AAC 回退、错误文件继续批处理、递归/冲突、原文件安全替换、停止清理、无效参数和 H.265。

CLI 示例：

```powershell
bin/SimpleVideoHandleCLI.exe --input "D:/视频" --output "D:/视频输出" --recursive --width 720 --height 1080 --mode blur --encoder auto --codec h264
bin/SimpleVideoHandleCLI.exe --detect
bin/SimpleVideoHandleCLI.exe --help
```

## GitHub Actions

`.github/workflows/build-windows.yml`：windows-latest / Visual Studio 2022 / MSVC / x64 / Release。

main 上传后或手动 workflow_dispatch 触发，流程为 checkout → configure → build → CTest → 下载/使用 FFmpeg → 整理 bin / 补 DLL → 视频集成测试 → upload artifact。

Artifact 名为 **SimpleVideoHandle-Windows-x64**，包含 `SimpleVideoHandle.exe`、FFmpeg 两个程序、README、依赖报告和测试报告。任何编译、测试、打包依赖缺失都会导致失败，禁止“空 Artifact 成功”。

代码上传和所有 Git 操作由仓库所有者处理。本地编译/测试通过不代表尚未触发的远端 Actions 已通过。

参考：[FFmpeg 滤镜文档](https://ffmpeg.org/ffmpeg-filters.html)、[FFmpeg 命令行文档](https://ffmpeg.org/ffmpeg.html)、[Windows FFmpeg 构建与源码](https://www.gyan.dev/ffmpeg/builds/)。
