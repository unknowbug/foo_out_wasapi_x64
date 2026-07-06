# foo_out_wasapi_x64

WASAPI exclusive-mode output plugin for **foobar2000 v2.0+ (64-bit only).**

Provides bit-exact audio output by bypassing the Windows audio mixer.

## Downloads

Pre-built `.fb2k-component` packages are available on the [Releases](https://github.com/NDark/foo_out_wasapi_x64/releases) page.

## Build

### Prerequisites

- Visual Studio 2022 with **Desktop development with C++** workload
- Windows SDK 10.0+
- [foobar2000 SDK 2025-03-07](https://www.foobar2000.org/SDK)

### Setup

```powershell
# Extract SDK to sdk_official/
7z x SDK-2025-03-07.7z -osdk_official
```

### Compile

Open `foo_out_wasapi_x64.sln` in Visual Studio, or build from command line:

```powershell
msbuild foo_out_wasapi_x64.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `Release\foo_out_wasapi_x64\foo_out_wasapi_x64.dll`

## Install

Double-click `foo_out_wasapi_x64.fb2k-component`, or extract the `.zip` inside to:

```
foobar2000\user-components\foo_out_wasapi_x64\
└── foo_out_wasapi_x64.dll
```

Then go to **Preferences → Playback → Output** and select one of:

| Module | Description |
|--------|-------------|
| `WASAPI (exclusive) - Push` | Push mode – compatible with all devices |
| `WASAPI (exclusive) - Event` | Event-driven mode – lower latency, device-dependent |

## How it works

- **Exclusive mode**: takes full control of the audio device, bypasses the Windows mixer
- **Bit-perfect**: no sample rate conversion, no system audio effects
- **Format-strict**: if the device doesn't support the exact audio format, it reports an error rather than silently downsampling
- **Buffer alignment**: automatically aligns buffer duration to the device's period for maximum compatibility

## License

MIT
