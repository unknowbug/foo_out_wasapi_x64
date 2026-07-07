// wasapi_output.cpp - WASAPI exclusive-mode output
#include "stdafx.h"
#include "wasapi_output.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

// FNV-1a hash of device ID → GUID
static void devid_to_guid(const wchar_t * id, GUID & out)
{
    uint64_t h = 14695981039346656037ULL;
    for (const wchar_t * p = id; *p; ++p) { h ^= (uint64_t)*p; h *= 1099511628211ULL; }
    out.Data1 = guid_device_ns.Data1 ^ (uint32_t)(h);
    out.Data2 = guid_device_ns.Data2 ^ (uint16_t)(h >> 32);
    out.Data3 = guid_device_ns.Data3 ^ (uint16_t)(h >> 48);
    for (int i = 0; i < 8; ++i)
        out.Data4[i] = guid_device_ns.Data4[i] ^ (uint8_t)(h >> (i * 8));
}

template <typename T> static void safe_release(T *& p)
{ if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------------------
// ctor / dtor
// ---------------------------------------------------------------------------

wasapi_output::wasapi_output(const GUID & dev, double buflen,
                              bool dither, t_uint32 bps)
    : m_dither(dither), m_bitdepth(bps ? bps : 16)
    , m_buffer_length(buflen < 0.05 ? 1.0 : buflen)
    , m_device_guid(dev)
{}

wasapi_output::~wasapi_output() { cleanup(); }

// ---------------------------------------------------------------------------
// output_entry statics
// ---------------------------------------------------------------------------

// GUID and name are provided by wrapper classes (push/event)
bool wasapi_output::g_needs_bitdepth_config() { return true; }
bool wasapi_output::g_needs_dither_config() { return true; }
bool wasapi_output::g_supports_multiple_streams() { return false; }
bool wasapi_output::g_is_low_latency() { return true; }
bool wasapi_output::g_is_high_latency() { return false; }
bool wasapi_output::g_needs_device_list_prefixes() { return false; }
t_uint32 wasapi_output::g_extra_flags() { return 0; }
bool wasapi_output::g_advanced_settings_query() { return false; }

void wasapi_output::g_enum_devices(output_device_enum_callback & cb)
{
    IMMDeviceEnumerator * e = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&e)) || !e)
        return;

    IMMDeviceCollection * c = nullptr;
    if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &c)))
        { e->Release(); return; }

    UINT n = 0;
    if (SUCCEEDED(c->GetCount(&n)) && n > 0) {
        if (n > kMaxDevices) n = (UINT)kMaxDevices;
        for (UINT i = 0; i < n; ++i) {
            IMMDevice * d = nullptr;
            if (FAILED(c->Item(i, &d)) || !d) continue;
            LPWSTR id = nullptr;
            if (SUCCEEDED(d->GetId(&id)) && id) {
                GUID guid; devid_to_guid(id, guid);

                IPropertyStore * ps = nullptr;
                pfc::string8 name;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
                    PROPVARIANT v; PropVariantInit(&v);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
                        name = pfc::stringcvt::string_utf8_from_wide(v.pwszVal);
                    PropVariantClear(&v);
                    ps->Release();
                }
                if (name.length() == 0) name = "[WASAPI device]";
                cb.on_device(guid, name, name.length());
                CoTaskMemFree(id);
            }
            d->Release();
        }
    }
    c->Release(); e->Release();
}

// ---------------------------------------------------------------------------
// output_v4
// ---------------------------------------------------------------------------

pfc::eventHandle_t wasapi_output::get_trigger_event()
{
    return (m_event_driven && m_event_handle) ? m_event_handle : pfc::eventInvalid;
}

// ---------------------------------------------------------------------------
// output_impl / output
// ---------------------------------------------------------------------------

