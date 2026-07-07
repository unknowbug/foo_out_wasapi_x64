# foo_out_wasapi_x64

WASAPI 独占模式输出插件，适用于 **foobar2000 v2.0+（仅 64-bit）。**

WASAPI exclusive-mode output plugin for **foobar2000 v2.0+ (64-bit only).**

绕过 Windows 混音器，实现比特精确的音频输出。
Provides bit-exact audio output by bypassing the Windows audio mixer.

---

## 安装 Install

双击 `foo_out_wasapi_x64.fb2k-component`，或将内部的 `foo_out_wasapi_x64.dll` 解压到：
Double-click `foo_out_wasapi_x64.fb2k-component`, or extract the DLL to:

```
foobar2000\user-components\foo_out_wasapi_x64\
└── foo_out_wasapi_x64.dll
```

然后进入 **Preferences → Playback → Output**，选择以下之一：
Then go to **Preferences → Playback → Output** and select one of:

| 模块 Module | 说明 Description |
|-------------|-----------------|
| `WASAPI (exclusive) - Push` | 推模式 — 兼容所有设备 / Compatible with all devices |
| `WASAPI (exclusive) - Event` | 事件驱动 — 更低延迟，依赖设备支持 / Lower latency, device-dependent |

---

## 编译 Build

### 前置依赖 Prerequisites

- Visual Studio 2022（勾选 **Desktop development with C++** 工作负载）
- Windows SDK 10.0+
- [foobar2000 SDK 2025-03-07](https://www.foobar2000.org/SDK)（下载后解压到 `sdk_official/`）

### 编译步骤 Steps

```powershell
# 解压 SDK 到 sdk_official/
# Extract SDK to sdk_official/
7z x SDK-2025-03-07.7z -osdk_official

# 编译 Release x64
msbuild foo_out_wasapi_x64.vcxproj /p:Configuration=Release /p:Platform=x64
```

输出产物 Output: `Release\foo_out_wasapi_x64\foo_out_wasapi_x64.dll`

---

## 工作原理 How it works

- **独占模式 Exclusive mode** — 完全控制音频设备，绕过系统混音器
  Takes full control of the audio device, bypasses the Windows mixer
- **比特精确 Bit-perfect** — 不做采样率转换，不经过系统音效
  No sample rate conversion, no system audio effects
- **格式严格 Format-strict** — 硬件不支持当前格式时报错，而非偷偷降采样
  Reports error if device doesn't support the exact format, rather than silently downsampling
- **缓冲区对齐 Buffer alignment** — 自动按设备周期对齐缓冲区时长，保证兼容性
  Automatically aligns buffer duration to the device's period for maximum compatibility

---

## 故障排除 Troubleshooting

### 声音断裂 / 爆音 Stuttering / Crackling

如果播放时出现声音断裂或爆音，请尝试增大 **Preferences → Playback → Output → Buffer length**（缓冲时间长度）：

If you experience audio stuttering or crackling, try increasing the **Buffer length** setting under **Preferences → Playback → Output**.

将缓冲时长从默认的 1000ms 增大到 2000ms~5000ms 通常可以解决。
Increasing from the default 1000ms to 2000ms–5000ms usually resolves the issue.

> **原因**：WASAPI 独占模式下缓冲区由驱动的周期决定，某些设备的周期很短（如 10ms），fb2k 的默认缓冲无法填满。增大缓冲时长让 fb2k 有更多余量。
> **Why**: In exclusive mode the buffer is determined by the device driver's period; some devices use very short periods (e.g. 10ms). A larger buffer gives fb2k more headroom to avoid underruns.

---

## 许可证 License

MIT
