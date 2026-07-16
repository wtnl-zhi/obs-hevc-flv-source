# OBS HEVC-FLV Source

**简体中文** | [English](README.en.md)

这是一个原生 Windows 和 macOS OBS 输入源插件，面向 H.264/AVC（`CodecID = 7`）
和 HEVC/H.265（`CodecID = 12`）的 HTTP-FLV 流。它解决了 OBS 普通“媒体源”
可能只能播放 AAC 音频、视频画面黑屏的问题。

## 功能

- Windows 使用 WinHTTP、macOS 使用 libcurl 拉取 `http://` 和 `https://`
  FLV 地址。
- 自行解析 FLV 标签，包括 H.264（`avcC`）、HEVC（`hvcC`）和 AAC 序列头。
- 使用 OBS SDK/构建环境提供的 FFmpeg 解码 H.264、HEVC 和 AAC。
- 将视频转换为 BGRA，并把原始音视频帧输出到 OBS。
- 源站断开或失败时，按配置的延迟自动重连。
- 直接粘贴抖音直播间链接（包括分享短链），自动解析可用的 HTTP-FLV 地址。

这是一个输入源插件：请通过 **来源 → 添加 → HEVC FLV Stream** 添加，不要
使用 OBS 自带的“媒体源”。

## 使用已构建版本

从 [最新 GitHub Release](https://github.com/wtnl-zhi/obs-hevc-flv-source/releases/latest)
下载与操作系统对应的包。安装前请先退出 OBS。

### Windows x64

1. 解压 `obs-hevc-flv-source-windows-x64.zip`，保留其中的
   `obs-hevc-flv-source` 目录层级。
2. 将该目录复制到 `C:\ProgramData\obs-studio\plugins\`。
3. 重启 OBS，并通过 **来源 → 添加 → HEVC FLV Stream** 添加输入源。

最终文件路径应为：
`C:\ProgramData\obs-studio\plugins\obs-hevc-flv-source\bin\64bit\obs-hevc-flv-source.dll`。

### macOS（Apple Silicon 或 Intel）

1. 解压 `obs-hevc-flv-source-macos-universal.zip`。
2. 将 `obs-hevc-flv-source.plugin` 复制到
   `~/Library/Application Support/obs-studio/plugins/`。
3. 重启 OBS，并通过 **来源 → 添加 → HEVC FLV Stream** 添加输入源。

如果 macOS 阻止加载未签名的本地插件，请先在 Finder 中打开一次插件包，或使用自己
的 Apple Development 签名身份重新签名。

## 构建

需要包含 `libobs` 和 FFmpeg CMake 包的 OBS Studio 开发构建/依赖目录。插件与
OBS 必须使用兼容主版本的 FFmpeg DLL 或 Framework。

### Windows x64

安装 Visual Studio 2022（Desktop C++）、CMake 3.28+ 和 Ninja，然后在 x64
Native Tools 命令提示符中运行：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_PREFIX_PATH="C:\path\to\obs-build;C:\path\to\obs-deps"
cmake --build build
cmake --install build --prefix package
```

生成目录结构：

```text
package/
  obs-hevc-flv-source/bin/64bit/obs-hevc-flv-source.dll
  obs-hevc-flv-source/data/locale/en-US.ini
```

将 `obs-hevc-flv-source` 目录复制到
`C:\ProgramData\obs-studio\plugins\`，然后重启 OBS。

### macOS（Apple Silicon 或 Intel）

安装 Xcode、CMake 3.28+、Ninja、可被 CMake 找到的 libcurl，以及对应的通用
`libobs`/FFmpeg 开发依赖。可用以下命令构建通用插件包：

```zsh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_PREFIX_PATH="/path/to/obs-build;/path/to/obs-deps"
cmake --build build
cmake --install build --prefix "$HOME/Library/Application Support/obs-studio/plugins"
```

该命令会将 `obs-hevc-flv-source.plugin` 安装到 OBS 的当前用户插件目录。安装后
重启 OBS。若从已签名的 App Bundle 运行 OBS，且 macOS 阻止加载未签名的本地插件，
请用 Apple Development 签名身份重新构建，并签名生成的 `.plugin` 包。

## 使用方式

1. 在 OBS 中选择 **来源 → 添加 → HEVC FLV Stream**。
2. 粘贴完整、带签名的 HTTP-FLV 地址；或直接粘贴抖音直播间的网页链接/分享短链。
3. 除非源站要求更长间隔，否则保持重连延迟为 1500 ms。
4. 通过 **帮助 → 日志文件 → 查看当前日志** 查找 `[HEVC FLV]` 日志。

CDN 签名地址会过期。当来源持续因 HTTP 403/404 重连失败时，获取新的地址并在来源
属性中替换。

对于公开的抖音直播间，插件会在首次连接和每次重连前重新获取当前的 FLV 地址，因此
不需要手动从浏览器开发者工具复制带签名的地址。直播间未开播、需要登录，或抖音变更
网页接口时，解析可能失败；此时请查看 OBS 日志中的 `[HEVC FLV]` 提示。

在 macOS 上，如果抖音向插件返回验证码中间页，请将同一直播间保持在已打开的 Chrome
标签页中。插件会仅读取该页面的公开脚本状态来取得当前 FLV 地址，不读取 Cookie、
本地存储、密码或账号数据。macOS 可能会询问是否允许 OBS 控制 Chrome；允许后无需再
手动从浏览器抓取推流地址。

## 当前范围

首个实现支持常规 FLV 包格式：H.264 视频标签 `0x17`、HEVC 视频标签 `0x1c`、
对应的配置记录和长度前缀访问单元，以及常规 FLV AAC 序列头。当前不支持 AV1-FLV、
通过元数据切换清晰度、Cookie 鉴权或非 AAC 音频。抖音链接解析仅面向无需登录即可访问
的公开直播间。
