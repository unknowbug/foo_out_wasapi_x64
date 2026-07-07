// wasapi_output.h - WASAPI exclusive-mode output implementation
#pragma once

#include "stdafx.h"

static const size_t kMaxDevices = 64;

// Push mode module GUID: {6E8A97A8-0B8A-4A8C-A8B0-8F1A8E8F8A10}
static const GUID guid_push_module =
{ 0x6e8a97a8, 0xb8a, 0x4a8c, { 0xa8, 0xb0, 0x8f, 0x1a, 0x8e, 0x8f, 0x8a, 0x10 } };

// Event mode module GUID: {6E8A97A8-0B8A-4A8C-A8B0-8F1A8E8F8A11}
static const GUID guid_event_module =
{ 0x6e8a97a8, 0xb8a, 0x4a8c, { 0xa8, 0xb0, 0x8f, 0x1a, 0x8e, 0x8f, 0x8a, 0x11 } };

// Device GUID namespace base: {F00DCAFE-0000-0000-0000-000000000000}
static const GUID guid_device_ns =
{ 0xf00dcafe, 0x0, 0x0, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };

class wasapi_output : public output_impl
{
public:
    wasapi_output(const GUID & p_device, double p_buffer_length, bool p_dither, t_uint32 p_bitdepth);
    ~wasapi_output();

    // statics shared by both modes
    static void g_enum_devices(output_device_enum_callback & p_callback);
    static bool g_needs_bitdepth_config();
    static bool g_needs_dither_config();
    static bool g_supports_multiple_streams();
    static bool g_is_low_latency();
    static bool g_is_high_latency();
    static bool g_needs_device_list_prefixes();
    static t_uint32 g_extra_flags();
    static bool g_advanced_settings_query();

protected:
    void open(audio_chunk::spec_t const & p_spec) override;
    void write(const audio_chunk & p_data) override;
    t_size can_write_samples() override;
    t_size get_latency_samples() override;
    void on_update() override;
    void on_flush() override;
    void on_force_play() override;
    void pause(bool p_state) override;
    void volume_set(double p_val) override;
    bool is_progressing() override;
    pfc::eventHandle_t get_trigger_event() override;

    bool m_event_driven = false;

private:
    void cleanup();
    bool find_device(IMMDeviceEnumerator *, const GUID &, IMMDevice **);

    bool m_initialized = false;
    bool m_paused = false;
    bool m_is_bluetooth = false;
    bool m_dither;
    bool m_is_float = false;
    double m_current_volume = 1.0;
    t_uint32 m_bitdepth;
    UINT32 m_buffer_frames = 0;
    UINT32 m_padding = 0;
    double m_buffer_length;
    GUID m_device_guid;
    audio_chunk::spec_t m_current_spec;

    IMMDeviceEnumerator * m_enumerator = nullptr;
    IMMDevice * m_device = nullptr;
    IAudioClient * m_audio_client = nullptr;
    IAudioRenderClient * m_render_client = nullptr;
    IAudioEndpointVolume * m_endpoint_volume = nullptr;
    HANDLE m_event_handle = nullptr;

    pfc::array_t<audio_sample> m_volume_buffer;
};

// --- Push variant ---
class wasapi_output_push : public wasapi_output {
public:
    wasapi_output_push(const GUID & d, double b, bool di, t_uint32 bp)
        : wasapi_output(d, b, di, bp) { m_event_driven = false; }
    static GUID g_get_guid() { return guid_push_module; }
    static const char * g_get_name() { return "WASAPI (exclusive) - Push"; }
};

// --- Event variant ---
class wasapi_output_event : public wasapi_output {
public:
    wasapi_output_event(const GUID & d, double b, bool di, t_uint32 bp)
        : wasapi_output(d, b, di, bp) { m_event_driven = true; }
    static GUID g_get_guid() { return guid_event_module; }
    static const char * g_get_name() { return "WASAPI (exclusive) - Event"; }
};

static output_factory_t<wasapi_output_push>   g_factory_push;
static output_factory_t<wasapi_output_event>  g_factory_event;