void wasapi_output::open(audio_chunk::spec_t const & spec)
{
    cleanup();

    m_current_spec = spec;
    m_paused = false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        throw std::runtime_error("COM init failed");

    // enumerator
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator);
    if (FAILED(hr) || !m_enumerator)
        throw std::runtime_error("No MMDeviceEnumerator");

    // find device
    IMMDevice * dev = nullptr;
    if (!find_device(m_enumerator, m_device_guid, &dev) &&
        FAILED(m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &dev)))
        throw std::runtime_error("No audio device");
    m_device = dev;

    // Detect Bluetooth devices (BTHENUM) to skip hardware volume
    IPropertyStore * ps = nullptr;
    if (SUCCEEDED(m_device->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_EnumeratorName, &pv)) && pv.vt == VT_LPWSTR) {
            m_is_bluetooth = (wcscmp(pv.pwszVal, L"BTHENUM") == 0);
        }
        PropVariantClear(&pv);
        ps->Release();
    }

    // activate client
    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             (void**)&m_audio_client);
    if (FAILED(hr) || !m_audio_client)
        throw std::runtime_error("Activate IAudioClient failed");

    // Get device timing info for buffer alignment
    REFERENCE_TIME devPeriod = 0, devMinPeriod = 0;
    m_audio_client->GetDevicePeriod(&devPeriod, &devMinPeriod);
    if (devPeriod == 0) devPeriod = 100000; // 10ms fallback

    // Build candidate buffer durations (aligned to device period)
    // Use multiples of device period for proper alignment
    pfc::list_t<REFERENCE_TIME> bufDurs;
    auto addDur = [&](REFERENCE_TIME d) {
        if (d > 0) {
            // Align to device period (round up to nearest multiple)
            d = ((d + devPeriod - 1) / devPeriod) * devPeriod;
        }
        if (d > 50000000) d = 50000000; // cap at 5s
        // Avoid duplicates
        for (t_size i = 0; i < bufDurs.get_size(); ++i)
            if (bufDurs[i] == d) return;
        bufDurs.add_item(d);
    };

    // Try user preference, several reasonable durations, and 0 (auto)
    addDur((REFERENCE_TIME)(m_buffer_length * 10000000.0));
    REFERENCE_TIME defaultPeriodMultiple = devPeriod * 50; // ~500ms if period=10ms
    addDur(defaultPeriodMultiple);
    addDur(devPeriod * 20);  // ~200ms
    addDur(devPeriod * 10);  // ~100ms
    addDur(devPeriod * 5);   // ~50ms
    addDur(0);               // auto (last resort)

    // Build format candidates
    struct FmtTry {
        void * ptr;
        uint32_t rate;
        uint32_t bps;
        bool isFloat;
    };
    pfc::list_t<FmtTry> tries;
    auto push = [&](void * p, uint32_t r, uint32_t b, bool f) {
        tries.add_item({ p, r, b, f });
    };

    // Pre-allocate WAVEFORMATEX structs
    WAVEFORMATEX * wfex_storage[8] = {};
    int wfex_count = 0;
    auto new_wfex = [&]() -> WAVEFORMATEX * {
        if (wfex_count >= 8) return nullptr;
        WAVEFORMATEX * w = new WAVEFORMATEX();
        wfex_storage[wfex_count++] = w;
        return w;
    };

    uint32_t ch = m_current_spec.chanCount;
    if (ch == 0) ch = 2;

    // PCM formats
    auto make_pcm = [&](uint32_t rate, uint32_t channels, uint32_t bps) -> WAVEFORMATEX * {
        WAVEFORMATEX * w = new_wfex(); if (!w) return nullptr;
        WORD bpsB = (WORD)(bps / 8);
        w->wFormatTag = WAVE_FORMAT_PCM;
        w->nChannels = (WORD)channels;
        w->nSamplesPerSec = rate;
        w->nAvgBytesPerSec = rate * channels * bpsB;
        w->nBlockAlign = (WORD)(channels * bpsB);
        w->wBitsPerSample = (WORD)bps;
        w->cbSize = 0;
        return w;
    };
    auto make_float = [&](uint32_t rate, uint32_t channels) -> WAVEFORMATEX * {
        WAVEFORMATEX * w = new_wfex(); if (!w) return nullptr;
        w->wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        w->nChannels = (WORD)channels;
        w->nSamplesPerSec = rate;
        w->nAvgBytesPerSec = rate * channels * 4;
        w->nBlockAlign = (WORD)(channels * 4);
        w->wBitsPerSample = 32;
        w->cbSize = 0;
        return w;
    };

    // Only try formats matching the original sample rate and channel count.
    // If the hardware doesn't support it, report an error rather than
    // silently playing with wrong sample rate / channel mapping.
    m_event_driven = false;

    // Build candidates: same rate+channels, just vary bitdepth and float
    uint32_t origRate = m_current_spec.sampleRate;
    uint32_t origCh = m_current_spec.chanCount;
    if (origCh == 0) origCh = 2;

    if (auto * w = make_pcm(origRate, origCh, m_bitdepth)) push(w, origRate, m_bitdepth, false);
    if (auto * w = make_pcm(origRate, origCh, 16)) push(w, origRate, 16, false);
    if (auto * w = make_float(origRate, origCh)) push(w, origRate, 32, true);

    // Try each format with aligned buffer durations
    //pfc::string8 lastFailMsg;
    bool fmt_ok = false;

    for (auto & t : tries) {
        for (int di = 0; di < (int)bufDurs.get_size() && !fmt_ok; ++di) {
            REFERENCE_TIME thisDur = bufDurs[di];

            // Only try the configured event/push mode, not both
            for (int attempt = 0; attempt < 1 && !fmt_ok; ++attempt) {
                DWORD flags = m_event_driven ? AUDCLNT_STREAMFLAGS_EVENTCALLBACK : 0;
                if (thisDur == 0 && m_event_driven) continue;

                safe_release(m_audio_client);
                HRESULT hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                                  nullptr, (void**)&m_audio_client);
                if (FAILED(hr) || !m_audio_client) break;

                HANDLE ev = nullptr;
                if (flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) {
                    ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (ev) m_event_handle = ev; else flags = 0;
                }

                uint32_t bps2 = t.isFloat ? 32 : t.bps;
                WAVEFORMATEX wfxe = {};
                memcpy(&wfxe, t.ptr, sizeof(WAVEFORMATEX));

                hr = m_audio_client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags,
                    thisDur, 0, &wfxe, nullptr);
                if (SUCCEEDED(hr)) {
                    if (ev && FAILED(m_audio_client->SetEventHandle(ev))) {
                        CloseHandle(ev);
                        m_event_handle = nullptr;
                    }
                    m_current_spec = audio_chunk::makeSpec(t.rate, origCh);
                    m_bitdepth = bps2;
                    m_is_float = t.isFloat;
                    fmt_ok = true;
                    break;
                }
                //lastFailMsg.reset();
                //lastFailMsg << "fmt=" << (int)wfxe.wFormatTag
                //    << " rate=" << wfxe.nSamplesPerSec << " bps=" << wfxe.wBitsPerSample
                //    << " ch=" << wfxe.nChannels;

                if (ev) { CloseHandle(ev); m_event_handle = nullptr; }
            }
        }
        if (fmt_ok) break;
    }

    for (int i = 0; i < wfex_count; ++i) delete wfex_storage[i];

    if (!fmt_ok) {
        cleanup();
        throw std::runtime_error("WASAPI Init failed - format not supported");
    }

    // Get the actual buffer size and decide if event mode is safe
    hr = m_audio_client->GetBufferSize(&m_buffer_frames);
    if (FAILED(hr)) { cleanup(); throw std::runtime_error("GetBufferSize"); }

    // If buffer is very small, disable event-driven to avoid underruns
    if (m_event_handle && m_current_spec.sampleRate > 0) {
        double bufMs = (double)m_buffer_frames / (double)m_current_spec.sampleRate * 1000.0;
        if (bufMs < 20.0) {
            // Buffer too small for reliable event-driven mode
            CloseHandle(m_event_handle);
            m_event_handle = nullptr;
            m_event_driven = false;
        }
    }

    hr = m_audio_client->GetService(__uuidof(IAudioRenderClient),
                                      (void**)&m_render_client);
    if (FAILED(hr) || !m_render_client)
        { cleanup(); throw std::runtime_error("Get IAudioRenderClient"); }

    // endpoint volume control (skip for Bluetooth — uses A2DP absolute volume)
    if (!m_is_bluetooth) {
        m_device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                            (void**)&m_endpoint_volume);
    }

    hr = m_audio_client->Start();
    if (FAILED(hr)) { cleanup(); throw std::runtime_error("Start failed"); }

    m_initialized = true;
}

