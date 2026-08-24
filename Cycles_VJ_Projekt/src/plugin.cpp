#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <vector>
#include <mutex>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("cycles_vj", "en-US")

// =========================================================================
// MIDI MOTOR (Multi-Device, Multi-Channel, Note Decay & CC Router)
// =========================================================================
class MidiCore {
public:
    static MidiCore& instance() { static MidiCore inst; return inst; }

    void init() { refresh_devices(); }
    void shutdown() { close_all(); }

    void refresh_devices() {
        std::lock_guard<std::mutex> lock(mtx);
        UINT num = midiInGetNumDevs();
        if (num == 0 || handles.size() == num) return;

        close_all_internal();
        for (UINT i = 0; i < num && i < 32; ++i) {
            MIDIINCAPSA caps;
            midiInGetDevCapsA(i, &caps, sizeof(caps));
            HMIDIIN h = nullptr;
            MMRESULT res = midiInOpen(&h, i, (DWORD_PTR)cb, (DWORD_PTR)i, CALLBACK_FUNCTION);
            if (res == MMSYSERR_NOERROR) {
                midiInStart(h);
                handles.push_back(h);
            }
        }
    }

    void close_all() {
        std::lock_guard<std::mutex> lock(mtx);
        close_all_internal();
    }

    float get_note(int dev_id, int ch) {
        refresh_devices();
        if (dev_id < 0 || dev_id >= 32) {
            if (ch == 0) {
                float v = global_note.load();
                global_note.store(v * 0.86f);
                return v;
            }
            int c = ch - 1;
            if (c < 0 || c > 15) return 0.0f;
            float max_v = 0.0f;
            for (int d = 0; d < 32; ++d) {
                float v = dev_notes[d][c].load();
                if (v > max_v) max_v = v;
                dev_notes[d][c].store(v * 0.86f);
            }
            return max_v;
        }

        if (ch == 0) {
            float max_v = 0.0f;
            for (int c = 0; c < 16; ++c) {
                float v = dev_notes[dev_id][c].load();
                if (v > max_v) max_v = v;
                dev_notes[dev_id][c].store(v * 0.86f);
            }
            return max_v;
        }

        int c = ch - 1;
        if (c < 0 || c > 15) return 0.0f;
        float v = dev_notes[dev_id][c].load();
        dev_notes[dev_id][c].store(v * 0.86f);
        return v;
    }

    float get_cc(int dev_id, int ch, int cc) {
        refresh_devices();
        if (cc < 0 || cc > 127) return 0.0f;
        if (dev_id < 0 || dev_id >= 32) {
            if (ch == 0) return global_cc[cc].load();
            int c = ch - 1;
            if (c < 0 || c > 15) return 0.0f;
            float max_v = 0.0f;
            for (int d = 0; d < 32; ++d) max_v = (std::max)(max_v, dev_cc[d][c][cc].load());
            return max_v;
        }

        if (ch == 0) {
            float max_v = 0.0f;
            for (int c = 0; c < 16; ++c) max_v = (std::max)(max_v, dev_cc[dev_id][c][cc].load());
            return max_v;
        }
        int c = ch - 1;
        if (c < 0 || c > 15) return 0.0f;
        return dev_cc[dev_id][c][cc].load();
    }

    static void populate_devices(obs_property_t *prop) {
        obs_property_list_clear(prop);
        obs_property_list_add_int(prop, "[AUTO] Minden MIDI Eszkoz", -1);
        UINT num = midiInGetNumDevs();
        for (UINT i = 0; i < num && i < 32; ++i) {
            MIDIINCAPSA caps;
            if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
                obs_property_list_add_int(prop, caps.szPname, (long long)i);
            }
        }
    }

private:
    MidiCore() {
        global_note.store(0.0f);
        for (int k = 0; k < 128; ++k) global_cc[k].store(0.0f);
        for (int d = 0; d < 32; ++d) {
            for (int c = 0; c < 16; ++c) {
                dev_notes[d][c].store(0.0f);
                for (int k = 0; k < 128; ++k) dev_cc[d][c][k].store(0.0f);
            }
        }
    }

    void close_all_internal() {
        for (auto h : handles) {
            if (h) {
                midiInStop(h);
                midiInReset(h);
                midiInClose(h);
            }
        }
        handles.clear();
    }

    static void CALLBACK cb(HMIDIIN, UINT msg, DWORD_PTR dwInstance, DWORD_PTR p1, DWORD_PTR) {
        if (msg == MIM_DATA) {
            int dev_idx = (int)dwInstance;
            if (dev_idx < 0 || dev_idx >= 32) return;

            unsigned char st = (unsigned char)(p1 & 0xFF);
            unsigned char type = st & 0xF0;
            unsigned char ch = st & 0x0F;

            if (type == 0x90) {
                unsigned char vel = (unsigned char)((p1 >> 16) & 0xFF);
                float val = vel / 127.0f;
                MidiCore::instance().dev_notes[dev_idx][ch].store(val);
                MidiCore::instance().global_note.store(val);
            } else if (type == 0x80) {
                MidiCore::instance().dev_notes[dev_idx][ch].store(0.0f);
            } else if (type == 0xB0) {
                unsigned char cc = (unsigned char)((p1 >> 8) & 0xFF);
                unsigned char val = (unsigned char)((p1 >> 16) & 0xFF);
                if (cc < 128) {
                    float f_val = val / 127.0f;
                    MidiCore::instance().dev_cc[dev_idx][ch][cc].store(f_val);
                    MidiCore::instance().global_cc[cc].store(f_val);
                }
            }
        }
    }

    std::mutex mtx;
    std::vector<HMIDIIN> handles;
    std::atomic<float> global_note;
    std::atomic<float> global_cc[128];
    std::atomic<float> dev_notes[32][16];
    std::atomic<float> dev_cc[32][16][128];
};

static obs_source_info reg_f(const char* id, const char* (*n)(void*), void* (*cr)(obs_data_t*, obs_source_t*), void (*ds)(void*), void (*up)(void*, obs_data_t*), void (*def)(obs_data_t*), obs_properties_t* (*pr)(void*), void (*rd)(void*, gs_effect_t*)) {
    obs_source_info info = {};
    info.id = id; info.type = OBS_SOURCE_TYPE_FILTER; info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = n; info.create = cr; info.destroy = ds; info.update = up; info.get_defaults = def; info.get_properties = pr; info.video_render = rd;
    return info;
}

