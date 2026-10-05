# 第三方程序

SimpleVideoHandle 通过独立 Windows 子进程调用 FFmpeg / ffprobe，没有链接 libav*。

FFmpeg 是独立第三方程序。Gyan essentials/full 构建采用 GPLv3，包含 libx264 / libx265 等库。分发时请保留 `licenses/FFmpeg/` 内的上游许可证及说明。

本工程构建流程的固定构建：
- FFmpeg 7.1 essentials Windows x64
- 二进制及版本信息：https://github.com/GyanD/codexffmpeg/releases/tag/7.1
- FFmpeg 对应源码：https://github.com/FFmpeg/FFmpeg/tree/n7.1
- 构建提供方、配置和包含库说明：https://www.gyan.dev/ffmpeg/builds/
- FFmpeg 官方源码和许可信息：https://ffmpeg.org/download.html / https://ffmpeg.org/legal.html

实际打包版本以 `ffmpeg.exe -version`、上游 README 和打包校验值为准；用户可用兼容构建替换程序目录中的 ffmpeg.exe 与 ffprobe.exe。

Windows 系统 DLL、显卡驱动和 Windows 字体不随本程序复制分发。字体由用户选择，文字功能默认引用本机 Windows 安装的微软雅黑。