void wasapi_output::write(const audio_chunk & chunk)
{
    if (!m_initialized || !m_render_client) return;

    UINT32 free = m_buffer_frames - m_padding;
    if (free == 0) return;

    UINT32 frames = (UINT32)chunk.get_sample_count();
    if (frames > free) frames = free;
    if (frames == 0) return;

    BYTE * buf = nullptr;
    if (FAILED(m_render_client->GetBuffer(frames, &buf)) || !buf) return;

    UINT32 ch = m_current_spec.chanCount;
    const audio_sample * src = chunk.get_data();
    size_t total = (size_t)frames * ch;

    // Software volume scaling for Bluetooth (no hardware endpoint volume)
    if (m_is_bluetooth && m_current_volume < 1.0) {
        m_volume_buffer.set_size(total);
        for (size_t i = 0; i < total; ++i) {
            m_volume_buffer[i] = src[i] * (audio_sample)m_current_volume;
        }
        src = m_volume_buffer.get_ptr();
    }

    if (m_bitdepth == 32 && m_is_float) {
        memcpy(buf, src, total * sizeof(audio_sample));
    } else if (m_bitdepth == 16) {
        INT16 * dst = (INT16*)buf;
        for (size_t i = 0; i < total; ++i) {
            float s = src[i];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            dst[i] = (INT16)(s * 32767.0f);
        }
    } else if (m_bitdepth == 24) {
        BYTE * dst = buf;
        for (size_t i = 0; i < total; ++i) {
            float s = src[i];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            int32_t smp = (int32_t)(s * 8388607.0f);
            *dst++ = (BYTE)(smp);
            *dst++ = (BYTE)(smp >> 8);
            *dst++ = (BYTE)(smp >> 16);
        }
    } else if (m_bitdepth == 32 && !m_is_float) {
        INT32 * dst = (INT32*)buf;
        for (size_t i = 0; i < total; ++i) {
            float s = src[i];
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            dst[i] = (INT32)(s * 2147483647.0f);
        }
    }

    HRESULT hr = m_render_client->ReleaseBuffer(frames, 0);
    if (SUCCEEDED(hr)) {
        m_padding += frames;
        if (m_padding > m_buffer_frames) m_padding = m_buffer_frames;
    }
}