// =========================================================================
// 1. [VIZZable] 2TONR (Hermite Duotone + Chroma Tint)
// =========================================================================
static const char* to_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float3 p_col_a;\nuniform float3 p_col_b;\nuniform float p_thresh;\nuniform float p_smooth;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    float luma = dot(orig.rgb, float3(0.2126, 0.7152, 0.0722));\n"
"    float sm = max(p_smooth * 0.5, 0.0001);\n"
"    float t = smoothstep(saturate(p_thresh - sm), saturate(p_thresh + sm), luma);\n"
"    float3 duo = lerp(p_col_b, p_col_a, t);\n"
"    return lerp(orig, float4(duo, orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct to_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, ra, ga, ba, rb, gb, bb, thresh, smooth, mx; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* to_n(void*) { return "[VIZZable] 2TONR"; }
static void* to_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (to_d*)bzalloc(sizeof(to_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->ra = 1.0f; d->ga = 0.85f; d->ba = 0.2f; d->rb = 0.1f; d->gb = 0.1f; d->bb = 0.8f; d->thresh = 0.5f; d->smooth = 0.5f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(to_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void to_ds(void* d) { auto* x = (to_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void to_up(void* d, obs_data_t* s) {
    auto* x = (to_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->thresh = (float)obs_data_get_double(s, "thresh"); x->smooth = (float)obs_data_get_double(s, "smooth"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void to_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "thresh", 0.5); obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_double(s, "smooth", 0.5); obs_data_set_default_int(s, "cdecay", 17);
    obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21);
    obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmix", 7);
}
static void to_rd(void* d, gs_effect_t*) {
    auto* x = (to_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    vec3 c_a = { (std::clamp)(1.0f - m.get_cc(x->dev, x->ch, x->cshape) + note_hit, 0.0f, 1.0f), (std::clamp)(0.8f + m.get_cc(x->dev, x->ch, x->ccolor), 0.0f, 1.0f), (std::clamp)(m.get_cc(x->dev, x->ch, x->csweep), 0.0f, 1.0f) };
    vec3 c_b = { (std::clamp)(m.get_cc(x->dev, x->ch, x->csweep), 0.0f, 1.0f), (std::clamp)(m.get_cc(x->dev, x->ch, x->cshape) * 0.5f, 0.0f, 1.0f), (std::clamp)(1.0f - m.get_cc(x->dev, x->ch, x->ccolor) + note_hit, 0.0f, 1.0f) };
    gs_effect_set_vec3(gs_effect_get_param_by_name(x->eff, "p_col_a"), &c_a); gs_effect_set_vec3(gs_effect_get_param_by_name(x->eff, "p_col_b"), &c_b);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_thresh"), (std::clamp)(x->thresh + (m.get_cc(x->dev, x->ch, x->cpitch) - 0.5f) + (note_hit * 0.4f), 0.0f, 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_smooth"), (std::clamp)(x->smooth + (m.get_cc(x->dev, x->ch, x->ccontour) * 1.5f), 0.01f, 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* to_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Flash Dinamika", 0.0, 2.0, 0.05);
    obs_properties_add_float_slider(p, "thresh", "Kuszob (Pitch CC16)", 0.0, 1.0, 0.01); obs_properties_add_float_slider(p, "smooth", "Hermite Atmenet (Contour CC21)", 0.01, 2.0, 0.02);
    obs_properties_add_float_slider(p, "mx", "Mix (Vol CC7)", 0.0, 1.0, 0.01);
    return p;
}

// =========================================================================
// 2. [VIZZable] BLURR (Multi-directional Motion Blur)
// =========================================================================
static const char* bl_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_amt;\nuniform float p_rad_blur;\nuniform float p_ang;\nuniform float p_cx;\nuniform float p_cy;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 center = float2(0.5 + p_cx, 0.5 + p_cy);\n"
"    float4 col = float4(0,0,0,0);\n"
"    float rad = p_ang * 0.01745329;\n"
"    float2 dir = float2(cos(rad), sin(rad)) * p_amt * 0.02;\n"
"    [unroll] for (int i = -4; i <= 4; ++i) {\n"
"        float fi = float(i) * 0.25;\n"
"        float2 uv_s = lerp(v.uv + dir * fi, center + (v.uv - center) * (1.0 + fi * p_rad_blur * 0.15), step(0.01, p_rad_blur));\n"
"        col += image.Sample(def_s, uv_s);\n"
"    }\n"
"    col /= 9.0;\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    return lerp(orig, col, p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct bl_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, amt, rad_b, ang, cx, cy, mx; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* bl_n(void*) { return "[VIZZable] BLURR"; }
static void* bl_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (bl_d*)bzalloc(sizeof(bl_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->amt = 1.0f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(bl_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void bl_ds(void* d) { auto* x = (bl_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void bl_up(void* d, obs_data_t* s) {
    auto* x = (bl_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->amt = (float)obs_data_get_double(s, "amt"); x->rad_b = (float)obs_data_get_double(s, "rad_b"); x->ang = (float)obs_data_get_double(s, "ang"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void bl_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "amt", 1.0); obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17);
    obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21);
    obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmix", 7);
}
static void bl_rd(void* d, gs_effect_t*) {
    auto* x = (bl_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_amt"), x->amt + (m.get_cc(x->dev, x->ch, x->ccolor) * 8.0f) + (note_hit * 4.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rad_blur"), (m.get_cc(x->dev, x->ch, x->csweep) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_ang"), (m.get_cc(x->dev, x->ch, x->cshape) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), ((m.get_cc(x->dev, x->ch, x->cpan) - 0.5f) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), ((m.get_cc(x->dev, x->ch, x->cdecay) - 0.5f) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* bl_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Blur Loket", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "amt", "Elmosas (Color CC18)", 0.0, 10.0, 0.1); obs_properties_add_float_slider(p, "mx", "Mix (Vol CC7)", 0.0, 1.0, 0.01);
    return p;
}

// =========================================================================
// 3. [VIZZable] BRCOSR+ (Rec.709 Luma + Rodrigue Matrix)
// =========================================================================
static const char* br_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_b;\nuniform float p_c;\nuniform float p_s;\nuniform float p_gamma;\nuniform float p_hue;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 col = image.Sample(def_s, v.uv);\n"
"    float angle = p_hue * 6.2831853;\n"
"    float3 k = float3(0.57735, 0.57735, 0.57735);\n"
"    float cosA = cos(angle); float sinA = sin(angle);\n"
"    col.rgb = col.rgb * cosA + cross(k, col.rgb) * sinA + k * dot(k, col.rgb) * (1.0 - cosA);\n"
"    float luma = dot(col.rgb, float3(0.2126, 0.7152, 0.0722));\n"
"    col.rgb = lerp(float3(luma, luma, luma), col.rgb, p_s);\n"
"    col.rgb = (col.rgb - 0.5) * p_c + 0.5 + p_b;\n"
"    col.rgb = pow(max(col.rgb, float3(0.0001, 0.0001, 0.0001)), float3(1.0/p_gamma, 1.0/p_gamma, 1.0/p_gamma));\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct br_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, b, c, s, gamma, hue; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* br_n(void*) { return "[VIZZable] BRCOSR+"; }
static void* br_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (br_d*)bzalloc(sizeof(br_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.0f; d->b = 0.0f; d->c = 1.0f; d->s = 1.0f; d->gamma = 1.0f; d->hue = 0.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(br_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void br_ds(void* d) { auto* x = (br_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void br_up(void* d, obs_data_t* s) {
    auto* x = (br_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->b = (float)obs_data_get_double(s, "b"); x->c = (float)obs_data_get_double(s, "c"); x->s = (float)obs_data_get_double(s, "s"); x->gamma = (float)obs_data_get_double(s, "gamma"); x->hue = (float)obs_data_get_double(s, "hue");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void br_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.0);
    obs_data_set_default_double(s, "b", 0.0); obs_data_set_default_double(s, "c", 1.0); obs_data_set_default_double(s, "s", 1.0); obs_data_set_default_double(s, "gamma", 1.0); obs_data_set_default_double(s, "hue", 0.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void br_rd(void* d, gs_effect_t*) {
    auto* x = (br_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_b"), x->b + ((m.get_cc(x->dev, x->ch, x->csweep) - 0.5f) * 2.0f) + (note_hit * 0.6f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_c"), x->c + (m.get_cc(x->dev, x->ch, x->ccontour) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_s"), x->s + (m.get_cc(x->dev, x->ch, x->ccolor) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_gamma"), (std::max)(0.1f, x->gamma + ((m.get_cc(x->dev, x->ch, x->cdecay) - 0.5f) * 3.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_hue"), x->hue + m.get_cc(x->dev, x->ch, x->cshape));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* br_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Flash", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "b", "Fenyero (Sweep CC20)", -1.0, 1.0, 0.02); obs_properties_add_float_slider(p, "c", "Kontraszt (Contour CC21)", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "s", "Telitettseg (Color CC18)", 0.0, 3.0, 0.05); obs_properties_add_float_slider(p, "gamma", "Gamma (Decay CC17)", 0.1, 4.0, 0.05);
    return p;
}

// =========================================================================
// 4. [VIZZable] BREATHR (Morphological Dilate / Erode)
// =========================================================================
static const char* brt_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_width;\nuniform float p_height;\nuniform float p_chroma;\nuniform float p_thresh;\nuniform float p_speed;\nuniform float p_edge;\nuniform float p_mix;\nuniform float p_time;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    float lfo = sin(p_time * p_speed) * 0.5;\n"
"    float2 rad = float2(p_width + lfo, p_height + lfo) * 0.015;\n"
"    float4 col_max = float4(0,0,0,0); float4 col_min = float4(1,1,1,1);\n"
"    float2 offsets[8] = { float2(-rad.x, 0.0), float2(rad.x, 0.0), float2(0.0, -rad.y), float2(0.0, rad.y), float2(-rad.x, -rad.y)*0.707, float2(rad.x, -rad.y)*0.707, float2(-rad.x, rad.y)*0.707, float2(rad.x, rad.y)*0.707 };\n"
"    [unroll] for (int i = 0; i < 8; ++i) {\n"
"        float2 uv_s = v.uv + offsets[i];\n"
"        float r = image.Sample(def_s, uv_s + offsets[i] * p_chroma * 0.5).r;\n"
"        float g = image.Sample(def_s, uv_s).g;\n"
"        float b = image.Sample(def_s, uv_s - offsets[i] * p_chroma * 0.5).b;\n"
"        float4 s_col = float4(r, g, b, 1.0);\n"
"        col_max = max(col_max, s_col); col_min = min(col_min, s_col);\n"
"    }\n"
"    float morph_dir = (rad.x + rad.y);\n"
"    float4 morphed = (morph_dir >= 0.0) ? col_max : col_min;\n"
"    float luma = dot(morphed.rgb, float3(0.2126, 0.7152, 0.0722));\n"
"    if (luma < p_thresh * 0.5) morphed = lerp(orig, morphed, luma / max(p_thresh * 0.5, 0.001));\n"
"    float4 final_col = lerp(orig, morphed, saturate(1.0 + p_edge));\n"
"    return lerp(orig, final_col, p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct brt_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, width, height, chroma, thresh, speed, edge, mx, time; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* brt_n(void*) { return "[VIZZable] BREATHR"; }
static void* brt_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (brt_d*)bzalloc(sizeof(brt_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->width = 0.5f; d->height = 0.5f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(brt_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void brt_ds(void* d) { auto* x = (brt_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void brt_up(void* d, obs_data_t* s) {
    auto* x = (brt_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->width = (float)obs_data_get_double(s, "width"); x->height = (float)obs_data_get_double(s, "height"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void brt_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "width", 0.5); obs_data_set_default_double(s, "height", 0.5); obs_data_set_default_double(s, "mx", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void brt_rd(void* d, gs_effect_t*) {
    auto* x = (brt_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->time += 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_width"), ((m.get_cc(x->dev, x->ch, x->cpitch) - 0.5f) * 2.0f) + (note_hit * 1.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_height"), ((m.get_cc(x->dev, x->ch, x->cdecay) - 0.5f) * 2.0f) + (note_hit * 1.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_chroma"), m.get_cc(x->dev, x->ch, x->ccolor) * 3.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_thresh"), m.get_cc(x->dev, x->ch, x->cshape));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_speed"), m.get_cc(x->dev, x->ch, x->csweep) * 10.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_edge"), m.get_cc(x->dev, x->ch, x->ccontour) * 2.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_time"), x->time);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* brt_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Morph Bump", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "width", "X Dilate/Erode (Pitch CC16)", -1.0, 1.0, 0.02); obs_properties_add_float_slider(p, "height", "Y Dilate/Erode (Decay CC17)", -1.0, 1.0, 0.02);
    obs_properties_add_float_slider(p, "mx", "Mix (Vol CC7)", 0.0, 1.0, 0.01);
    return p;
}

// =========================================================================
// 5. [VIZZable] CLRMAPR (Continuous Sine Solarizer)
// =========================================================================
static const char* cm_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_shift;\nuniform float p_freq;\nuniform float p_phase;\nuniform float p_invert;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    float luma = dot(orig.rgb, float3(0.2126, 0.7152, 0.0722));\n"
"    float a = luma * p_freq * 3.14159 + p_shift * 6.28318;\n"
"    float3 c = float3(sin(a), sin(a + p_phase * 2.094), sin(a + p_phase * 4.188)) * 0.5 + 0.5;\n"
"    if (p_invert > 0.5) c = 1.0 - c;\n"
"    return lerp(orig, float4(c, orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct cm_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, shift, freq, phase, inv, mx; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* cm_n(void*) { return "[VIZZable] CLRMAPR"; }
static void* cm_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (cm_d*)bzalloc(sizeof(cm_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->freq = 2.0f; d->phase = 1.0f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(cm_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void cm_ds(void* d) { auto* x = (cm_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void cm_up(void* d, obs_data_t* s) {
    auto* x = (cm_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->shift = (float)obs_data_get_double(s, "shift"); x->freq = (float)obs_data_get_double(s, "freq"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void cm_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "shift", 0.0); obs_data_set_default_double(s, "freq", 2.0); obs_data_set_default_double(s, "mx", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void cm_rd(void* d, gs_effect_t*) {
    auto* x = (cm_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_shift"), x->shift + m.get_cc(x->dev, x->ch, x->ccolor) + (note_hit * 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_freq"), x->freq + (m.get_cc(x->dev, x->ch, x->cshape) * 16.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_phase"), m.get_cc(x->dev, x->ch, x->csweep) * 2.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_invert"), m.get_cc(x->dev, x->ch, x->ccontour) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* cm_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Color Cycle", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "shift", "Eltolas (Color CC18)", 0.0, 1.0, 0.01); obs_properties_add_float_slider(p, "freq", "Szolarizacio Szeletek (Shape CC19)", 1.0, 20.0, 0.5);
    return p;
}

// =========================================================================
// 6. [VIZZable] CROPR (Soft Edge Letterbox)
// =========================================================================
static const char* cr_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_left;\nuniform float p_right;\nuniform float p_top;\nuniform float p_bottom;\nuniform float p_feather;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float f = max(p_feather, 0.0001);\n"
"    float m_l = smoothstep(p_left, p_left + f, v.uv.x);\n"
"    float m_r = 1.0 - smoothstep(1.0 - p_right - f, 1.0 - p_right, v.uv.x);\n"
"    float m_t = smoothstep(p_top, p_top + f, v.uv.y);\n"
"    float m_b = 1.0 - smoothstep(1.0 - p_bottom - f, 1.0 - p_bottom, v.uv.y);\n"
"    float mask = m_l * m_r * m_t * m_b;\n"
"    return image.Sample(def_s, v.uv) * mask;\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct cr_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, left, right, top, bottom, feather; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* cr_n(void*) { return "[VIZZable] CROPR"; }
static void* cr_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (cr_d*)bzalloc(sizeof(cr_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.0f; d->feather = 0.01f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(cr_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void cr_ds(void* d) { auto* x = (cr_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void cr_up(void* d, obs_data_t* s) {
    auto* x = (cr_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->left = (float)obs_data_get_double(s, "left"); x->right = (float)obs_data_get_double(s, "right"); x->top = (float)obs_data_get_double(s, "top"); x->bottom = (float)obs_data_get_double(s, "bottom"); x->feather = (float)obs_data_get_double(s, "feather");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void cr_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.0);
    obs_data_set_default_double(s, "left", 0.0); obs_data_set_default_double(s, "right", 0.0); obs_data_set_default_double(s, "top", 0.0); obs_data_set_default_double(s, "bottom", 0.0); obs_data_set_default_double(s, "feather", 0.01);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void cr_rd(void* d, gs_effect_t*) {
    auto* x = (cr_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_left"), (std::clamp)(x->left + (m.get_cc(x->dev, x->ch, x->cpitch) * 0.45f) + (note_hit * 0.2f), 0.0f, 0.49f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_right"), (std::clamp)(x->right + (m.get_cc(x->dev, x->ch, x->ccolor) * 0.45f) + (note_hit * 0.2f), 0.0f, 0.49f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_top"), (std::clamp)(x->top + (m.get_cc(x->dev, x->ch, x->cdecay) * 0.45f) + (note_hit * 0.2f), 0.0f, 0.49f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bottom"), (std::clamp)(x->bottom + (m.get_cc(x->dev, x->ch, x->csweep) * 0.45f) + (note_hit * 0.2f), 0.0f, 0.49f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_feather"), (std::max)(0.0001f, x->feather + (m.get_cc(x->dev, x->ch, x->ccontour) * 0.5f)));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* cr_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Cut Pulse", 0.0, 2.0, 0.05);
    obs_properties_add_float_slider(p, "left", "Bal Vagas (Pitch CC16)", 0.0, 0.5, 0.01); obs_properties_add_float_slider(p, "feather", "Lagyitas (Contour CC21)", 0.0, 0.5, 0.01);
    return p;
}

// =========================================================================
// 7. [VIZZable] EXPOSR (EV Exposure + Bloom Bleed)
// =========================================================================
static const char* ex_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_ev;\nuniform float p_bloom;\nuniform float p_thresh;\nuniform float p_temp;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 col = image.Sample(def_s, v.uv);\n"
"    col.rgb *= exp2(p_ev);\n"
"    float3 highlight = max(float3(0,0,0), col.rgb - p_thresh) * p_bloom;\n"
"    col.rgb += highlight;\n"
"    col.r *= (1.0 + p_temp * 0.2); col.b *= (1.0 - p_temp * 0.2);\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct ex_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, ev, bloom, thresh, temp; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* ex_n(void*) { return "[VIZZable] EXPOSR"; }
static void* ex_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (ex_d*)bzalloc(sizeof(ex_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->ev = 0.0f; d->bloom = 1.0f; d->thresh = 0.7f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(ex_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void ex_ds(void* d) { auto* x = (ex_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void ex_up(void* d, obs_data_t* s) {
    auto* x = (ex_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->ev = (float)obs_data_get_double(s, "ev"); x->bloom = (float)obs_data_get_double(s, "bloom"); x->thresh = (float)obs_data_get_double(s, "thresh");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void ex_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "ev", 0.0); obs_data_set_default_double(s, "bloom", 1.0); obs_data_set_default_double(s, "thresh", 0.7);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void ex_rd(void* d, gs_effect_t*) {
    auto* x = (ex_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_ev"), x->ev + ((m.get_cc(x->dev, x->ch, x->ccolor) - 0.5f) * 6.0f) + (note_hit * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bloom"), x->bloom + (m.get_cc(x->dev, x->ch, x->csweep) * 5.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_thresh"), (std::max)(0.0f, x->thresh - (m.get_cc(x->dev, x->ch, x->ccontour) * 0.6f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_temp"), ((m.get_cc(x->dev, x->ch, x->cpitch) - 0.5f) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* ex_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Flash", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "ev", "Expozicio (Color CC18)", -3.0, 5.0, 0.1); obs_properties_add_float_slider(p, "bloom", "Highlight Bloom (Sweep CC20)", 0.0, 5.0, 0.1);
    return p;
}

// =========================================================================
// 8. [VIZZable] FISHEYR (Spherical Lens + Chroma Aberration)
// =========================================================================
static const char* fi_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_dist;\nuniform float p_rad;\nuniform float p_cx;\nuniform float p_cy;\nuniform float p_chroma;\nuniform float p_zoom;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 center = float2(0.5 + p_cx, 0.5 + p_cy);\n"
"    float2 uv = v.uv - center;\n"
"    float r = length(uv);\n"
"    float bind = max(p_rad, 0.01);\n"
"    if (r < bind) {\n"
"        float factor = (p_dist >= 0.0) ? pow(r / bind, 1.0 + p_dist * 2.0) : pow(r / bind, 1.0 / (1.0 - p_dist * 2.0));\n"
"        uv = uv * (factor / max(r / bind, 0.0001));\n"
"    }\n"
"    uv = uv * p_zoom + center;\n"
"    if (p_chroma > 0.01) {\n"
"        float2 off = (v.uv - center) * p_chroma * 0.03;\n"
"        float r_c = image.Sample(def_s, uv + off).r;\n"
"        float g_c = image.Sample(def_s, uv).g;\n"
"        float b_c = image.Sample(def_s, uv - off).b;\n"
"        return float4(r_c, g_c, b_c, 1.0);\n"
"    }\n"
"    return image.Sample(def_s, uv);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct fi_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, dist, rad, cx, cy, chroma, zoom; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* fi_n(void*) { return "[VIZZable] FISHEYR"; }
static void* fi_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (fi_d*)bzalloc(sizeof(fi_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->dist = 0.5f; d->rad = 0.8f; d->zoom = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(fi_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void fi_ds(void* d) { auto* x = (fi_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void fi_up(void* d, obs_data_t* s) {
    auto* x = (fi_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->dist = (float)obs_data_get_double(s, "dist"); x->rad = (float)obs_data_get_double(s, "rad"); x->zoom = (float)obs_data_get_double(s, "zoom");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void fi_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "dist", 0.5); obs_data_set_default_double(s, "rad", 0.8); obs_data_set_default_double(s, "zoom", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void fi_rd(void* d, gs_effect_t*) {
    auto* x = (fi_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_dist"), x->dist + ((m.get_cc(x->dev, x->ch, x->ccolor) - 0.5f) * 4.0f) + (note_hit * 0.8f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rad"), (std::max)(0.05f, x->rad + (m.get_cc(x->dev, x->ch, x->csweep) * 2.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), ((m.get_cc(x->dev, x->ch, x->cpan) - 0.5f) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), ((m.get_cc(x->dev, x->ch, x->cdecay) - 0.5f) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_chroma"), m.get_cc(x->dev, x->ch, x->cshape) * 3.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.1f, x->zoom * (1.0f + m.get_cc(x->dev, x->ch, x->ccontour) * 3.0f)));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* fi_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Bulge", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "dist", "Torzitas (Color CC18)", -2.0, 2.0, 0.05); obs_properties_add_float_slider(p, "zoom", "Zoom (Contour CC21)", 0.1, 5.0, 0.05);
    return p;
}

// =========================================================================
// 9. [VIZZable] HUESHIFTR (Continuous Rainbow Cycling)
// =========================================================================
static const char* hs_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_hue;\nuniform float p_sat;\nuniform float p_luma;\nuniform float p_cycle;\nuniform float p_time;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 col = image.Sample(def_s, v.uv);\n"
"    float angle = (p_hue + p_time * p_cycle) * 6.2831853;\n"
"    float3 k = float3(0.57735, 0.57735, 0.57735);\n"
"    float cosA = cos(angle); float sinA = sin(angle);\n"
"    col.rgb = col.rgb * cosA + cross(k, col.rgb) * sinA + k * dot(k, col.rgb) * (1.0 - cosA);\n"
"    float grey = dot(col.rgb, float3(0.2126, 0.7152, 0.0722));\n"
"    col.rgb = lerp(float3(grey, grey, grey), col.rgb, p_sat) * p_luma;\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct hs_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, hue, sat, luma, cycle, time; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* hs_n(void*) { return "[VIZZable] HUESHIFTR"; }
static void* hs_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (hs_d*)bzalloc(sizeof(hs_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->sat = 1.0f; d->luma = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(hs_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void hs_ds(void* d) { auto* x = (hs_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void hs_up(void* d, obs_data_t* s) {
    auto* x = (hs_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->hue = (float)obs_data_get_double(s, "hue"); x->sat = (float)obs_data_get_double(s, "sat"); x->luma = (float)obs_data_get_double(s, "luma");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void hs_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "hue", 0.0); obs_data_set_default_double(s, "sat", 1.0); obs_data_set_default_double(s, "luma", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void hs_rd(void* d, gs_effect_t*) {
    auto* x = (hs_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->time += 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_hue"), x->hue + m.get_cc(x->dev, x->ch, x->ccolor) + (note_hit * 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_sat"), x->sat + (m.get_cc(x->dev, x->ch, x->csweep) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_luma"), x->luma + (m.get_cc(x->dev, x->ch, x->ccontour) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cycle"), (m.get_cc(x->dev, x->ch, x->cshape) * 5.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_time"), x->time);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* hs_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Hue Jump", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "hue", "Hue (Color CC18)", 0.0, 1.0, 0.01); obs_properties_add_float_slider(p, "sat", "Telitettseg (Sweep CC20)", 0.0, 4.0, 0.05);
    return p;
}

// =========================================================================
// 10. [VIZZable] KALEIDR (Polar Kaleidoscope Mirror)
// =========================================================================
static const char* kl_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_sides;\nuniform float p_rot;\nuniform float p_twist;\nuniform float p_zoom;\nuniform float p_ox;\nuniform float p_oy;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 c = float2(0.5 + p_ox, 0.5 + p_oy);\n"
"    float2 uv = v.uv - c;\n"
"    float seg = 6.2831853 / max(p_sides, 1.0);\n"
"    float r = length(uv);\n"
"    float a = atan2(uv.y, uv.x) + (p_rot * 0.01745329) + (r * p_twist);\n"
"    a = abs(fmod(abs(a), seg) - (seg * 0.5));\n"
"    float2 k_uv = float2(cos(a), sin(a)) * r * max(p_zoom, 0.01) + 0.5;\n"
"    return image.Sample(def_s, k_uv);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct kl_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, sides, rot, twist, zoom, ox, oy; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* kl_n(void*) { return "[VIZZable] KALEIDR"; }
static void* kl_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (kl_d*)bzalloc(sizeof(kl_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->sides = 6.0f; d->zoom = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(kl_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void kl_ds(void* d) { auto* x = (kl_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void kl_up(void* d, obs_data_t* s) {
    auto* x = (kl_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->sides = (float)obs_data_get_double(s, "sides"); x->zoom = (float)obs_data_get_double(s, "zoom");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void kl_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "sides", 6.0); obs_data_set_default_double(s, "zoom", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void kl_rd(void* d, gs_effect_t*) {
    auto* x = (kl_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_sides"), (std::max)(1.0f, x->sides + std::floor(m.get_cc(x->dev, x->ch, x->ccolor) * 31.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rot"), (m.get_cc(x->dev, x->ch, x->cshape) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_twist"), ((m.get_cc(x->dev, x->ch, x->csweep) - 0.5f) * 10.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.05f, x->zoom * (1.0f + m.get_cc(x->dev, x->ch, x->ccontour) * 3.0f) + (note_hit * 0.8f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_ox"), ((m.get_cc(x->dev, x->ch, x->cpan) - 0.5f) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_oy"), ((m.get_cc(x->dev, x->ch, x->cdecay) - 0.5f) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* kl_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Zoom Pump", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "sides", "Szegmensek (Color CC18)", 1.0, 32.0, 1.0); obs_properties_add_float_slider(p, "zoom", "Zoom (Contour CC21)", 0.1, 5.0, 0.05);
    return p;
}

// =========================================================================
// 11. [VIZZable] PINCHR (Bulge Pinch Distortion)
// =========================================================================
static const char* pn_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_pinch;\nuniform float p_rad;\nuniform float p_cx;\nuniform float p_cy;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 center = float2(0.5 + p_cx, 0.5 + p_cy);\n"
"    float2 uv = v.uv - center;\n"
"    float r = length(uv);\n"
"    float bind = max(p_rad, 0.01);\n"
"    if (r < bind) {\n"
"        float f = pow(r / bind, p_pinch);\n"
"        uv = uv * (f / max(r / bind, 0.0001));\n"
"    }\n"
"    return image.Sample(def_s, uv + center);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct pn_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, pinch, rad; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* pn_n(void*) { return "[VIZZable] PINCHR"; }
static void* pn_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (pn_d*)bzalloc(sizeof(pn_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->pinch = 2.0f; d->rad = 0.8f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(pn_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void pn_ds(void* d) { auto* x = (pn_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void pn_up(void* d, obs_data_t* s) {
    auto* x = (pn_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->pinch = (float)obs_data_get_double(s, "pinch"); x->rad = (float)obs_data_get_double(s, "rad");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void pn_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "pinch", 2.0); obs_data_set_default_double(s, "rad", 0.8);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void pn_rd(void* d, gs_effect_t*) {
    auto* x = (pn_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_pinch"), (std::max)(0.1f, x->pinch + (m.get_cc(x->dev, x->ch, x->ccolor) * 6.0f) + (note_hit * 2.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rad"), (std::max)(0.05f, x->rad + (m.get_cc(x->dev, x->ch, x->csweep) * 2.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), ((m.get_cc(x->dev, x->ch, x->cpan) - 0.5f) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), ((m.get_cc(x->dev, x->ch, x->cdecay) - 0.5f) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* pn_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Pinch Bump", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "pinch", "Pinch Erossege (Color CC18)", 0.1, 8.0, 0.1); obs_properties_add_float_slider(p, "rad", "Sugar (Sweep CC20)", 0.1, 3.0, 0.05);
    return p;
}

// =========================================================================
// 12. [VIZZable] PIXEL8R (Quantize & Bit Crusher)
// =========================================================================
static const char* px_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Point; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_res;\nuniform float p_bits;\nuniform float p_grid;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float r = max(p_res, 4.0);\n"
"    float2 p_uv = floor(v.uv * r) / r;\n"
"    float4 col = image.Sample(def_s, p_uv);\n"
"    if (p_bits > 1.0) {\n"
"        float steps = pow(2.0, floor(p_bits));\n"
"        col.rgb = floor(col.rgb * steps) / steps;\n"
"    }\n"
"    float2 cell = frac(v.uv * r);\n"
"    float edge = step(cell.x, p_grid) + step(1.0 - p_grid, cell.x) + step(cell.y, p_grid) + step(1.0 - p_grid, cell.y);\n"
"    col.rgb *= (1.0 - saturate(edge) * 0.75);\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    return lerp(orig, col, p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct px_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, res, bits, grid, mx; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* px_n(void*) { return "[VIZZable] PIXEL8R"; }
static void* px_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (px_d*)bzalloc(sizeof(px_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.0f; d->res = 128.0f; d->bits = 8.0f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(px_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void px_ds(void* d) { auto* x = (px_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void px_up(void* d, obs_data_t* s) {
    auto* x = (px_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->res = (float)obs_data_get_double(s, "res"); x->bits = (float)obs_data_get_double(s, "bits"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void px_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.0);
    obs_data_set_default_double(s, "res", 128.0); obs_data_set_default_double(s, "bits", 8.0); obs_data_set_default_double(s, "mx", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void px_rd(void* d, gs_effect_t*) {
    auto* x = (px_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    float r = (std::max)(4.0f, x->res * (1.0f - (m.get_cc(x->dev, x->ch, x->ccolor) * 0.95f) - (note_hit * 0.8f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_res"), r);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bits"), (std::clamp)(8.0f - (m.get_cc(x->dev, x->ch, x->csweep) * 7.0f), 1.0f, 8.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_grid"), m.get_cc(x->dev, x->ch, x->ccontour) * 0.3f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* px_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Pixel Crush", 0.0, 1.0, 0.02);
    obs_properties_add_float_slider(p, "res", "Felbontas (Color CC18)", 4.0, 512.0, 2.0); obs_properties_add_float_slider(p, "bits", "Bit-depth (Sweep CC20)", 1.0, 8.0, 1.0);
    return p;
}

// =========================================================================
// 13. [VIZZable] RGBR (Chromatic Dispersion & Channel Separation)
// =========================================================================
static const char* rg_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_shift;\nuniform float p_angle;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float rad = p_angle * 0.01745329;\n"
"    float2 dir = float2(cos(rad), sin(rad)) * p_shift * 0.05;\n"
"    float r = image.Sample(def_s, v.uv + dir).r;\n"
"    float g = image.Sample(def_s, v.uv).g;\n"
"    float b = image.Sample(def_s, v.uv - dir).b;\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    return lerp(orig, float4(r, g, b, orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct rg_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, shift, angle, mx; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* rg_n(void*) { return "[VIZZable] RGBR"; }
static void* rg_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (rg_d*)bzalloc(sizeof(rg_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->shift = 0.0f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(rg_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void rg_ds(void* d) { auto* x = (rg_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void rg_up(void* d, obs_data_t* s) {
    auto* x = (rg_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->shift = (float)obs_data_get_double(s, "shift"); x->angle = (float)obs_data_get_double(s, "angle"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void rg_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "shift", 0.0); obs_data_set_default_double(s, "angle", 0.0); obs_data_set_default_double(s, "mx", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void rg_rd(void* d, gs_effect_t*) {
    auto* x = (rg_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_shift"), x->shift + (m.get_cc(x->dev, x->ch, x->ccolor) * 3.0f) + (note_hit * 1.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_angle"), x->angle + (m.get_cc(x->dev, x->ch, x->cshape) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* rg_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note RGB Shift", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "shift", "RGB Eltolas (Color CC18)", 0.0, 3.0, 0.05); obs_properties_add_float_slider(p, "angle", "Irany (Shape CC19)", -180.0, 180.0, 1.0);
    return p;
}

// =========================================================================
// 14. [VIZZable] SCANLINES (CRT Scanlines & VHS Noise)
// =========================================================================
static const char* sc_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_lines;\nuniform float p_dark;\nuniform float p_noise;\nuniform float p_roll;\nuniform float p_time;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float s_line = sin((v.uv.y + p_time * p_roll * 0.05) * p_lines * 3.14159);\n"
"    float4 col = image.Sample(def_s, v.uv);\n"
"    col.rgb *= (1.0 - saturate(s_line * 0.5 + 0.5) * p_dark);\n"
"    float rand_n = frac(sin(dot(v.uv + float2(p_time * 0.1, p_time * 0.2), float2(12.9898, 78.233))) * 43758.5453);\n"
"    col.rgb += (rand_n - 0.5) * p_noise;\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct sc_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, lines, dark, noise, roll, time; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* sc_n(void*) { return "[VIZZable] SCANLINES"; }
static void* sc_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (sc_d*)bzalloc(sizeof(sc_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.0f; d->lines = 240.0f; d->dark = 0.5f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(sc_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void sc_ds(void* d) { auto* x = (sc_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void sc_up(void* d, obs_data_t* s) {
    auto* x = (sc_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->lines = (float)obs_data_get_double(s, "lines"); x->dark = (float)obs_data_get_double(s, "dark");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void sc_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.0);
    obs_data_set_default_double(s, "lines", 240.0); obs_data_set_default_double(s, "dark", 0.5);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void sc_rd(void* d, gs_effect_t*) {
    auto* x = (sc_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->time += 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_lines"), x->lines + (m.get_cc(x->dev, x->ch, x->cpitch) * 600.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_dark"), (std::clamp)(x->dark + (m.get_cc(x->dev, x->ch, x->ccontour) * 0.8f), 0.0f, 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_noise"), (m.get_cc(x->dev, x->ch, x->csweep) * 0.6f) + (note_hit * 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_roll"), ((m.get_cc(x->dev, x->ch, x->cshape) - 0.5f) * 20.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_time"), x->time);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* sc_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Noise Hit", 0.0, 2.0, 0.02);
    obs_properties_add_float_slider(p, "lines", "Sorok (Pitch CC16)", 10.0, 800.0, 5.0); obs_properties_add_float_slider(p, "dark", "Sotetseg (Contour CC21)", 0.0, 1.0, 0.02);
    return p;
}

// =========================================================================
// 15. [VIZZable] SLICR (Stripe Shift Slicer)
// =========================================================================
static const char* sl_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_slice_amt;\nuniform float p_bands;\nuniform float p_vert;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 uv = v.uv;\n"
"    float coord = (p_vert > 0.5) ? uv.x : uv.y;\n"
"    float band_id = floor(coord * max(p_bands, 2.0));\n"
"    float shift = (fmod(band_id, 2.0) == 0.0 ? 1.0 : -1.0) * p_slice_amt * 0.1;\n"
"    if (p_vert > 0.5) uv.y += shift; else uv.x += shift;\n"
"    return image.Sample(def_s, uv);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct sl_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, amt, bands, vert; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* sl_n(void*) { return "[VIZZable] SLICR"; }
static void* sl_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (sl_d*)bzalloc(sizeof(sl_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->bands = 10.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(sl_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void sl_ds(void* d) { auto* x = (sl_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void sl_up(void* d, obs_data_t* s) {
    auto* x = (sl_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->amt = (float)obs_data_get_double(s, "amt"); x->bands = (float)obs_data_get_double(s, "bands");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void sl_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "amt", 0.0); obs_data_set_default_double(s, "bands", 10.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void sl_rd(void* d, gs_effect_t*) {
    auto* x = (sl_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_slice_amt"), x->amt + (m.get_cc(x->dev, x->ch, x->ccolor) * 3.0f) + (note_hit * 1.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bands"), (std::max)(2.0f, x->bands + (m.get_cc(x->dev, x->ch, x->csweep) * 40.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_vert"), m.get_cc(x->dev, x->ch, x->cshape) > 0.5f ? 1.0f : 0.0f);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* sl_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Slicing Hit", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "amt", "Eltolas (Color CC18)", 0.0, 3.0, 0.05); obs_properties_add_float_slider(p, "bands", "Szeletszam (Sweep CC20)", 2.0, 50.0, 1.0);
    return p;
}

// =========================================================================
// 16. [VIZZable] SPRINKLR (Film Grain Sparkle Generator)
// =========================================================================
static const char* sp_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_density;\nuniform float p_speed;\nuniform float p_mono;\nuniform float p_mix;\nuniform float p_time;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    float2 uv = v.uv * 1000.0;\n"
"    float n_r = frac(sin(dot(uv + p_time * p_speed, float2(12.9898, 78.233))) * 43758.5453);\n"
"    float n_g = frac(sin(dot(uv + p_time * p_speed * 1.1, float2(39.346, 11.135))) * 43758.5453);\n"
"    float n_b = frac(sin(dot(uv + p_time * p_speed * 1.2, float2(73.156, 52.235))) * 43758.5453);\n"
"    float3 noise = (p_mono > 0.5) ? float3(n_r, n_r, n_r) : float3(n_r, n_g, n_b);\n"
"    float3 sparkle = step(1.0 - p_density, noise) * noise;\n"
"    return lerp(orig, float4(orig.rgb + sparkle, orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct sp_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, density, speed, mono, mx, time; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* sp_n(void*) { return "[VIZZable] SPRINKLR"; }
static void* sp_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (sp_d*)bzalloc(sizeof(sp_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->density = 0.1f; d->speed = 1.0f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(sp_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void sp_ds(void* d) { auto* x = (sp_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void sp_up(void* d, obs_data_t* s) {
    auto* x = (sp_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->density = (float)obs_data_get_double(s, "density"); x->speed = (float)obs_data_get_double(s, "speed"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void sp_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "density", 0.1); obs_data_set_default_double(s, "speed", 1.0); obs_data_set_default_double(s, "mx", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void sp_rd(void* d, gs_effect_t*) {
    auto* x = (sp_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->time += 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_density"), (std::clamp)(x->density + (m.get_cc(x->dev, x->ch, x->ccolor) * 0.8f) + (note_hit * 0.5f), 0.0f, 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_speed"), x->speed + (m.get_cc(x->dev, x->ch, x->csweep) * 5.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mono"), m.get_cc(x->dev, x->ch, x->cshape) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_time"), x->time);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* sp_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Sparkle", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "density", "Suruseg (Color CC18)", 0.0, 1.0, 0.02); obs_properties_add_float_slider(p, "speed", "Sebesseg (Sweep CC20)", 0.0, 10.0, 0.1);
    return p;
}

// =========================================================================
// 17. [VIZZable] STROBR (Subtractive Invert & RGB Swap)
// =========================================================================
static const char* st_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_inv_r;\nuniform float p_inv_g;\nuniform float p_inv_b;\nuniform float p_flash;\nuniform float p_swap;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    float4 col = orig;\n"
"    if (p_swap > 1.5) col.rgb = col.brg;\n"
"    else if (p_swap > 0.5) col.rgb = col.gbr;\n"
"    col.r = abs(p_inv_r - col.r); col.g = abs(p_inv_g - col.g); col.b = abs(p_inv_b - col.b);\n"
"    col.rgb += p_flash;\n"
"    return lerp(orig, float4(saturate(col.rgb), orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct st_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, ir, ig, ib, flash, swap, mx; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* st_n(void*) { return "[VIZZable] STROBR"; }
static void* st_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (st_d*)bzalloc(sizeof(st_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 1.0f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(st_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void st_ds(void* d) { auto* x = (st_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void st_up(void* d, obs_data_t* s) {
    auto* x = (st_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->flash = (float)obs_data_get_double(s, "flash"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void st_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 1.0);
    obs_data_set_default_double(s, "flash", 0.0); obs_data_set_default_double(s, "mx", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void st_rd(void* d, gs_effect_t*) {
    auto* x = (st_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_inv_r"), m.get_cc(x->dev, x->ch, x->ccolor) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_inv_g"), m.get_cc(x->dev, x->ch, x->csweep) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_inv_b"), m.get_cc(x->dev, x->ch, x->ccontour) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_flash"), x->flash + (note_hit * 1.2f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_swap"), std::floor(m.get_cc(x->dev, x->ch, x->cshape) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* st_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Flash", 0.0, 2.0, 0.05);
    obs_properties_add_float_slider(p, "mx", "Mix (Vol CC7)", 0.0, 1.0, 0.02);
    return p;
}

// =========================================================================
// 18. [VIZZable] TWISTR (Vortex Swirl Warp)
// =========================================================================
static const char* tw_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_angle;\nuniform float p_rad;\nuniform float p_cx;\nuniform float p_cy;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 center = float2(0.5 + p_cx, 0.5 + p_cy);\n"
"    float2 uv = v.uv - center;\n"
"    float r = length(uv);\n"
"    float bind = max(p_rad, 0.01);\n"
"    if (r < bind) {\n"
"        float theta = atan2(uv.y, uv.x) + (p_angle * (1.0 - r / bind) * 3.14159);\n"
"        uv = float2(cos(theta), sin(theta)) * r;\n"
"    }\n"
"    return image.Sample(def_s, uv + center);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct tw_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, angle, rad; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* tw_n(void*) { return "[VIZZable] TWISTR"; }
static void* tw_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (tw_d*)bzalloc(sizeof(tw_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->angle = 2.0f; d->rad = 0.7f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(tw_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void tw_ds(void* d) { auto* x = (tw_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void tw_up(void* d, obs_data_t* s) {
    auto* x = (tw_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->angle = (float)obs_data_get_double(s, "angle"); x->rad = (float)obs_data_get_double(s, "rad");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void tw_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "angle", 2.0); obs_data_set_default_double(s, "rad", 0.7);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void tw_rd(void* d, gs_effect_t*) {
    auto* x = (tw_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_angle"), x->angle + ((m.get_cc(x->dev, x->ch, x->ccolor) - 0.5f) * 10.0f) + (note_hit * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rad"), (std::max)(0.05f, x->rad + (m.get_cc(x->dev, x->ch, x->csweep) * 2.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), ((m.get_cc(x->dev, x->ch, x->cpan) - 0.5f) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), ((m.get_cc(x->dev, x->ch, x->cdecay) - 0.5f) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* tw_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Twist Impulse", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "angle", "Csavaras (Color CC18)", -10.0, 10.0, 0.1); obs_properties_add_float_slider(p, "rad", "Sugar (Sweep CC20)", 0.05, 3.0, 0.05);
    return p;
}

// =========================================================================
// 19. [VIZZable] ZOROPR (Mirror Slit Scan Symmetrical Invert)
// =========================================================================
static const char* zo_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_angle;\nuniform float p_slit;\nuniform float p_zoom;\nuniform float p_cx;\nuniform float p_cy;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 center = float2(0.5 + p_cx, 0.5 + p_cy);\n"
"    float2 uv = v.uv - center;\n"
"    float rad = p_angle * 0.01745329;\n"
"    float cosA = cos(rad); float sinA = sin(rad);\n"
"    float2 r_uv = float2(uv.x * cosA - uv.y * sinA, uv.x * sinA + uv.y * cosA);\n"
"    r_uv.x = abs(r_uv.x) + p_slit * 0.2;\n"
"    float2 out_uv = float2(r_uv.x * cosA + r_uv.y * sinA, -r_uv.x * sinA + r_uv.y * cosA) * p_zoom + center;\n"
"    return image.Sample(def_s, out_uv);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct zo_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, angle, slit, zoom; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* zo_n(void*) { return "[VIZZable] ZOROPR"; }
static void* zo_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (zo_d*)bzalloc(sizeof(zo_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->zoom = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(zo_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void zo_ds(void* d) { auto* x = (zo_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void zo_up(void* d, obs_data_t* s) {
    auto* x = (zo_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->angle = (float)obs_data_get_double(s, "angle"); x->slit = (float)obs_data_get_double(s, "slit"); x->zoom = (float)obs_data_get_double(s, "zoom");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void zo_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "angle", 45.0); obs_data_set_default_double(s, "slit", 0.0); obs_data_set_default_double(s, "zoom", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void zo_rd(void* d, gs_effect_t*) {
    auto* x = (zo_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_angle"), x->angle + (m.get_cc(x->dev, x->ch, x->cshape) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_slit"), x->slit + (m.get_cc(x->dev, x->ch, x->ccolor) * 2.0f) + (note_hit * 0.8f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.1f, x->zoom * (1.0f + m.get_cc(x->dev, x->ch, x->ccontour) * 3.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), ((m.get_cc(x->dev, x->ch, x->cpan) - 0.5f) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), ((m.get_cc(x->dev, x->ch, x->cdecay) - 0.5f) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* zo_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Slit Pulse", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "slit", "Res Szel (Color CC18)", 0.0, 3.0, 0.05); obs_properties_add_float_slider(p, "angle", "Szog (Shape CC19)", -180.0, 180.0, 1.0);
    return p;
}

// =========================================================================
// 20. [VIZZable] DRTYFEEDR (Analog Video Overdrive & Bleed)
// =========================================================================
static const char* df_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_drive;\nuniform float p_bleed;\nuniform float p_zoom;\nuniform float p_rot;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 uv = v.uv - 0.5;\n"
"    float rad = p_rot * 0.01745329;\n"
"    float cosA = cos(rad); float sinA = sin(rad);\n"
"    uv = float2(uv.x * cosA - uv.y * sinA, uv.x * sinA + uv.y * cosA) * p_zoom + 0.5;\n"
"    float4 col = image.Sample(def_s, uv);\n"
"    col.rgb = tanh(col.rgb * (1.0 + p_drive * 3.0));\n"
"    col.r += col.g * p_bleed * 0.3;\n"
"    col.b += col.r * p_bleed * 0.3;\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    return lerp(orig, float4(saturate(col.rgb), orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct df_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, drive, bleed, zoom, rot, mx; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* df_n(void*) { return "[VIZZable] DRTYFEEDR"; }
static void* df_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (df_d*)bzalloc(sizeof(df_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->zoom = 1.0f; d->mx = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(df_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void df_ds(void* d) { auto* x = (df_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void df_up(void* d, obs_data_t* s) {
    auto* x = (df_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->drive = (float)obs_data_get_double(s, "drive"); x->bleed = (float)obs_data_get_double(s, "bleed"); x->zoom = (float)obs_data_get_double(s, "zoom"); x->rot = (float)obs_data_get_double(s, "rot"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void df_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "drive", 0.0); obs_data_set_default_double(s, "bleed", 0.0); obs_data_set_default_double(s, "zoom", 1.0); obs_data_set_default_double(s, "rot", 0.0); obs_data_set_default_double(s, "mx", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void df_rd(void* d, gs_effect_t*) {
    auto* x = (df_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_drive"), x->drive + (m.get_cc(x->dev, x->ch, x->ccolor) * 4.0f) + (note_hit * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bleed"), x->bleed + (m.get_cc(x->dev, x->ch, x->csweep) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.1f, x->zoom * (1.0f + m.get_cc(x->dev, x->ch, x->ccontour) * 2.0f) - (note_hit * 0.3f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rot"), x->rot + (m.get_cc(x->dev, x->ch, x->cshape) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx * m.get_cc(x->dev, x->ch, x->cmix), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* df_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Overdrive Hit", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "drive", "Overdrive (Color CC18)", 0.0, 4.0, 0.05); obs_properties_add_float_slider(p, "bleed", "Color Bleed (Sweep CC20)", 0.0, 3.0, 0.05);
    return p;
}

// =========================================================================
// 21. [VIZZable] FEEDR & BUFFR (Multi-Tap Delay Loop)
// =========================================================================
static const char* fd_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_decay;\nuniform float p_rot;\nuniform float p_zoom;\nuniform float p_chroma;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 col = image.Sample(def_s, v.uv);\n"
"    float2 uv = v.uv - 0.5;\n"
"    float rad = p_rot * 0.01745329;\n"
"    float cosA = cos(rad); float sinA = sin(rad);\n"
"    [unroll] for (int i = 1; i <= 6; ++i) {\n"
"        float fi = float(i);\n"
"        float z = pow(p_zoom, fi * 0.3);\n"
"        float2 r_uv = float2(uv.x * cosA - uv.y * sinA, uv.x * sinA + uv.y * cosA) * z + 0.5;\n"
"        float w = pow(p_decay, fi);\n"
"        float4 echo = image.Sample(def_s, r_uv);\n"
"        col = max(col, echo * w);\n"
"    }\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct fd_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, decay, rot, zoom; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* fd_n(void*) { return "[VIZZable] FEEDR & BUFFR"; }
static void* fd_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (fd_d*)bzalloc(sizeof(fd_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->decay = 0.8f; d->zoom = 1.05f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(fd_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void fd_ds(void* d) { auto* x = (fd_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void fd_up(void* d, obs_data_t* s) {
    auto* x = (fd_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->decay = (float)obs_data_get_double(s, "decay"); x->rot = (float)obs_data_get_double(s, "rot"); x->zoom = (float)obs_data_get_double(s, "zoom");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void fd_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "decay", 0.8); obs_data_set_default_double(s, "rot", 0.0); obs_data_set_default_double(s, "zoom", 1.05);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void fd_rd(void* d, gs_effect_t*) {
    auto* x = (fd_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_decay"), (std::clamp)(x->decay + (m.get_cc(x->dev, x->ch, x->ccolor) * 0.3f) + (note_hit * 0.3f), 0.0f, 0.98f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rot"), x->rot + (m.get_cc(x->dev, x->ch, x->cshape) * 90.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.5f, x->zoom + ((m.get_cc(x->dev, x->ch, x->csweep) - 0.5f) * 1.0f)));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* fd_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Echo Decay Hit", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "decay", "Lecsenges (Color CC18)", 0.0, 0.98, 0.02); obs_properties_add_float_slider(p, "zoom", "Echo Zoom (Sweep CC20)", 0.5, 2.0, 0.02);
    return p;
}

// =========================================================================
// 22. [VIZZable] WAVR (Phase-Modulated Directional Sine Wave)
// =========================================================================
static const char* wv_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_amp;\nuniform float p_freq;\nuniform float p_speed;\nuniform float p_angle;\nuniform float p_chroma;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float rad = p_angle * 6.2831853;\n"
"    float2 dir = float2(cos(rad), sin(rad));\n"
"    float w = sin(dot(v.uv, dir) * p_freq * 20.0 + p_speed) * p_amp * 0.1;\n"
"    float2 offset = float2(-dir.y, dir.x) * w;\n"
"    if (p_chroma > 0.001) {\n"
"        float r = image.Sample(def_s, v.uv + offset * (1.0 + p_chroma)).r;\n"
"        float g = image.Sample(def_s, v.uv + offset).g;\n"
"        float b = image.Sample(def_s, v.uv + offset * (1.0 - p_chroma)).b;\n"
"        return float4(r, g, b, 1.0);\n"
"    }\n"
"    return image.Sample(def_s, v.uv + offset);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct wv_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, amp, freq, sp, angle, chroma, phase; int cpitch, cdecay, ccolor, cshape, csweep, ccontour, cpan, cmix; };
static const char* wv_n(void*) { return "[VIZZable] WAVR"; }
static void* wv_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (wv_d*)bzalloc(sizeof(wv_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->amp = 0.05f; d->freq = 5.0f; d->sp = 1.0f;
    d->cpitch = 16; d->cdecay = 17; d->ccolor = 18; d->cshape = 19; d->csweep = 20; d->ccontour = 21; d->cpan = 10; d->cmix = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(wv_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void wv_ds(void* d) { auto* x = (wv_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void wv_up(void* d, obs_data_t* s) {
    auto* x = (wv_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->amp = (float)obs_data_get_double(s, "amp"); x->freq = (float)obs_data_get_double(s, "freq"); x->sp = (float)obs_data_get_double(s, "sp");
    x->cpitch = (int)obs_data_get_int(s, "cpitch"); x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->ccolor = (int)obs_data_get_int(s, "ccolor");
    x->cshape = (int)obs_data_get_int(s, "cshape"); x->csweep = (int)obs_data_get_int(s, "csweep"); x->ccontour = (int)obs_data_get_int(s, "ccontour");
    x->cpan = (int)obs_data_get_int(s, "cpan"); x->cmix = (int)obs_data_get_int(s, "cmix");
}
static void wv_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "amp", 0.05); obs_data_set_default_double(s, "freq", 5.0); obs_data_set_default_double(s, "sp", 1.0);
    obs_data_set_default_int(s, "cpitch", 16); obs_data_set_default_int(s, "cdecay", 17); obs_data_set_default_int(s, "ccolor", 18); obs_data_set_default_int(s, "cshape", 19); obs_data_set_default_int(s, "csweep", 20); obs_data_set_default_int(s, "ccontour", 21); obs_data_set_default_int(s, "cpan", 10); obs_data_set_default_int(s, "cmix", 7);
}
static void wv_rd(void* d, gs_effect_t*) {
    auto* x = (wv_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->phase += (x->sp + (m.get_cc(x->dev, x->ch, x->cdecay) * 5.0f)) * 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_amp"), x->amp + (m.get_cc(x->dev, x->ch, x->ccolor) * 0.5f) + (note_hit * 0.4f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_freq"), x->freq + (m.get_cc(x->dev, x->ch, x->cpitch) * 30.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_speed"), x->phase);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_angle"), m.get_cc(x->dev, x->ch, x->cshape));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_chroma"), m.get_cc(x->dev, x->ch, x->csweep) * 2.0f);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* wv_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Wave Blast", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "amp", "Amplitudo (Color CC18)", 0.0, 1.0, 0.01); obs_properties_add_float_slider(p, "freq", "Frekvencia (Pitch CC16)", 1.0, 50.0, 0.5);
    return p;
}

// =========================================================================
// OBS MODUL REGISZTRACIO (DLL-specifikus felteteles regisztracio)
// =========================================================================
static obs_source_info s_active_info;

bool obs_module_load(void) {
    MidiCore::instance().init();

#if defined(BUILD_VZ_2TONR)
    s_active_info = reg_f("vz_2tonr", to_n, to_cr, to_ds, to_up, to_def, to_pr, to_rd);
#elif defined(BUILD_VZ_BLURR)
    s_active_info = reg_f("vz_blurr", bl_n, bl_cr, bl_ds, bl_up, bl_def, bl_pr, bl_rd);
#elif defined(BUILD_VZ_BRCOSR)
    s_active_info = reg_f("vz_brcosr", br_n, br_cr, br_ds, br_up, br_def, br_pr, br_rd);
#elif defined(BUILD_VZ_BREATHR)
    s_active_info = reg_f("vz_breathr", brt_n, brt_cr, brt_ds, brt_up, brt_def, brt_pr, brt_rd);
#elif defined(BUILD_VZ_CLRMAPR)
    s_active_info = reg_f("vz_clrmapr", cm_n, cm_cr, cm_ds, cm_up, cm_def, cm_pr, cm_rd);
#elif defined(BUILD_VZ_CROPR)
    s_active_info = reg_f("vz_cropr", cr_n, cr_cr, cr_ds, cr_up, cr_def, cr_pr, cr_rd);
#elif defined(BUILD_VZ_EXPOSR)
    s_active_info = reg_f("vz_exposr", ex_n, ex_cr, ex_ds, ex_up, ex_def, ex_pr, ex_rd);
#elif defined(BUILD_VZ_FISHEYR)
    s_active_info = reg_f("vz_fisheyr", fi_n, fi_cr, fi_ds, fi_up, fi_def, fi_pr, fi_rd);
#elif defined(BUILD_VZ_HUESHIFTR)
    s_active_info = reg_f("vz_hueshiftr", hs_n, hs_cr, hs_ds, hs_up, hs_def, hs_pr, hs_rd);
#elif defined(BUILD_VZ_KALEIDR)
    s_active_info = reg_f("vz_kaleidr", kl_n, kl_cr, kl_ds, kl_up, kl_def, kl_pr, kl_rd);
#elif defined(BUILD_VZ_PINCHR)
    s_active_info = reg_f("vz_pinchr", pn_n, pn_cr, pn_ds, pn_up, pn_def, pn_pr, pn_rd);
#elif defined(BUILD_VZ_PIXEL8R)
    s_active_info = reg_f("vz_pixel8r", px_n, px_cr, px_ds, px_up, px_def, px_pr, px_rd);
#elif defined(BUILD_VZ_RGBR)
    s_active_info = reg_f("vz_rgbr", rg_n, rg_cr, rg_ds, rg_up, rg_def, rg_pr, rg_rd);
#elif defined(BUILD_VZ_SCANLINES)
    s_active_info = reg_f("vz_scanlines", sc_n, sc_cr, sc_ds, sc_up, sc_def, sc_pr, sc_rd);
#elif defined(BUILD_VZ_SLICR)
    s_active_info = reg_f("vz_slicr", sl_n, sl_cr, sl_ds, sl_up, sl_def, sl_pr, sl_rd);
#elif defined(BUILD_VZ_SPRINKLR)
    s_active_info = reg_f("vz_sprinklr", sp_n, sp_cr, sp_ds, sp_up, sp_def, sp_pr, sp_rd);
#elif defined(BUILD_VZ_STROBR)
    s_active_info = reg_f("vz_strobr", st_n, st_cr, st_ds, st_up, st_def, st_pr, st_rd);
#elif defined(BUILD_VZ_TWISTR)
    s_active_info = reg_f("vz_twistr", tw_n, tw_cr, tw_ds, tw_up, tw_def, tw_pr, tw_rd);
#elif defined(BUILD_VZ_ZOROPR)
    s_active_info = reg_f("vz_zoropr", zo_n, zo_cr, zo_ds, zo_up, zo_def, zo_pr, zo_rd);
#elif defined(BUILD_VZ_DRTYFEEDR)
    s_active_info = reg_f("vz_drtyfeedr", df_n, df_cr, df_ds, df_up, df_def, df_pr, df_rd);
#elif defined(BUILD_VZ_FEEDR)
    s_active_info = reg_f("vz_feedr", fd_n, fd_cr, fd_ds, fd_up, fd_def, fd_pr, fd_rd);
#elif defined(BUILD_VZ_WAVR)
    s_active_info = reg_f("vz_wavr", wv_n, wv_cr, wv_ds, wv_up, wv_def, wv_pr, wv_rd);
#endif

    obs_register_source(&s_active_info);
    return true;
}

void obs_module_unload(void) {
    MidiCore::instance().shutdown();
}
