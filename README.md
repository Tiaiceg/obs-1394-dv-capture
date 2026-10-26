# obs-dv-capture

OBS Studio 采集源插件，用于通过 **IEEE-1394（FireWire/火线）** 采集老式磁带 DV 摄像机（如 Panasonic AG-DVC180 等“Microsoft DV Camera and VCR”设备），在 OBS 中直接得到 **DVSD 解码后的 YUY2 画面和内嵌 PCM 音频**。

## 为什么需要它

DV 摄像机通过 1394 输出的是 **DVSD（dvsd）压缩流**，而不是未压缩画面。系统需要在 DirectShow 滤镜图中自动串联：

```
DV 摄像机 (Microsoft DV Camera and VCR)
        │
   DV Splitter（系统自带）
        ├──────────────┐
 DV Video Decoder      音频 PCM
 (qdvd.dll, 系统自带)    │
        │               │
   Sample Grabber   Sample Grabber
        │               │
        └──────► OBS（视频 YUY2 / 音频 16bit PCM）
```

OBS 内置的“视频采集设备”源在某些 DV 设备上不能正确插入 DV Splitter / DV Video Decoder，滤镜图只拿到未解码的 DVSD 流，于是表现为黑屏（PotPlayer 能播、OBS 黑屏）。本插件使用 `ICaptureGraphBuilder2::RenderStream` 构建完整滤镜图，强制完成解码后再把帧交给 OBS。

## 功能

- 自动枚举 DirectShow 视频输入设备
- 自动构建 DV 解码链路（PAL 720×576@25 / NTSC 720×480@29.97 自适应）
- 输出 YUY2 画面（BT.601，Limited Range）
- 内嵌 16bit PCM 音频（32k/44.1k/48k，单/立体声自适应）
- 设备丢失自动记录日志
- 无第三方运行时依赖（仅使用 Windows 自带组件，MSVC 运行时静态链接）

## 安装

> 完整图文步骤见压缩包内的 `INSTALL.txt`。

1. 从 [Releases](https://github.com/REPLACE_ME/obs-dv-capture/releases) 下载 `obs-dv-capture-*-windows-x64.zip`。
2. 将压缩包中的 `obs-plugins` 与 `data` 两个文件夹合并到 OBS 安装目录（通常为 `C:\Program Files\obs-studio`）。
3. 重启 OBS，在“来源”中添加 **DV/1394 FireWire Capture**。

### 必需的系统准备

1. 1394 控制器必须使用 **Legacy 旧版 OHCI 驱动**：
   设备管理器 → IEEE 1394 总线主控制器 → 右键 TI 控制器 → 更新驱动 → 浏览我的计算机 → 让我选取 → 取消勾选“显示兼容硬件” → 厂商选 Microsoft → 选 **1394 OHCI Compliant Host Controller (Legacy)** → 重启。
2. 摄像机开机并置于 **CAMERA** 档（回放磁带用 VCR 档）。
3. 启动 OBS 前**关闭** PotPlayer / AMCap / WinDV 等所有占用 1394 设备的程序（DV 设备独占）。

## 从源码构建

需要 Windows + Visual Studio 2022（含 C++ 桌面开发）、CMake ≥ 3.16。

```bat
git clone https://github.com/REPLACE_ME/obs-dv-capture.git
cd obs-dv-capture
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target obs-dv-capture
```

首次配置会自动拉取 OBS 30.1.2 源码并只编译 libobs（无需 Qt），产物位于 `build/Release/obs-dv-capture.dll`。

也可直接在 GitHub Actions 中手动触发 **Build Windows plugin** 工作流，从 Artifacts 下载编译好的 zip。

## 验证清单（真机）

| # | 检查项 | 预期结果 |
|---|--------|----------|
| 1 | 设备管理器 1394 控制器驱动详细信息 | 含 `1394bus.sys`、`ohci1394.sys`（Legacy） |
| 2 | OBS 添加源，设备下拉列表 | 能看到 “Microsoft DV Camera and VCR” |
| 3 | 激活源后画面区 | 出现实时 YUY2 画面，无黑屏 |
| 4 | 右键源 → 属性 → 分辨率 | 720×576（PAL）/ 720×480（NTSC） |
| 5 | OBS 混音器 | 出现 DV 摄像机音频电平 |
| 6 | 日志（帮助 → 日志文件 → 查看当前日志） | 无 `[obs-dv-capture]` 警告 |
| 7 | 拔插 1394 线 | 日志记录 `DV device lost`，重新插回后重新激活源可恢复 |

## 故障排查

- **黑屏，日志 `failed to start DV capture ... device not found`**：设备被 PotPlayer 等占用，或未切换到 Legacy 1394 驱动。
- **`Cannot build DV video graph (DV decoder missing?)`**：系统 DV 解码器损坏，安装 [K-Lite Codec Pack](https://codecguide.com/) 修复系统 DirectShow 组件，或执行 `sfc /scannow`。
- **有画面无声音**：确认摄像机菜单中 1394 音频输出开启、属性中勾选了“Capture embedded DV audio”；部分设备在 VCR 档位才有音频。
- **列表里没有设备**：点击“Refresh device list”；仍无设备说明系统未识别 1394 摄像机（检查线材、驱动、摄像机档位）。

## 许可证

GPL-2.0-or-later（与 libobs 兼容）。