t_size wasapi_output::can_write_samples()
{
    if (!m_initialized || !m_audio_client) return 0;
    if (FAILED(m_audio_client->GetCurrentPadding(&m_padding))) return 0;
    UINT32 free = m_buffer_frames - m_padding;
    return m_current_spec.chanCount ? free : 0;
}

t_size wasapi_output::get_latency_samples() { return m_padding; }

void wasapi_output::on_update() {}

void wasapi_output::on_flush()
{
    if (!m_initialized) return;
    if (m_audio_client) { m_audio_client->Stop(); m_audio_client->Reset(); }
    m_padding = 0;
}

void wasapi_output::on_force_play() {}

void wasapi_output::pause(bool s)
{
    if (!m_initialized || !m_audio_client) return;
    m_paused = s;
    s ? m_audio_client->Stop() : m_audio_client->Start();
}

void wasapi_output::volume_set(double db)
{
    // Always convert dB to linear scalar and store
    m_current_volume = (db > -96.0) ? powf(10.0f, (float)db / 20.0f) : 0.0f;

    // Non-BT: use hardware endpoint volume
    if (m_endpoint_volume) {
        m_endpoint_volume->SetMasterVolumeLevelScalar((float)m_current_volume, nullptr);
    }
    // BT: volume scaling applied in write()
}

