# 验证记录

## 一、云端已完成的验证（Linux 云电脑，MinGW-w64 交叉编译）

云电脑没有 1394 硬件、没有 Windows 运行环境，无法验证“实际出画面”，但完成了以下编译级验证：

| 验证项 | 方法 | 结果 |
|---|---|---|
| DirectShow 采集引擎编译 | `x86_64-w64-mingw32-g++-posix -std=c++17 -Wall` | 通过 |
| DirectShow 采集引擎链接 | 链接为真实 Windows PE 程序，`-lstrmiids -lole32 -loleaut32` | 通过，全部 GUID/COM 符号解析 |
| OBS 接入层编译 | 使用真实 **OBS 30.1.2 官方 libobs 头文件** | 通过 |
| 插件 DLL 链接 | 链接为 `obs-dv-capture.dll`（libobs 部分用空桩，仅验证符号与模块结构） | 通过 |
| 模块导出表检查 | `objdump` 检查导出 | 正确导出 `obs_module_load` / `obs_module_unload` / `obs_module_ver` / `obs_module_text` 等 OBS 模块入口 |
| DLL 运行时依赖 | `objdump -p` | 仅依赖系统自带 `kernel32/ole32/oleaut32`，无第三方运行时 |

交叉编译过程中发现并修复的真实问题：

1. `WideCharToMultiByte` 第二次调用缓冲区长度参数误传 `nullptr`（应为长度）。
2. MinGW/旧 SDK 缺少 `IID_PPV_ARG`、`CLSID_NullRenderer`、`ISampleGrabber` 定义——改为显式 GUID 与手动声明（也保证了对老开发环境的兼容性）。
3. OBS 30.1.2 的 `obs_source_frame` 没有 `colorspace/range` 字段——改用 `video_format_get_parameters(VIDEO_CS_601, VIDEO_RANGE_PARTIAL, ...)` 与 `full_range`，与 30.x 版本兼容。
4. 本地辅助函数名 `TEXT()` 与 Windows 头文件 `TEXT` 宏冲突——重命名为 `Tr()`。
5. 音频回调在三元表达式中产生不兼容的 lambda 类型——改为显式空 `std::function`。
6. COM 初始化计数、视频帧尺寸校验、启动等待超时等健壮性问题。

> 云电脑构建出的 dll 含空桩，**不能用于真实 OBS**；真实产物以 GitHub Actions 的 Windows/MSVC 构建为准。

## 二、GitHub Actions 真实 MSVC 构建（已通过）

工作流在 `windows-2022` 上使用 Visual Studio 2022 + 官方 OBS 插件模板，链接真实 **OBS 31.1.1 libobs**：

| 验证项 | 结果 |
|---|---|
| MSVC `/W3` + `/permissive-` + `/Zc:preprocessor` 编译（警告即错误 `/WX`） | 通过，无告警 |
| 链接真实 `OBS::libobs` | 通过，生成 `obs-dv-capture.dll` |
| 打包 `obs-dv-capture-0.1.0-windows-x64.zip` | 结构为 `obs-dv-capture/bin/64bit/obs-dv-capture.dll`、`.../obs-dv-capture.pdb`、`obs-dv-capture/data/locale/en-US.ini` |

## 三、真机验证清单（用户 Windows 电脑，1394 摄像机）

## 三、真机验证清单（用户 Windows 电脑，1394 摄像机）

1. 安装 CI 产出的插件后启动 OBS，确认 来源 → 添加 中出现 **DV/1394 FireWire Capture**。
2. 设备下拉中能选到 **Microsoft DV Camera and VCR**。
3. 激活后 30 秒内出画面（PAL 720×576 / NTSC 720×480）。
4. 混音器出现 DV 音频电平。
5. 录制 30 秒后回放，检查 A/V 同步。
6. 查看 `帮助 → 日志文件 → 当前日志`，确认无 `[obs-dv-capture]` 警告。
7. 拔插 1394 线，日志应出现 `DV device lost`；重新插回后重新激活源可恢复。

## 四、真机测试发现与修复

- **rc1 安装路径**：Windows 下 OBS 只扫描 `C:\ProgramData\obs-studio\plugins`（不扫描 `%APPDATA%\Roaming\...`）。已在 README/INSTALL 修正。
- **rc1 源未注册（已修复，发布 rc2）**：在 OBS 32.1.2 真机日志中发现
  `Required value 'get_width' for 'dv1394_capture' not found. obs_register_source failed.`
  原因是源使用 `OBS_SOURCE_VIDEO`（同步源，必须提供 get_width/get_height），但实际通过 `obs_source_output_video()` 异步推送帧。改为 `OBS_SOURCE_ASYNC_VIDEO`（与 OBS 内置视频采集源一致）后，异步源无需 get_width/get_height/video_render。该问题为运行时注册校验，编译与 CI 均无法发现。
- 同时补充了简体中文 `zh-CN.ini` 界面语言。
