# OBS HEVC-FLV Source

[简体中文](README.md) | **English**

This is a native Windows and macOS OBS input-source plugin for HTTP-FLV streams
whose video tag is HEVC/H.265 (`CodecID = 12`). It bypasses the incompatibility
that causes OBS's ordinary Media Source to play AAC audio while showing a black
video frame.

## What it does

- Pulls `http://` and `https://` FLV URLs with WinHTTP on Windows and the
  system CFNetwork HTTP stream on macOS.
- Parses FLV tags itself, including HEVC sequence headers (`hvcC`) and AAC
  sequence headers.
- Uses the FFmpeg libraries supplied with an OBS SDK/build to decode HEVC/AAC.
- Converts decoded video to BGRA and sends raw video/audio frames to OBS.
- Reconnects after the configured delay when the source closes or fails.

It is intentionally a source plugin: add it through **Sources → Add → HEVC FLV
Stream**, rather than through OBS's Media Source.

## Install a prebuilt release

Download the package matching your operating system from the
[latest GitHub Release](https://github.com/wtnl-zhi/obs-hevc-flv-source/releases/latest),
then quit OBS before installing it.

### Windows x64

1. Extract `obs-hevc-flv-source-windows-x64.zip` without flattening its
   `obs-hevc-flv-source` folder.
2. Copy that folder to `C:\ProgramData\obs-studio\plugins\`.
3. Restart OBS and select **Sources → Add → HEVC FLV Stream**.

The final directory should contain
`C:\ProgramData\obs-studio\plugins\obs-hevc-flv-source\bin\64bit\obs-hevc-flv-source.dll`.

### macOS (Apple Silicon or Intel)

1. Extract `obs-hevc-flv-source-macos-universal.zip`.
2. Copy `obs-hevc-flv-source.plugin` to
   `~/Library/Application Support/obs-studio/plugins/`.
3. Restart OBS and select **Sources → Add → HEVC FLV Stream**.

If macOS prevents an unsigned local plugin from loading, open the bundle once
from Finder or re-sign it with your own Apple Development identity.

## Build

You need an OBS Studio development build/dependency tree that exports `libobs`
and the FFmpeg CMake packages. The plugin and OBS must use compatible FFmpeg
DLL/framework major versions.

### Windows x64

Install Visual Studio 2022 (Desktop C++), CMake 3.28+, and Ninja. Build from an
x64 Native Tools command prompt:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo `
  -DCMAKE_PREFIX_PATH="C:\path\to\obs-build;C:\path\to\obs-deps"
cmake --build build
cmake --install build --prefix package
```

The generated layout is:

```text
package/
  obs-hevc-flv-source/bin/64bit/obs-hevc-flv-source.dll
  obs-hevc-flv-source/data/locale/en-US.ini
```

Copy the `obs-hevc-flv-source` folder into
`C:\ProgramData\obs-studio\plugins\`, then restart OBS.

### macOS (Apple Silicon or Intel)

Install Xcode, CMake 3.28+, Ninja, and matching universal `libobs`/FFmpeg
development dependencies. Build a universal bundle with:

```zsh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_PREFIX_PATH="/path/to/obs-build;/path/to/obs-deps"
cmake --build build
cmake --install build --prefix "$HOME/Library/Application Support/obs-studio/plugins"
```

This installs `obs-hevc-flv-source.plugin` in OBS's per-user plugin directory.
Restart OBS after installing. If OBS is running from a signed app bundle and
macOS blocks an unsigned local plugin, build with your Apple Development signing
identity and sign the produced `.plugin` bundle before loading it.

## Use

1. In OBS, select **Sources → Add → HEVC FLV Stream**.
2. Paste the complete signed HTTP-FLV URL.
3. Leave reconnect delay at 1500 ms unless the origin needs a longer interval.
4. Check **Help → Log Files → View Current Log** for `[HEVC FLV]` messages.

Signed CDN URLs expire. When a source stops reconnecting with an HTTP 403/404,
obtain a fresh URL and replace it in the source properties.

## Current scope

The first implementation supports the conventional FLV packet layout used by
the supplied stream: video tag `0x1c`, an HEVC configuration record, and
length-prefixed HEVC access units; AAC uses the usual FLV AAC sequence header.
It does not currently support AV1-FLV, metadata-driven quality switching,
authenticated cookies, or non-AAC audio.