bool wasapi_output::is_progressing()
{
    // After open(): we start the client immediately, so always progressing.
    return m_initialized;
}

// ---------------------------------------------------------------------------
// internal
// ---------------------------------------------------------------------------

void wasapi_output::cleanup()
{
    if (m_audio_client) m_audio_client->Stop();
    safe_release(m_endpoint_volume);
    safe_release(m_render_client);
    safe_release(m_audio_client);
    safe_release(m_device);
    safe_release(m_enumerator);
    if (m_event_handle) { CloseHandle(m_event_handle); m_event_handle = nullptr; }
    m_initialized = false;
    m_padding = 0;
    m_buffer_frames = 0;
}

bool wasapi_output::find_device(IMMDeviceEnumerator * e, const GUID & want, IMMDevice ** out)
{
    IMMDeviceCollection * c = nullptr;
    if (FAILED(e->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &c)) || !c)
        return false;
    UINT n = 0;
    if (FAILED(c->GetCount(&n))) { c->Release(); return false; }
    bool found = false;
    for (UINT i = 0; i < n; ++i) {
        IMMDevice * d = nullptr;
        if (FAILED(c->Item(i, &d)) || !d) continue;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) {
            GUID g; devid_to_guid(id, g);
            CoTaskMemFree(id);
            if (g == want) { *out = d; found = true; break; }
        }
        d->Release();
    }
    c->Release();
    return found;
}

/*
bool wasapi_output::init_audio(const audio_chunk::spec_t & spec)
{
    // Try PCM first, then float
    m_bitdepth = m_bitdepth ? m_bitdepth : 16;

    auto try_depth = [&](uint32_t bps, bool isFloat) -> bool {
        WAVEFORMATEXTENSIBLE fmt = {};
        if (isFloat) {
            fmt = spec.toWFXEX(); // 32-bit float
        } else {
            fmt = spec.toWFXEXWithBPS(bps);
        }
        WAVEFORMATEX * closest = nullptr;
        HRESULT hr = m_audio_client->IsFormatSupported(
            AUDCLNT_SHAREMODE_EXCLUSIVE,
            (WAVEFORMATEX*)&fmt, &closest);
        if (closest) CoTaskMemFree(closest);
        if (hr == S_OK) {
            m_bitdepth = bps;
            m_is_float = isFloat;
            return true;
        }
        return false;
    };

    // Order: preferred depth, then 32f, then 24i, then 16i
    if (try_depth(m_bitdepth, false)) return true;
    if (try_depth(32, true)) return true;
    if (m_bitdepth != 24 && try_depth(24, false)) return true;
    if (m_bitdepth != 16 && try_depth(16, false)) return true;

    // Fallback: try 48k / 44.1k at 16-bit
    struct { uint32_t rate; } fallbacks[] = { {48000}, {44100} };
    for (auto & fb : fallbacks) {
        auto fs = audio_chunk::makeSpec(fb.rate, 2);
        WAVEFORMATEXTENSIBLE f = fs.toWFXEXWithBPS(16);
        WAVEFORMATEX * cp = nullptr;
        if (m_audio_client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                (WAVEFORMATEX*)&f, &cp) == S_OK) {
            if (cp) CoTaskMemFree(cp);
            m_current_spec = fs;
            m_bitdepth = 16;
            m_is_float = false;
            return true;
        }
        if (cp) CoTaskMemFree(cp);
    }
    return false;
}
*/
