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
// 1. 2TONR
// =========================================================================
static const char* to_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\n"
"sampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float3 p_col_a;\nuniform float3 p_col_b;\nuniform float p_thresh;\nuniform float p_curve;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    float luma = dot(orig.rgb, float3(0.299, 0.587, 0.114));\n"
"    luma = saturate((luma - p_thresh) * p_curve + 0.5);\n"
"    float3 duo = lerp(p_col_b, p_col_a, luma);\n"
"    return lerp(orig, float4(duo, orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct to_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, ra, ga, ba, rb, gb, bb, thresh, curve, mx; int cra, cga, cba, crb, cgb, cbb, cthresh, ccurve, cmx; };
static const char* to_n(void*) { return "[VIZZable] 2TONR"; }
static void* to_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (to_d*)bzalloc(sizeof(to_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->ra = 1.0f; d->ga = 0.9f; d->ba = 0.2f; d->rb = 0.1f; d->gb = 0.1f; d->bb = 0.8f; d->thresh = 0.5f; d->curve = 1.0f; d->mx = 1.0f;
    d->cra = 16; d->cga = 17; d->cba = 18; d->crb = 19; d->cgb = 20; d->cbb = 21; d->cthresh = 10; d->ccurve = 12; d->cmx = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(to_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void to_ds(void* d) { auto* x = (to_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void to_up(void* d, obs_data_t* s) {
    auto* x = (to_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->ra = (float)obs_data_get_double(s, "ra"); x->ga = (float)obs_data_get_double(s, "ga"); x->ba = (float)obs_data_get_double(s, "ba");
    x->rb = (float)obs_data_get_double(s, "rb"); x->gb = (float)obs_data_get_double(s, "gb"); x->bb = (float)obs_data_get_double(s, "bb");
    x->thresh = (float)obs_data_get_double(s, "thresh"); x->curve = (float)obs_data_get_double(s, "curve"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cra = (int)obs_data_get_int(s, "cra"); x->cga = (int)obs_data_get_int(s, "cga"); x->cba = (int)obs_data_get_int(s, "cba");
    x->crb = (int)obs_data_get_int(s, "crb"); x->cgb = (int)obs_data_get_int(s, "cgb"); x->cbb = (int)obs_data_get_int(s, "cbb");
    x->cthresh = (int)obs_data_get_int(s, "cthresh"); x->ccurve = (int)obs_data_get_int(s, "ccurve"); x->cmx = (int)obs_data_get_int(s, "cmx");
}
static void to_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "ra", 1.0); obs_data_set_default_int(s, "cra", 16); obs_data_set_default_double(s, "ga", 0.9); obs_data_set_default_int(s, "cga", 17); obs_data_set_default_double(s, "ba", 0.2); obs_data_set_default_int(s, "cba", 18);
    obs_data_set_default_double(s, "rb", 0.1); obs_data_set_default_int(s, "crb", 19); obs_data_set_default_double(s, "gb", 0.1); obs_data_set_default_int(s, "cgb", 20); obs_data_set_default_double(s, "bb", 0.8); obs_data_set_default_int(s, "cbb", 21);
    obs_data_set_default_double(s, "thresh", 0.5); obs_data_set_default_int(s, "cthresh", 10); obs_data_set_default_double(s, "curve", 1.0); obs_data_set_default_int(s, "ccurve", 12); obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmx", 7);
}
static void to_rd(void* d, gs_effect_t*) {
    auto* x = (to_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    vec3 c_a, c_b;
    c_a.x = (std::clamp)(x->ra + m.get_cc(x->dev, x->ch, x->cra) * 2.0f + (note_hit * 0.8f), 0.0f, 1.0f); c_a.y = (std::clamp)(x->ga + m.get_cc(x->dev, x->ch, x->cga) * 2.0f, 0.0f, 1.0f); c_a.z = (std::clamp)(x->ba + m.get_cc(x->dev, x->ch, x->cba) * 2.0f, 0.0f, 1.0f);
    c_b.x = (std::clamp)(x->rb + m.get_cc(x->dev, x->ch, x->crb) * 2.0f, 0.0f, 1.0f); c_b.y = (std::clamp)(x->gb + m.get_cc(x->dev, x->ch, x->cgb) * 2.0f, 0.0f, 1.0f); c_b.z = (std::clamp)(x->bb + m.get_cc(x->dev, x->ch, x->cbb) * 2.0f + (note_hit * 0.8f), 0.0f, 1.0f);
    gs_effect_set_vec3(gs_effect_get_param_by_name(x->eff, "p_col_a"), &c_a); gs_effect_set_vec3(gs_effect_get_param_by_name(x->eff, "p_col_b"), &c_b);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_thresh"), x->thresh + ((m.get_cc(x->dev, x->ch, x->cthresh) - 0.5f) * 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_curve"), (std::max)(0.1f, x->curve + (m.get_cc(x->dev, x->ch, x->ccurve) * 5.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx + (m.get_cc(x->dev, x->ch, x->cmx) * 1.0f), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* to_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Duotone Flash", 0.0, 2.0, 0.05);
    obs_properties_add_float_slider(p, "ra", "Vilagos R", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cra", "-> CC Vilagos R [16]", 0, 127, 1);
    obs_properties_add_float_slider(p, "ga", "Vilagos G", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cga", "-> CC Vilagos G [17]", 0, 127, 1);
    obs_properties_add_float_slider(p, "ba", "Vilagos B", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cba", "-> CC Vilagos B [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "rb", "Arnyek R", 0.0, 1.0, 0.02); obs_properties_add_int(p, "crb", "-> CC Arnyek R [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "gb", "Arnyek G", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cgb", "-> CC Arnyek G [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "bb", "Arnyek B", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cbb", "-> CC Arnyek B [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "thresh", "Kuszob", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cthresh", "-> CC Kuszob [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "curve", "Meredekseg", 0.1, 8.0, 0.1); obs_properties_add_int(p, "ccurve", "-> CC Meredekseg [12]", 0, 127, 1);
    obs_properties_add_float_slider(p, "mx", "Mix", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cmx", "-> CC Mix [7]", 0, 127, 1);
    return p;
}

// =========================================================================
// 2. BLURR
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
"    float2 dir = float2(cos(rad), sin(rad)) * p_amt * 0.01;\n"
"    for (int i = -4; i <= 4; ++i) {\n"
"        float fi = float(i) * 0.25;\n"
"        float2 uv_s = lerp(v.uv + dir * fi, center + (v.uv - center) * (1.0 + fi * p_rad_blur * 0.1), step(0.01, p_rad_blur));\n"
"        col += image.Sample(def_s, uv_s);\n"
"    }\n"
"    col /= 9.0;\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    return lerp(orig, col, p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct bl_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, amt, rad_b, ang, cx, cy, mx; int camt, crad, cang, ccx, ccy, cmx; };
static const char* bl_n(void*) { return "[VIZZable] BLURR"; }
static void* bl_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (bl_d*)bzalloc(sizeof(bl_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->amt = 1.0f; d->mx = 1.0f;
    d->camt = 18; d->crad = 20; d->cang = 19; d->ccx = 10; d->ccy = 17; d->cmx = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(bl_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void bl_ds(void* d) { auto* x = (bl_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void bl_up(void* d, obs_data_t* s) {
    auto* x = (bl_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->amt = (float)obs_data_get_double(s, "amt"); x->rad_b = (float)obs_data_get_double(s, "rad_b"); x->ang = (float)obs_data_get_double(s, "ang");
    x->cx = (float)obs_data_get_double(s, "cx"); x->cy = (float)obs_data_get_double(s, "cy"); x->mx = (float)obs_data_get_double(s, "mx");
    x->camt = (int)obs_data_get_int(s, "camt"); x->crad = (int)obs_data_get_int(s, "crad"); x->cang = (int)obs_data_get_int(s, "cang");
    x->ccx = (int)obs_data_get_int(s, "ccx"); x->ccy = (int)obs_data_get_int(s, "ccy"); x->cmx = (int)obs_data_get_int(s, "cmx");
}
static void bl_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "amt", 1.0); obs_data_set_default_int(s, "camt", 18); obs_data_set_default_double(s, "rad_b", 0.0); obs_data_set_default_int(s, "crad", 20);
    obs_data_set_default_double(s, "ang", 0.0); obs_data_set_default_int(s, "cang", 19); obs_data_set_default_double(s, "cx", 0.0); obs_data_set_default_int(s, "ccx", 10);
    obs_data_set_default_double(s, "cy", 0.0); obs_data_set_default_int(s, "ccy", 17); obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmx", 7);
}
static void bl_rd(void* d, gs_effect_t*) {
    auto* x = (bl_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_amt"), x->amt + (m.get_cc(x->dev, x->ch, x->camt) * 8.0f) + (note_hit * 4.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rad_blur"), x->rad_b + (m.get_cc(x->dev, x->ch, x->crad) * 3.0f) + (note_hit * 1.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_ang"), x->ang + (m.get_cc(x->dev, x->ch, x->cang) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), x->cx + (m.get_cc(x->dev, x->ch, x->ccx) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), x->cy + (m.get_cc(x->dev, x->ch, x->ccy) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx + (m.get_cc(x->dev, x->ch, x->cmx) * 1.0f), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* bl_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Blur Loket", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "amt", "Elmosas Merteke", 0.0, 10.0, 0.1); obs_properties_add_int(p, "camt", "-> CC Elmosas [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "rad_b", "Radial Zoom Blur", 0.0, 3.0, 0.05); obs_properties_add_int(p, "crad", "-> CC Radial Blur [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "ang", "Irany Szog", -180.0, 180.0, 1.0); obs_properties_add_int(p, "cang", "-> CC Szog [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cx", "Origo X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccx", "-> CC Origo X [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cy", "Origo Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccy", "-> CC Origo Y [17]", 0, 127, 1);
    obs_properties_add_float_slider(p, "mx", "Mix", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cmx", "-> CC Mix [7]", 0, 127, 1);
    return p;
}

// =========================================================================
// 3. BRCOSR+
// =========================================================================
static const char* br_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_b;\nuniform float p_c;\nuniform float p_s;\nuniform float p_gamma;\nuniform float p_r;\nuniform float p_g;\nuniform float p_bl;\nuniform float p_hue;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 col = image.Sample(def_s, v.uv);\n"
"    col.r *= p_r; col.g *= p_g; col.b *= p_bl;\n"
"    float angle = p_hue * 6.2831853;\n"
"    float3 k = float3(0.57735, 0.57735, 0.57735);\n"
"    float cosA = cos(angle);\n"
"    col.rgb = col.rgb * cosA + cross(k, col.rgb) * sin(angle) + k * dot(k, col.rgb) * (1.0 - cosA);\n"
"    float grey = dot(col.rgb, float3(0.299, 0.587, 0.114));\n"
"    col.rgb = lerp(float3(grey, grey, grey), col.rgb, p_s);\n"
"    col.rgb = (col.rgb - 0.5) * p_c + 0.5 + p_b;\n"
"    col.rgb = pow(max(col.rgb, float3(0.0001, 0.0001, 0.0001)), float3(1.0/p_gamma, 1.0/p_gamma, 1.0/p_gamma));\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct br_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, b, c, s, gamma, r, g, bl, hue; int cb, cc, cs, cgamma, cr, cg, cbl, chue; };
static const char* br_n(void*) { return "[VIZZable] BRCOSR+"; }
static void* br_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (br_d*)bzalloc(sizeof(br_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.0f; d->b = 0.0f; d->c = 1.0f; d->s = 1.0f; d->gamma = 1.0f; d->r = 1.0f; d->g = 1.0f; d->bl = 1.0f; d->hue = 0.0f;
    d->cb = 20; d->cc = 21; d->cs = 18; d->cgamma = 17; d->cr = 16; d->cg = 10; d->cbl = 19; d->chue = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(br_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void br_ds(void* d) { auto* x = (br_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void br_up(void* d, obs_data_t* s) {
    auto* x = (br_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->b = (float)obs_data_get_double(s, "b"); x->c = (float)obs_data_get_double(s, "c"); x->s = (float)obs_data_get_double(s, "s"); x->gamma = (float)obs_data_get_double(s, "gamma");
    x->r = (float)obs_data_get_double(s, "r"); x->g = (float)obs_data_get_double(s, "g"); x->bl = (float)obs_data_get_double(s, "bl"); x->hue = (float)obs_data_get_double(s, "hue");
    x->cb = (int)obs_data_get_int(s, "cb"); x->cc = (int)obs_data_get_int(s, "cc"); x->cs = (int)obs_data_get_int(s, "cs"); x->cgamma = (int)obs_data_get_int(s, "cgamma");
    x->cr = (int)obs_data_get_int(s, "cr"); x->cg = (int)obs_data_get_int(s, "cg"); x->cbl = (int)obs_data_get_int(s, "cbl"); x->chue = (int)obs_data_get_int(s, "chue");
}
static void br_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.0);
    obs_data_set_default_double(s, "b", 0.0); obs_data_set_default_int(s, "cb", 20); obs_data_set_default_double(s, "c", 1.0); obs_data_set_default_int(s, "cc", 21);
    obs_data_set_default_double(s, "s", 1.0); obs_data_set_default_int(s, "cs", 18); obs_data_set_default_double(s, "gamma", 1.0); obs_data_set_default_int(s, "cgamma", 17);
    obs_data_set_default_double(s, "r", 1.0); obs_data_set_default_int(s, "cr", 16); obs_data_set_default_double(s, "g", 1.0); obs_data_set_default_int(s, "cg", 10);
    obs_data_set_default_double(s, "bl", 1.0); obs_data_set_default_int(s, "cbl", 19); obs_data_set_default_double(s, "hue", 0.0); obs_data_set_default_int(s, "chue", 7);
}
static void br_rd(void* d, gs_effect_t*) {
    auto* x = (br_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_b"), x->b + (m.get_cc(x->dev, x->ch, x->cb) * 2.0f) + (note_hit * 0.6f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_c"), x->c + (m.get_cc(x->dev, x->ch, x->cc) * 3.0f) + (note_hit * 0.8f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_s"), x->s + (m.get_cc(x->dev, x->ch, x->cs) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_gamma"), (std::max)(0.1f, x->gamma + (m.get_cc(x->dev, x->ch, x->cgamma) * 3.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_r"), x->r + (m.get_cc(x->dev, x->ch, x->cr) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_g"), x->g + (m.get_cc(x->dev, x->ch, x->cg) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bl"), x->bl + (m.get_cc(x->dev, x->ch, x->cbl) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_hue"), x->hue + m.get_cc(x->dev, x->ch, x->chue));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* br_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Flash", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "b", "Fenyero", -1.0, 1.0, 0.02); obs_properties_add_int(p, "cb", "-> CC Fenyero [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "c", "Kontraszt", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cc", "-> CC Kontraszt [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "s", "Telitettseg", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cs", "-> CC Telitettseg [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "gamma", "Gamma", 0.1, 4.0, 0.05); obs_properties_add_int(p, "cgamma", "-> CC Gamma [17]", 0, 127, 1);
    obs_properties_add_float_slider(p, "r", "Voros", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cr", "-> CC Voros [16]", 0, 127, 1);
    obs_properties_add_float_slider(p, "g", "Zold", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cg", "-> CC Zold [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "bl", "Kek", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cbl", "-> CC Kek [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "hue", "Hue Szinkor", 0.0, 1.0, 0.01); obs_properties_add_int(p, "chue", "-> CC Hue [7]", 0, 127, 1);
    return p;
}

// =========================================================================
// 4. BREATHR
// =========================================================================
static const char* brt_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_depth;\nuniform float p_spd;\nuniform float p_chroma;\nuniform float p_cx;\nuniform float p_cy;\nuniform float p_phase;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 center = float2(0.5 + p_cx, 0.5 + p_cy);\n"
"    float zoom = 1.0 + sin(p_phase) * p_depth * 0.5;\n"
"    float2 uv = (v.uv - center) * zoom + center;\n"
"    if (p_chroma > 0.01) {\n"
"        float2 off = (v.uv - center) * p_chroma * 0.03 * sin(p_phase);\n"
"        float r = image.Sample(def_s, uv + off).r;\n"
"        float g = image.Sample(def_s, uv).g;\n"
"        float b = image.Sample(def_s, uv - off).b;\n"
"        return float4(r, g, b, 1.0);\n"
"    }\n"
"    return image.Sample(def_s, uv);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct brt_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, depth, spd, chroma, cx, cy; int cdepth, cspd, cchroma, ccx, ccy; float phase; };
static const char* brt_n(void*) { return "[VIZZable] BREATHR"; }
static void* brt_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (brt_d*)bzalloc(sizeof(brt_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->depth = 0.2f; d->spd = 1.0f;
    d->cdepth = 18; d->cspd = 20; d->cchroma = 19; d->ccx = 10; d->ccy = 17;
    obs_enter_graphics(); d->eff = gs_effect_create(brt_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void brt_ds(void* d) { auto* x = (brt_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void brt_up(void* d, obs_data_t* s) {
    auto* x = (brt_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->depth = (float)obs_data_get_double(s, "depth"); x->spd = (float)obs_data_get_double(s, "spd"); x->chroma = (float)obs_data_get_double(s, "chroma");
    x->cx = (float)obs_data_get_double(s, "cx"); x->cy = (float)obs_data_get_double(s, "cy");
    x->cdepth = (int)obs_data_get_int(s, "cdepth"); x->cspd = (int)obs_data_get_int(s, "cspd"); x->cchroma = (int)obs_data_get_int(s, "cchroma");
    x->ccx = (int)obs_data_get_int(s, "ccx"); x->ccy = (int)obs_data_get_int(s, "ccy");
}
static void brt_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "depth", 0.2); obs_data_set_default_int(s, "cdepth", 18); obs_data_set_default_double(s, "spd", 1.0); obs_data_set_default_int(s, "cspd", 20);
    obs_data_set_default_double(s, "chroma", 0.0); obs_data_set_default_int(s, "cchroma", 19); obs_data_set_default_double(s, "cx", 0.0); obs_data_set_default_int(s, "ccx", 10);
    obs_data_set_default_double(s, "cy", 0.0); obs_data_set_default_int(s, "ccy", 17);
}
static void brt_rd(void* d, gs_effect_t*) {
    auto* x = (brt_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->phase += (x->spd + (m.get_cc(x->dev, x->ch, x->cspd) * 5.0f)) * 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_depth"), x->depth + (m.get_cc(x->dev, x->ch, x->cdepth) * 1.5f) + (note_hit * 0.8f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_chroma"), x->chroma + (m.get_cc(x->dev, x->ch, x->cchroma) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), x->cx + (m.get_cc(x->dev, x->ch, x->ccx) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), x->cy + (m.get_cc(x->dev, x->ch, x->ccy) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_phase"), x->phase);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* brt_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Breath Pump", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "depth", "Legzes Melyseg", 0.0, 2.0, 0.02); obs_properties_add_int(p, "cdepth", "-> CC Melyseg [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "spd", "Legzes Sebesseg", 0.1, 10.0, 0.1); obs_properties_add_int(p, "cspd", "-> CC Sebesseg [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "chroma", "Kromatikus Szetszoras", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cchroma", "-> CC Kromatika [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cx", "Origo X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccx", "-> CC Origo X [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cy", "Origo Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccy", "-> CC Origo Y [17]", 0, 127, 1);
    return p;
}

// =========================================================================
// 5. CLRMAPR
// =========================================================================
static const char* cm_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_shift;\nuniform float p_solar;\nuniform float p_steps;\nuniform float p_inv;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    float luma = dot(orig.rgb, float3(0.299, 0.587, 0.114));\n"
"    luma = frac(luma + p_shift);\n"
"    if (p_solar > 0.01) luma = lerp(luma, abs(1.0 - luma * 2.0), p_solar);\n"
"    if (p_steps > 1.5) luma = floor(luma * p_steps) / p_steps;\n"
"    float3 map_c = float3(sin(luma * 6.28), sin(luma * 6.28 + 2.09), sin(luma * 6.28 + 4.18)) * 0.5 + 0.5;\n"
"    if (p_inv > 0.5) map_c = 1.0 - map_c;\n"
"    return lerp(orig, float4(map_c, orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct cm_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, shift, solar, steps, inv, mx; int cshift, csolar, csteps, cinv, cmx; };
static const char* cm_n(void*) { return "[VIZZable] CLRMAPR"; }
static void* cm_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (cm_d*)bzalloc(sizeof(cm_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->mx = 1.0f;
    d->cshift = 18; d->csolar = 20; d->csteps = 19; d->cinv = 21; d->cmx = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(cm_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void cm_ds(void* d) { auto* x = (cm_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void cm_up(void* d, obs_data_t* s) {
    auto* x = (cm_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->shift = (float)obs_data_get_double(s, "shift"); x->solar = (float)obs_data_get_double(s, "solar"); x->steps = (float)obs_data_get_double(s, "steps");
    x->inv = (float)obs_data_get_double(s, "inv"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cshift = (int)obs_data_get_int(s, "cshift"); x->csolar = (int)obs_data_get_int(s, "csolar"); x->csteps = (int)obs_data_get_int(s, "csteps");
    x->cinv = (int)obs_data_get_int(s, "cinv"); x->cmx = (int)obs_data_get_int(s, "cmx");
}
static void cm_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "shift", 0.0); obs_data_set_default_int(s, "cshift", 18); obs_data_set_default_double(s, "solar", 0.0); obs_data_set_default_int(s, "csolar", 20);
    obs_data_set_default_double(s, "steps", 0.0); obs_data_set_default_int(s, "csteps", 19); obs_data_set_default_double(s, "inv", 0.0); obs_data_set_default_int(s, "cinv", 21);
    obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmx", 7);
}
static void cm_rd(void* d, gs_effect_t*) {
    auto* x = (cm_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_shift"), x->shift + m.get_cc(x->dev, x->ch, x->cshift) + (note_hit * 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_solar"), (std::clamp)(x->solar + (m.get_cc(x->dev, x->ch, x->csolar) * 1.0f), 0.0f, 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_steps"), x->steps + (m.get_cc(x->dev, x->ch, x->csteps) * 16.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_inv"), std::fmod(x->inv + m.get_cc(x->dev, x->ch, x->cinv), 2.0f) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx + (m.get_cc(x->dev, x->ch, x->cmx) * 1.0f), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* cm_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Color Cycle", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "shift", "Szinterkep Eltolas", 0.0, 1.0, 0.01); obs_properties_add_int(p, "cshift", "-> CC Eltolas [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "solar", "Szolarizacio", 0.0, 1.0, 0.02); obs_properties_add_int(p, "csolar", "-> CC Szolarizacio [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "steps", "Szincskok", 0.0, 16.0, 1.0); obs_properties_add_int(p, "csteps", "-> CC Szincskok [19]", 0, 127, 1);
    obs_properties_add_bool(p, "inv", "Inverz Paletta"); obs_properties_add_int(p, "cinv", "-> CC Inverz [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "mx", "Mix", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cmx", "-> CC Mix [7]", 0, 127, 1);
    return p;
}

// =========================================================================
// 6. CROPR
// =========================================================================
static const char* cr_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_left;\nuniform float p_right;\nuniform float p_top;\nuniform float p_bottom;\nuniform float p_feather;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 uv = v.uv;\n"
"    float f = max(p_feather, 0.0001);\n"
"    float m_l = smoothstep(p_left, p_left + f, uv.x);\n"
"    float m_r = 1.0 - smoothstep(1.0 - p_right - f, 1.0 - p_right, uv.x);\n"
"    float m_t = smoothstep(p_top, p_top + f, uv.y);\n"
"    float m_b = 1.0 - smoothstep(1.0 - p_bottom - f, 1.0 - p_bottom, uv.y);\n"
"    float mask = m_l * m_r * m_t * m_b;\n"
"    float4 col = image.Sample(def_s, uv);\n"
"    return col * mask;\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct cr_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, left, right, top, bottom, feather; int cleft, cright, ctop, cbottom, cfeather; };
static const char* cr_n(void*) { return "[VIZZable] CROPR"; }
static void* cr_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (cr_d*)bzalloc(sizeof(cr_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.0f;
    d->cleft = 16; d->cright = 17; d->ctop = 18; d->cbottom = 19; d->cfeather = 20;
    obs_enter_graphics(); d->eff = gs_effect_create(cr_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void cr_ds(void* d) { auto* x = (cr_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void cr_up(void* d, obs_data_t* s) {
    auto* x = (cr_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->left = (float)obs_data_get_double(s, "left"); x->right = (float)obs_data_get_double(s, "right"); x->top = (float)obs_data_get_double(s, "top"); x->bottom = (float)obs_data_get_double(s, "bottom"); x->feather = (float)obs_data_get_double(s, "feather");
    x->cleft = (int)obs_data_get_int(s, "cleft"); x->cright = (int)obs_data_get_int(s, "cright"); x->ctop = (int)obs_data_get_int(s, "ctop"); x->cbottom = (int)obs_data_get_int(s, "cbottom"); x->cfeather = (int)obs_data_get_int(s, "cfeather");
}
static void cr_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.0);
    obs_data_set_default_double(s, "left", 0.0); obs_data_set_default_int(s, "cleft", 16); obs_data_set_default_double(s, "right", 0.0); obs_data_set_default_int(s, "cright", 17);
    obs_data_set_default_double(s, "top", 0.0); obs_data_set_default_int(s, "ctop", 18); obs_data_set_default_double(s, "bottom", 0.0); obs_data_set_default_int(s, "cbottom", 19);
    obs_data_set_default_double(s, "feather", 0.01); obs_data_set_default_int(s, "cfeather", 20);
}
static void cr_rd(void* d, gs_effect_t*) {
    auto* x = (cr_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_left"), (std::clamp)(x->left + (m.get_cc(x->dev, x->ch, x->cleft) * 0.5f) + (note_hit * 0.2f), 0.0f, 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_right"), (std::clamp)(x->right + (m.get_cc(x->dev, x->ch, x->cright) * 0.5f) + (note_hit * 0.2f), 0.0f, 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_top"), (std::clamp)(x->top + (m.get_cc(x->dev, x->ch, x->ctop) * 0.5f) + (note_hit * 0.2f), 0.0f, 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bottom"), (std::clamp)(x->bottom + (m.get_cc(x->dev, x->ch, x->cbottom) * 0.5f) + (note_hit * 0.2f), 0.0f, 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_feather"), (std::max)(0.0001f, x->feather + (m.get_cc(x->dev, x->ch, x->cfeather) * 0.5f)));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* cr_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Vagas", 0.0, 2.0, 0.05);
    obs_properties_add_float_slider(p, "left", "Bal Vagas", 0.0, 0.5, 0.01); obs_properties_add_int(p, "cleft", "-> CC Bal [16]", 0, 127, 1);
    obs_properties_add_float_slider(p, "right", "Jobb Vagas", 0.0, 0.5, 0.01); obs_properties_add_int(p, "cright", "-> CC Jobb [17]", 0, 127, 1);
    obs_properties_add_float_slider(p, "top", "Felso Vagas", 0.0, 0.5, 0.01); obs_properties_add_int(p, "ctop", "-> CC Felso [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "bottom", "Also Vagas", 0.0, 0.5, 0.01); obs_properties_add_int(p, "cbottom", "-> CC Also [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "feather", "Lagyitas (Feather)", 0.0, 0.5, 0.01); obs_properties_add_int(p, "cfeather", "-> CC Lagyitas [20]", 0, 127, 1);
    return p;
}

// =========================================================================
// 7. EXPOSR
// =========================================================================
static const char* ex_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_ev;\nuniform float p_bloom;\nuniform float p_thresh;\nuniform float p_temp;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 col = image.Sample(def_s, v.uv);\n"
"    col.rgb *= exp2(p_ev);\n"
"    float luma = dot(col.rgb, float3(0.299, 0.587, 0.114));\n"
"    float3 highlight = max(float3(0,0,0), col.rgb - p_thresh) * p_bloom;\n"
"    col.rgb += highlight;\n"
"    col.r *= (1.0 + p_temp * 0.2); col.b *= (1.0 - p_temp * 0.2);\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct ex_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, ev, bloom, thresh, temp; int cev, cbloom, cthresh, ctemp; };
static const char* ex_n(void*) { return "[VIZZable] EXPOSR"; }
static void* ex_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (ex_d*)bzalloc(sizeof(ex_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->ev = 0.0f; d->bloom = 1.0f; d->thresh = 0.7f; d->temp = 0.0f;
    d->cev = 18; d->cbloom = 20; d->cthresh = 21; d->ctemp = 16;
    obs_enter_graphics(); d->eff = gs_effect_create(ex_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void ex_ds(void* d) { auto* x = (ex_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void ex_up(void* d, obs_data_t* s) {
    auto* x = (ex_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->ev = (float)obs_data_get_double(s, "ev"); x->bloom = (float)obs_data_get_double(s, "bloom"); x->thresh = (float)obs_data_get_double(s, "thresh"); x->temp = (float)obs_data_get_double(s, "temp");
    x->cev = (int)obs_data_get_int(s, "cev"); x->cbloom = (int)obs_data_get_int(s, "cbloom"); x->cthresh = (int)obs_data_get_int(s, "cthresh"); x->ctemp = (int)obs_data_get_int(s, "ctemp");
}
static void ex_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "ev", 0.0); obs_data_set_default_int(s, "cev", 18); obs_data_set_default_double(s, "bloom", 1.0); obs_data_set_default_int(s, "cbloom", 20);
    obs_data_set_default_double(s, "thresh", 0.7); obs_data_set_default_int(s, "cthresh", 21); obs_data_set_default_double(s, "temp", 0.0); obs_data_set_default_int(s, "ctemp", 16);
}
static void ex_rd(void* d, gs_effect_t*) {
    auto* x = (ex_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_ev"), x->ev + (m.get_cc(x->dev, x->ch, x->cev) * 4.0f) + (note_hit * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bloom"), x->bloom + (m.get_cc(x->dev, x->ch, x->cbloom) * 4.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_thresh"), (std::max)(0.0f, x->thresh - (m.get_cc(x->dev, x->ch, x->cthresh) * 0.6f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_temp"), x->temp + ((m.get_cc(x->dev, x->ch, x->ctemp) - 0.5f) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* ex_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Expozicio Flash", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "ev", "Expozicio (EV)", -3.0, 5.0, 0.1); obs_properties_add_int(p, "cev", "-> CC Expozicio [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "bloom", "Highlight Bloom", 0.0, 5.0, 0.1); obs_properties_add_int(p, "cbloom", "-> CC Bloom [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "thresh", "Bloom Kuszob", 0.1, 1.0, 0.02); obs_properties_add_int(p, "cthresh", "-> CC Kuszob [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "temp", "Szinhomerseklet", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ctemp", "-> CC Szinhomerseklet [16]", 0, 127, 1);
    return p;
}

// =========================================================================
// 8. FISHEYR
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

struct fi_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, dist, rad, cx, cy, chroma, zoom; int cdist, crad, ccx, ccy, cchroma, czoom; };
static const char* fi_n(void*) { return "[VIZZable] FISHEYR"; }
static void* fi_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (fi_d*)bzalloc(sizeof(fi_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->dist = 0.5f; d->rad = 0.8f; d->zoom = 1.0f;
    d->cdist = 18; d->crad = 20; d->ccx = 10; d->ccy = 17; d->cchroma = 19; d->czoom = 21;
    obs_enter_graphics(); d->eff = gs_effect_create(fi_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void fi_ds(void* d) { auto* x = (fi_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void fi_up(void* d, obs_data_t* s) {
    auto* x = (fi_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->dist = (float)obs_data_get_double(s, "dist"); x->rad = (float)obs_data_get_double(s, "rad"); x->cx = (float)obs_data_get_double(s, "cx"); x->cy = (float)obs_data_get_double(s, "cy");
    x->chroma = (float)obs_data_get_double(s, "chroma"); x->zoom = (float)obs_data_get_double(s, "zoom");
    x->cdist = (int)obs_data_get_int(s, "cdist"); x->crad = (int)obs_data_get_int(s, "crad"); x->ccx = (int)obs_data_get_int(s, "ccx"); x->ccy = (int)obs_data_get_int(s, "ccy");
    x->cchroma = (int)obs_data_get_int(s, "cchroma"); x->czoom = (int)obs_data_get_int(s, "czoom");
}
static void fi_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "dist", 0.5); obs_data_set_default_int(s, "cdist", 18); obs_data_set_default_double(s, "rad", 0.8); obs_data_set_default_int(s, "crad", 20);
    obs_data_set_default_double(s, "cx", 0.0); obs_data_set_default_int(s, "ccx", 10); obs_data_set_default_double(s, "cy", 0.0); obs_data_set_default_int(s, "ccy", 17);
    obs_data_set_default_double(s, "chroma", 0.0); obs_data_set_default_int(s, "cchroma", 19); obs_data_set_default_double(s, "zoom", 1.0); obs_data_set_default_int(s, "czoom", 21);
}
static void fi_rd(void* d, gs_effect_t*) {
    auto* x = (fi_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_dist"), x->dist + ((m.get_cc(x->dev, x->ch, x->cdist) - 0.5f) * 4.0f) + (note_hit * 0.6f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rad"), (std::max)(0.05f, x->rad + (m.get_cc(x->dev, x->ch, x->crad) * 2.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), x->cx + (m.get_cc(x->dev, x->ch, x->ccx) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), x->cy + (m.get_cc(x->dev, x->ch, x->ccy) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_chroma"), x->chroma + (m.get_cc(x->dev, x->ch, x->cchroma) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.1f, x->zoom * (1.0f + m.get_cc(x->dev, x->ch, x->czoom) * 3.0f)));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* fi_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Bulge Pump", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "dist", "Torzitas", -2.0, 2.0, 0.05); obs_properties_add_int(p, "cdist", "-> CC Torzitas [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "rad", "Lencse Sugar", 0.1, 3.0, 0.05); obs_properties_add_int(p, "crad", "-> CC Sugar [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cx", "Kozep X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccx", "-> CC Kozep X [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cy", "Kozep Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccy", "-> CC Kozep Y [17]", 0, 127, 1);
    obs_properties_add_float_slider(p, "chroma", "Kromatikus Szetszoras", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cchroma", "-> CC Kromatika [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "zoom", "Zoom", 0.1, 5.0, 0.05); obs_properties_add_int(p, "czoom", "-> CC Zoom [21]", 0, 127, 1);
    return p;
}

// =========================================================================
// 9. HUESHIFTR
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
"    float cosA = cos(angle);\n"
"    col.rgb = col.rgb * cosA + cross(k, col.rgb) * sin(angle) + k * dot(k, col.rgb) * (1.0 - cosA);\n"
"    float grey = dot(col.rgb, float3(0.299, 0.587, 0.114));\n"
"    col.rgb = lerp(float3(grey, grey, grey), col.rgb, p_sat) * p_luma;\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct hs_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, hue, sat, luma, cycle; int chue, csat, cluma, ccycle; float time; };
static const char* hs_n(void*) { return "[VIZZable] HUESHIFTR"; }
static void* hs_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (hs_d*)bzalloc(sizeof(hs_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->sat = 1.0f; d->luma = 1.0f;
    d->chue = 18; d->csat = 20; d->cluma = 21; d->ccycle = 19;
    obs_enter_graphics(); d->eff = gs_effect_create(hs_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void hs_ds(void* d) { auto* x = (hs_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void hs_up(void* d, obs_data_t* s) {
    auto* x = (hs_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->hue = (float)obs_data_get_double(s, "hue"); x->sat = (float)obs_data_get_double(s, "sat"); x->luma = (float)obs_data_get_double(s, "luma"); x->cycle = (float)obs_data_get_double(s, "cycle");
    x->chue = (int)obs_data_get_int(s, "chue"); x->csat = (int)obs_data_get_int(s, "csat"); x->cluma = (int)obs_data_get_int(s, "cluma"); x->ccycle = (int)obs_data_get_int(s, "ccycle");
}
static void hs_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "hue", 0.0); obs_data_set_default_int(s, "chue", 18); obs_data_set_default_double(s, "sat", 1.0); obs_data_set_default_int(s, "csat", 20);
    obs_data_set_default_double(s, "luma", 1.0); obs_data_set_default_int(s, "cluma", 21); obs_data_set_default_double(s, "cycle", 0.0); obs_data_set_default_int(s, "ccycle", 19);
}
static void hs_rd(void* d, gs_effect_t*) {
    auto* x = (hs_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->time += 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_hue"), x->hue + m.get_cc(x->dev, x->ch, x->chue) + (note_hit * 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_sat"), x->sat + (m.get_cc(x->dev, x->ch, x->csat) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_luma"), x->luma + (m.get_cc(x->dev, x->ch, x->cluma) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cycle"), x->cycle + (m.get_cc(x->dev, x->ch, x->ccycle) * 5.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_time"), x->time);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* hs_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Hue Jump", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "hue", "Hue Eltolas", 0.0, 1.0, 0.01); obs_properties_add_int(p, "chue", "-> CC Hue [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "sat", "Telitettseg", 0.0, 4.0, 0.05); obs_properties_add_int(p, "csat", "-> CC Telitettseg [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "luma", "Fenyero", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cluma", "-> CC Fenyero [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cycle", "Auto Szinforgas", 0.0, 5.0, 0.1); obs_properties_add_int(p, "ccycle", "-> CC Forgas [19]", 0, 127, 1);
    return p;
}

// =========================================================================
// 10. KALEIDR
// =========================================================================
static const char* kl_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_sides;\nuniform float p_rot;\nuniform float p_twist;\nuniform float p_zoom;\nuniform float p_ox;\nuniform float p_oy;\nuniform float p_iox;\nuniform float p_ioy;\nuniform float p_irot;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 screen_center = float2(0.5 + p_ox, 0.5 + p_oy);\n"
"    float2 uv = v.uv - screen_center;\n"
"    float seg = 6.2831853 / max(p_sides, 1.0);\n"
"    float r = length(uv);\n"
"    float a = atan2(uv.y, uv.x) + (p_rot * 0.0174532925) + (r * p_twist);\n"
"    a = abs(fmod(abs(a), seg) - (seg * 0.5));\n"
"    float2 k_uv = float2(cos(a), sin(a)) * r * max(p_zoom, 0.01);\n"
"    float irad = p_irot * 0.0174532925;\n"
"    float cosI = cos(irad); float sinI = sin(irad);\n"
"    float2 inner_uv = float2(k_uv.x * cosI - k_uv.y * sinI, k_uv.x * sinI + k_uv.y * cosI);\n"
"    inner_uv += float2(0.5 + p_iox, 0.5 + p_ioy);\n"
"    return image.Sample(def_s, inner_uv);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct kl_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, sides, rot, twist, zoom, ox, oy, iox, ioy, irot; int cs, cr, ct, cz, cox, coy, ciox, cioy, cirot; };
static const char* kl_n(void*) { return "[VIZZable] KALEIDR"; }
static void* kl_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (kl_d*)bzalloc(sizeof(kl_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->sides = 6.0f; d->zoom = 1.0f;
    d->cs = 18; d->cr = 19; d->ct = 20; d->cz = 21; d->cox = 10; d->coy = 17; d->ciox = 16; d->cioy = 7; d->cirot = 12;
    obs_enter_graphics(); d->eff = gs_effect_create(kl_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void kl_ds(void* d) { auto* x = (kl_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void kl_up(void* d, obs_data_t* s) {
    auto* x = (kl_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->sides = (float)obs_data_get_double(s, "sides"); x->rot = (float)obs_data_get_double(s, "rot"); x->twist = (float)obs_data_get_double(s, "twist"); x->zoom = (float)obs_data_get_double(s, "zoom");
    x->ox = (float)obs_data_get_double(s, "ox"); x->oy = (float)obs_data_get_double(s, "oy"); x->iox = (float)obs_data_get_double(s, "iox"); x->ioy = (float)obs_data_get_double(s, "ioy"); x->irot = (float)obs_data_get_double(s, "irot");
    x->cs = (int)obs_data_get_int(s, "cs"); x->cr = (int)obs_data_get_int(s, "cr"); x->ct = (int)obs_data_get_int(s, "ct"); x->cz = (int)obs_data_get_int(s, "cz");
    x->cox = (int)obs_data_get_int(s, "cox"); x->coy = (int)obs_data_get_int(s, "coy"); x->ciox = (int)obs_data_get_int(s, "ciox"); x->cioy = (int)obs_data_get_int(s, "cioy"); x->cirot = (int)obs_data_get_int(s, "cirot");
}
static void kl_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "sides", 6.0); obs_data_set_default_int(s, "cs", 18); obs_data_set_default_double(s, "rot", 0.0); obs_data_set_default_int(s, "cr", 19);
    obs_data_set_default_double(s, "twist", 0.0); obs_data_set_default_int(s, "ct", 20); obs_data_set_default_double(s, "zoom", 1.0); obs_data_set_default_int(s, "cz", 21);
    obs_data_set_default_double(s, "ox", 0.0); obs_data_set_default_int(s, "cox", 10); obs_data_set_default_double(s, "oy", 0.0); obs_data_set_default_int(s, "coy", 17);
    obs_data_set_default_double(s, "iox", 0.0); obs_data_set_default_int(s, "ciox", 16); obs_data_set_default_double(s, "ioy", 0.0); obs_data_set_default_int(s, "cioy", 7); obs_data_set_default_double(s, "irot", 0.0); obs_data_set_default_int(s, "cirot", 12);
}
static void kl_rd(void* d, gs_effect_t*) {
    auto* x = (kl_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_sides"), (std::max)(x->sides + std::floor(m.get_cc(x->dev, x->ch, x->cs) * 31.0f), 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rot"), x->rot + (m.get_cc(x->dev, x->ch, x->cr) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_twist"), x->twist + (m.get_cc(x->dev, x->ch, x->ct) * 8.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.05f, x->zoom * (1.0f + m.get_cc(x->dev, x->ch, x->cz) * 3.0f) + (note_hit * 0.8f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_ox"), x->ox + (m.get_cc(x->dev, x->ch, x->cox) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_oy"), x->oy + (m.get_cc(x->dev, x->ch, x->coy) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_iox"), x->iox + (m.get_cc(x->dev, x->ch, x->ciox) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_ioy"), x->ioy + (m.get_cc(x->dev, x->ch, x->cioy) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_irot"), x->irot + (m.get_cc(x->dev, x->ch, x->cirot) * 360.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* kl_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Zoom Pump", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "sides", "Szegmensek", 1.0, 32.0, 1.0); obs_properties_add_int(p, "cs", "-> CC Szegmensek [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "rot", "Forgatas", -360.0, 360.0, 1.0); obs_properties_add_int(p, "cr", "-> CC Forgatas [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "twist", "Spiral Csavaras", -5.0, 5.0, 0.05); obs_properties_add_int(p, "ct", "-> CC Spiral [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "zoom", "Zoom", 0.1, 5.0, 0.05); obs_properties_add_int(p, "cz", "-> CC Zoom [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "ox", "Origo X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "cox", "-> CC Origo X [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "oy", "Origo Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "coy", "-> CC Origo Y [17]", 0, 127, 1);
    obs_properties_add_float_slider(p, "iox", "Belso Eltolas X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ciox", "-> CC Belso X [16]", 0, 127, 1);
    obs_properties_add_float_slider(p, "ioy", "Belso Eltolas Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "cioy", "-> CC Belso Y [7]", 0, 127, 1);
    obs_properties_add_float_slider(p, "irot", "Belso Forgatas", -360.0, 360.0, 1.0); obs_properties_add_int(p, "cirot", "-> CC Belso Forgatas [12]", 0, 127, 1);
    return p;
}

// =========================================================================
// 11. PINCHR
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

struct pn_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, pinch, rad, cx, cy; int cpinch, crad, ccx, ccy; };
static const char* pn_n(void*) { return "[VIZZable] PINCHR"; }
static void* pn_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (pn_d*)bzalloc(sizeof(pn_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->pinch = 2.0f; d->rad = 0.8f;
    d->cpinch = 18; d->crad = 20; d->ccx = 10; d->ccy = 17;
    obs_enter_graphics(); d->eff = gs_effect_create(pn_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void pn_ds(void* d) { auto* x = (pn_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void pn_up(void* d, obs_data_t* s) {
    auto* x = (pn_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->pinch = (float)obs_data_get_double(s, "pinch"); x->rad = (float)obs_data_get_double(s, "rad"); x->cx = (float)obs_data_get_double(s, "cx"); x->cy = (float)obs_data_get_double(s, "cy");
    x->cpinch = (int)obs_data_get_int(s, "cpinch"); x->crad = (int)obs_data_get_int(s, "crad"); x->ccx = (int)obs_data_get_int(s, "ccx"); x->ccy = (int)obs_data_get_int(s, "ccy");
}
static void pn_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "pinch", 2.0); obs_data_set_default_int(s, "cpinch", 18); obs_data_set_default_double(s, "rad", 0.8); obs_data_set_default_int(s, "crad", 20);
    obs_data_set_default_double(s, "cx", 0.0); obs_data_set_default_int(s, "ccx", 10); obs_data_set_default_double(s, "cy", 0.0); obs_data_set_default_int(s, "ccy", 17);
}
static void pn_rd(void* d, gs_effect_t*) {
    auto* x = (pn_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_pinch"), (std::max)(0.1f, x->pinch + (m.get_cc(x->dev, x->ch, x->cpinch) * 5.0f) + (note_hit * 2.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rad"), (std::max)(0.05f, x->rad + (m.get_cc(x->dev, x->ch, x->crad) * 2.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), x->cx + (m.get_cc(x->dev, x->ch, x->ccx) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), x->cy + (m.get_cc(x->dev, x->ch, x->ccy) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* pn_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Pinch Loket", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "pinch", "Pinch Erossege", 0.1, 8.0, 0.1); obs_properties_add_int(p, "cpinch", "-> CC Pinch [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "rad", "Sugar", 0.1, 3.0, 0.05); obs_properties_add_int(p, "crad", "-> CC Sugar [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cx", "Origo X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccx", "-> CC Origo X [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cy", "Origo Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccy", "-> CC Origo Y [17]", 0, 127, 1);
    return p;
}

// =========================================================================
// 12. PIXEL8R
// =========================================================================
static const char* px_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Point; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_res_x;\nuniform float p_res_y;\nuniform float p_poster;\nuniform float p_grid;\nuniform float p_warp;\nuniform float p_off_x;\nuniform float p_off_y;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 grid = float2(max(p_res_x, 4.0), max(p_res_y, 4.0));\n"
"    float2 uv = v.uv + float2(p_off_x, p_off_y);\n"
"    uv += sin(uv.yx * 15.0) * p_warp * 0.05;\n"
"    float2 p_uv = floor(uv * grid) / grid;\n"
"    float4 col = image.Sample(def_s, p_uv);\n"
"    if (p_poster > 1.5) col.rgb = floor(col.rgb * p_poster) / p_poster;\n"
"    float2 cell = frac(uv * grid);\n"
"    float edge = step(cell.x, p_grid) + step(1.0 - p_grid, cell.x) + step(cell.y, p_grid) + step(1.0 - p_grid, cell.y);\n"
"    col.rgb *= (1.0 - saturate(edge) * 0.7);\n"
"    return col;\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct px_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, rx, ry, poster, grid, warp, offx, offy; int crx, cry, cposter, cgrid, cwarp, coffx, coffy; };
static const char* px_n(void*) { return "[VIZZable] PIXEL8R"; }
static void* px_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (px_d*)bzalloc(sizeof(px_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.0f; d->rx = 1920.0f; d->ry = 1080.0f;
    d->crx = 18; d->cry = 19; d->cposter = 20; d->cgrid = 21; d->cwarp = 16; d->coffx = 10; d->coffy = 17;
    obs_enter_graphics(); d->eff = gs_effect_create(px_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void px_ds(void* d) { auto* x = (px_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void px_up(void* d, obs_data_t* s) {
    auto* x = (px_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->rx = (float)obs_data_get_double(s, "rx"); x->ry = (float)obs_data_get_double(s, "ry"); x->poster = (float)obs_data_get_double(s, "poster"); x->grid = (float)obs_data_get_double(s, "grid");
    x->warp = (float)obs_data_get_double(s, "warp"); x->offx = (float)obs_data_get_double(s, "offx"); x->offy = (float)obs_data_get_double(s, "offy");
    x->crx = (int)obs_data_get_int(s, "crx"); x->cry = (int)obs_data_get_int(s, "cry"); x->cposter = (int)obs_data_get_int(s, "cposter"); x->cgrid = (int)obs_data_get_int(s, "cgrid");
    x->cwarp = (int)obs_data_get_int(s, "cwarp"); x->coffx = (int)obs_data_get_int(s, "coffx"); x->coffy = (int)obs_data_get_int(s, "coffy");
}
static void px_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.0);
    obs_data_set_default_double(s, "rx", 1920.0); obs_data_set_default_int(s, "crx", 18); obs_data_set_default_double(s, "ry", 1080.0); obs_data_set_default_int(s, "cry", 19);
    obs_data_set_default_double(s, "poster", 0.0); obs_data_set_default_int(s, "cposter", 20); obs_data_set_default_double(s, "grid", 0.0); obs_data_set_default_int(s, "cgrid", 21);
    obs_data_set_default_double(s, "warp", 0.0); obs_data_set_default_int(s, "cwarp", 16); obs_data_set_default_double(s, "offx", 0.0); obs_data_set_default_int(s, "coffx", 10); obs_data_set_default_double(s, "offy", 0.0); obs_data_set_default_int(s, "coffy", 17);
}
static void px_rd(void* d, gs_effect_t*) {
    auto* x = (px_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    float f_x = (std::max)(x->rx * (1.0f - (m.get_cc(x->dev, x->ch, x->crx) * 0.98f) - (note_hit * 0.85f)), 4.0f);
    float f_y = (std::max)(x->ry * (1.0f - (m.get_cc(x->dev, x->ch, x->cry) * 0.98f) - (note_hit * 0.85f)), 4.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_res_x"), f_x); gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_res_y"), f_y);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_poster"), x->poster + (m.get_cc(x->dev, x->ch, x->cposter) * 16.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_grid"), x->grid + (m.get_cc(x->dev, x->ch, x->cgrid) * 0.3f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_warp"), x->warp + (m.get_cc(x->dev, x->ch, x->cwarp) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_off_x"), x->offx + (m.get_cc(x->dev, x->ch, x->coffx) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_off_y"), x->offy + (m.get_cc(x->dev, x->ch, x->coffy) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* px_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Pixel Crush", 0.0, 1.0, 0.02);
    obs_properties_add_float_slider(p, "rx", "Felbontas X", 4.0, 1920.0, 2.0); obs_properties_add_int(p, "crx", "-> CC Pixel X [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "ry", "Felbontas Y", 4.0, 1080.0, 2.0); obs_properties_add_int(p, "cry", "-> CC Pixel Y [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "poster", "Bit-depth", 0.0, 16.0, 1.0); obs_properties_add_int(p, "cposter", "-> CC Bit-depth [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "grid", "Racsvonalak", 0.0, 0.3, 0.01); obs_properties_add_int(p, "cgrid", "-> CC Racsvonal [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "warp", "Torzitas", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cwarp", "-> CC Torzitas [16]", 0, 127, 1);
    obs_properties_add_float_slider(p, "offx", "Eltolas X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "coffx", "-> CC Eltolas X [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "offy", "Eltolas Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "coffy", "-> CC Eltolas Y [17]", 0, 127, 1);
    return p;
}

// =========================================================================
// 13. RGBR
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

struct rg_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, shift, angle, mx; int cshift, cangle, cmx; };
static const char* rg_n(void*) { return "[VIZZable] RGBR"; }
static void* rg_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (rg_d*)bzalloc(sizeof(rg_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->shift = 0.0f; d->mx = 1.0f;
    d->cshift = 18; d->cangle = 19; d->cmx = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(rg_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void rg_ds(void* d) { auto* x = (rg_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void rg_up(void* d, obs_data_t* s) {
    auto* x = (rg_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->shift = (float)obs_data_get_double(s, "shift"); x->angle = (float)obs_data_get_double(s, "angle"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cshift = (int)obs_data_get_int(s, "cshift"); x->cangle = (int)obs_data_get_int(s, "cangle"); x->cmx = (int)obs_data_get_int(s, "cmx");
}
static void rg_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "shift", 0.0); obs_data_set_default_int(s, "cshift", 18); obs_data_set_default_double(s, "angle", 0.0); obs_data_set_default_int(s, "cangle", 19); obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmx", 7);
}
static void rg_rd(void* d, gs_effect_t*) {
    auto* x = (rg_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_shift"), x->shift + (m.get_cc(x->dev, x->ch, x->cshift) * 3.0f) + (note_hit * 1.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_angle"), x->angle + (m.get_cc(x->dev, x->ch, x->cangle) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx + (m.get_cc(x->dev, x->ch, x->cmx) * 1.0f), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* rg_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes RGB Shift", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "shift", "RGB Szetszoras", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cshift", "-> CC Shift [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "angle", "Irany", -180.0, 180.0, 1.0); obs_properties_add_int(p, "cangle", "-> CC Irany [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "mx", "Mix", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cmx", "-> CC Mix [7]", 0, 127, 1);
    return p;
}

// =========================================================================
// 14. SCANLINES
// =========================================================================
static const char* sc_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_lines;\nuniform float p_dark;\nuniform float p_noise;\nuniform float p_roll;\nuniform float p_time;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float2 uv = v.uv;\n"
"    float s_line = sin((uv.y + p_time * p_roll * 0.05) * p_lines * 3.14159);\n"
"    float4 col = image.Sample(def_s, uv);\n"
"    col.rgb *= (1.0 - saturate(s_line * 0.5 + 0.5) * p_dark);\n"
"    float rand_n = frac(sin(dot(uv + float2(p_time * 0.1, p_time * 0.2), float2(12.9898, 78.233))) * 43758.5453);\n"
"    col.rgb += (rand_n - 0.5) * p_noise;\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct sc_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, lines, dark, noise, roll; int clines, cdark, cnoise, croll; float time; };
static const char* sc_n(void*) { return "[VIZZable] SCANLINES"; }
static void* sc_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (sc_d*)bzalloc(sizeof(sc_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.0f; d->lines = 200.0f; d->dark = 0.5f;
    d->clines = 18; d->cdark = 21; d->cnoise = 20; d->croll = 19;
    obs_enter_graphics(); d->eff = gs_effect_create(sc_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void sc_ds(void* d) { auto* x = (sc_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void sc_up(void* d, obs_data_t* s) {
    auto* x = (sc_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->lines = (float)obs_data_get_double(s, "lines"); x->dark = (float)obs_data_get_double(s, "dark"); x->noise = (float)obs_data_get_double(s, "noise"); x->roll = (float)obs_data_get_double(s, "roll");
    x->clines = (int)obs_data_get_int(s, "clines"); x->cdark = (int)obs_data_get_int(s, "cdark"); x->cnoise = (int)obs_data_get_int(s, "cnoise"); x->croll = (int)obs_data_get_int(s, "croll");
}
static void sc_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.0);
    obs_data_set_default_double(s, "lines", 200.0); obs_data_set_default_int(s, "clines", 18); obs_data_set_default_double(s, "dark", 0.5); obs_data_set_default_int(s, "cdark", 21);
    obs_data_set_default_double(s, "noise", 0.0); obs_data_set_default_int(s, "cnoise", 20); obs_data_set_default_double(s, "roll", 0.0); obs_data_set_default_int(s, "croll", 19);
}
static void sc_rd(void* d, gs_effect_t*) {
    auto* x = (sc_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->time += 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_lines"), x->lines + (m.get_cc(x->dev, x->ch, x->clines) * 600.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_dark"), (std::clamp)(x->dark + (m.get_cc(x->dev, x->ch, x->cdark) * 0.8f), 0.0f, 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_noise"), x->noise + (m.get_cc(x->dev, x->ch, x->cnoise) * 0.6f) + (note_hit * 0.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_roll"), x->roll + (m.get_cc(x->dev, x->ch, x->croll) * 10.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_time"), x->time);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* sc_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Zaj", 0.0, 2.0, 0.02);
    obs_properties_add_float_slider(p, "lines", "Sorok", 10.0, 800.0, 5.0); obs_properties_add_int(p, "clines", "-> CC Sorok [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "dark", "Sotetseg", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cdark", "-> CC Sotetseg [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "noise", "VHS Zaj", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cnoise", "-> CC Zaj [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "roll", "Gorgesi Sebesseg", -10.0, 10.0, 0.1); obs_properties_add_int(p, "croll", "-> CC Gorgetes [19]", 0, 127, 1);
    return p;
}

// =========================================================================
// 15. SLICR
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

struct sl_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, amt, bands, vert; int camt, cbands, cvert; };
static const char* sl_n(void*) { return "[VIZZable] SLICR"; }
static void* sl_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (sl_d*)bzalloc(sizeof(sl_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->bands = 10.0f;
    d->camt = 18; d->cbands = 20; d->cvert = 19;
    obs_enter_graphics(); d->eff = gs_effect_create(sl_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void sl_ds(void* d) { auto* x = (sl_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void sl_up(void* d, obs_data_t* s) {
    auto* x = (sl_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->amt = (float)obs_data_get_double(s, "amt"); x->bands = (float)obs_data_get_double(s, "bands"); x->vert = (float)obs_data_get_double(s, "vert");
    x->camt = (int)obs_data_get_int(s, "camt"); x->cbands = (int)obs_data_get_int(s, "cbands"); x->cvert = (int)obs_data_get_int(s, "cvert");
}
static void sl_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "amt", 0.0); obs_data_set_default_int(s, "camt", 18); obs_data_set_default_double(s, "bands", 10.0); obs_data_set_default_int(s, "cbands", 20); obs_data_set_default_double(s, "vert", 0.0); obs_data_set_default_int(s, "cvert", 19);
}
static void sl_rd(void* d, gs_effect_t*) {
    auto* x = (sl_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_slice_amt"), x->amt + (m.get_cc(x->dev, x->ch, x->camt) * 3.0f) + (note_hit * 1.5f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bands"), (std::max)(2.0f, x->bands + (m.get_cc(x->dev, x->ch, x->cbands) * 40.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_vert"), std::fmod(x->vert + m.get_cc(x->dev, x->ch, x->cvert), 2.0f) > 0.5f ? 1.0f : 0.0f);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* sl_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Szeleteles", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "amt", "Szelet Eltolas", 0.0, 3.0, 0.05); obs_properties_add_int(p, "camt", "-> CC Eltolas [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "bands", "Szeletek Szama", 2.0, 50.0, 1.0); obs_properties_add_int(p, "cbands", "-> CC Szeletszam [20]", 0, 127, 1);
    obs_properties_add_bool(p, "vert", "Fuggoleges Szeleteles"); obs_properties_add_int(p, "cvert", "-> CC Irany [19]", 0, 127, 1);
    return p;
}

// =========================================================================
// 16. SPRINKLR
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

struct sp_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, density, speed, mono, mx; int cdensity, cspeed, cmono, cmx; float time; };
static const char* sp_n(void*) { return "[VIZZable] SPRINKLR"; }
static void* sp_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (sp_d*)bzalloc(sizeof(sp_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->density = 0.1f; d->speed = 1.0f; d->mx = 1.0f;
    d->cdensity = 18; d->cspeed = 20; d->cmono = 19; d->cmx = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(sp_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void sp_ds(void* d) { auto* x = (sp_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void sp_up(void* d, obs_data_t* s) {
    auto* x = (sp_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->density = (float)obs_data_get_double(s, "density"); x->speed = (float)obs_data_get_double(s, "speed"); x->mono = (float)obs_data_get_double(s, "mono"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cdensity = (int)obs_data_get_int(s, "cdensity"); x->cspeed = (int)obs_data_get_int(s, "cspeed"); x->cmono = (int)obs_data_get_int(s, "cmono"); x->cmx = (int)obs_data_get_int(s, "cmx");
}
static void sp_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "density", 0.1); obs_data_set_default_int(s, "cdensity", 18); obs_data_set_default_double(s, "speed", 1.0); obs_data_set_default_int(s, "cspeed", 20);
    obs_data_set_default_double(s, "mono", 0.0); obs_data_set_default_int(s, "cmono", 19); obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmx", 7);
}
static void sp_rd(void* d, gs_effect_t*) {
    auto* x = (sp_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->time += 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_density"), (std::clamp)(x->density + (m.get_cc(x->dev, x->ch, x->cdensity) * 0.8f) + (note_hit * 0.5f), 0.0f, 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_speed"), x->speed + (m.get_cc(x->dev, x->ch, x->cspeed) * 5.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mono"), std::fmod(x->mono + m.get_cc(x->dev, x->ch, x->cmono), 2.0f) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx + (m.get_cc(x->dev, x->ch, x->cmx) * 1.0f), 0.0f, 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_time"), x->time);
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* sp_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Sparkle Loket", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "density", "Szemcse Suruseg", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cdensity", "-> CC Suruseg [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "speed", "Zaj Sebesseg", 0.0, 10.0, 0.1); obs_properties_add_int(p, "cspeed", "-> CC Sebesseg [20]", 0, 127, 1);
    obs_properties_add_bool(p, "mono", "Fekete-Feher Szemcse"); obs_properties_add_int(p, "cmono", "-> CC Mono [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "mx", "Mix", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cmx", "-> CC Mix [7]", 0, 127, 1);
    return p;
}

// =========================================================================
// 17. STROBR
// =========================================================================
static const char* st_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Clamp; AddressV = Clamp; };\n"
"uniform float p_inv_r;\nuniform float p_inv_g;\nuniform float p_inv_b;\nuniform float p_master_inv;\nuniform float p_flash;\nuniform float p_swap;\nuniform float p_mix;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float4 orig = image.Sample(def_s, v.uv);\n"
"    float4 col = orig;\n"
"    if (p_swap > 1.5) col.rgb = col.brg;\n"
"    else if (p_swap > 0.5) col.rgb = col.gbr;\n"
"    col.r = abs(p_inv_r - col.r); col.g = abs(p_inv_g - col.g); col.b = abs(p_inv_b - col.b);\n"
"    col.rgb = abs(p_master_inv - col.rgb) + p_flash;\n"
"    return lerp(orig, float4(saturate(col.rgb), orig.a), p_mix);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct st_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, ir, ig, ib, minv, flash, swap, mx; int cir, cig, cib, cminv, cflash, cswap, cmx; };
static const char* st_n(void*) { return "[VIZZable] STROBR"; }
static void* st_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (st_d*)bzalloc(sizeof(st_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 1.0f; d->mx = 1.0f;
    d->cir = 18; d->cig = 10; d->cib = 20; d->cminv = 21; d->cflash = 16; d->cswap = 17; d->cmx = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(st_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void st_ds(void* d) { auto* x = (st_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void st_up(void* d, obs_data_t* s) {
    auto* x = (st_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->ir = (float)obs_data_get_double(s, "ir"); x->ig = (float)obs_data_get_double(s, "ig"); x->ib = (float)obs_data_get_double(s, "ib"); x->minv = (float)obs_data_get_double(s, "minv");
    x->flash = (float)obs_data_get_double(s, "flash"); x->swap = (float)obs_data_get_double(s, "swap"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cir = (int)obs_data_get_int(s, "cir"); x->cig = (int)obs_data_get_int(s, "cig"); x->cib = (int)obs_data_get_int(s, "cib"); x->cminv = (int)obs_data_get_int(s, "cminv");
    x->cflash = (int)obs_data_get_int(s, "cflash"); x->cswap = (int)obs_data_get_int(s, "cswap"); x->cmx = (int)obs_data_get_int(s, "cmx");
}
static void st_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 1.0);
    obs_data_set_default_double(s, "ir", 0.0); obs_data_set_default_int(s, "cir", 18); obs_data_set_default_double(s, "ig", 0.0); obs_data_set_default_int(s, "cig", 10);
    obs_data_set_default_double(s, "ib", 0.0); obs_data_set_default_int(s, "cib", 20); obs_data_set_default_double(s, "minv", 0.0); obs_data_set_default_int(s, "cminv", 21);
    obs_data_set_default_double(s, "flash", 0.0); obs_data_set_default_int(s, "cflash", 16); obs_data_set_default_double(s, "swap", 0.0); obs_data_set_default_int(s, "cswap", 17);
    obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmx", 7);
}
static void st_rd(void* d, gs_effect_t*) {
    auto* x = (st_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_inv_r"), std::fmod(x->ir + m.get_cc(x->dev, x->ch, x->cir) * 2.0f, 2.0f) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_inv_g"), std::fmod(x->ig + m.get_cc(x->dev, x->ch, x->cig) * 2.0f, 2.0f) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_inv_b"), std::fmod(x->ib + m.get_cc(x->dev, x->ch, x->cib) * 2.0f, 2.0f) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_master_inv"), std::fmod(x->minv + m.get_cc(x->dev, x->ch, x->cminv) * 2.0f, 2.0f) > 0.5f ? 1.0f : 0.0f);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_flash"), x->flash + (m.get_cc(x->dev, x->ch, x->cflash) * 1.5f) + (note_hit * 1.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_swap"), std::floor(x->swap + (m.get_cc(x->dev, x->ch, x->cswap) * 3.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx + (m.get_cc(x->dev, x->ch, x->cmx) * 1.0f), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* st_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Flash", 0.0, 2.0, 0.05);
    obs_properties_add_bool(p, "ir", "Piros Inverz"); obs_properties_add_int(p, "cir", "-> CC Piros [18]", 0, 127, 1);
    obs_properties_add_bool(p, "ig", "Zold Inverz"); obs_properties_add_int(p, "cig", "-> CC Zold [10]", 0, 127, 1);
    obs_properties_add_bool(p, "ib", "Kek Inverz"); obs_properties_add_int(p, "cib", "-> CC Kek [20]", 0, 127, 1);
    obs_properties_add_bool(p, "minv", "Master Inverz"); obs_properties_add_int(p, "cminv", "-> CC Master [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "flash", "Feher Flash Villanas", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cflash", "-> CC Flash [16]", 0, 127, 1);
    obs_properties_add_float_slider(p, "swap", "RGB Csatornacseere", 0.0, 2.0, 1.0); obs_properties_add_int(p, "cswap", "-> CC Csatornacseere [17]", 0, 127, 1);
    obs_properties_add_float_slider(p, "mx", "Mix", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cmx", "-> CC Mix [7]", 0, 127, 1);
    return p;
}

// =========================================================================
// 18. TWISTR
// =========================================================================
static const char* tw_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_angle;\nuniform float p_rad;\nuniform float p_cx;\nuniform float p_cy;\nuniform float p_tile;\n"
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
"    uv = uv * p_tile + center;\n"
"    return image.Sample(def_s, uv);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct tw_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, angle, rad, cx, cy, tile; int cangle, crad, ccx, ccy, ctile; };
static const char* tw_n(void*) { return "[VIZZable] TWISTR"; }
static void* tw_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (tw_d*)bzalloc(sizeof(tw_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->angle = 2.0f; d->rad = 0.7f; d->tile = 1.0f;
    d->cangle = 18; d->crad = 20; d->ccx = 10; d->ccy = 17; d->ctile = 21;
    obs_enter_graphics(); d->eff = gs_effect_create(tw_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void tw_ds(void* d) { auto* x = (tw_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void tw_up(void* d, obs_data_t* s) {
    auto* x = (tw_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->angle = (float)obs_data_get_double(s, "angle"); x->rad = (float)obs_data_get_double(s, "rad"); x->cx = (float)obs_data_get_double(s, "cx"); x->cy = (float)obs_data_get_double(s, "cy"); x->tile = (float)obs_data_get_double(s, "tile");
    x->cangle = (int)obs_data_get_int(s, "cangle"); x->crad = (int)obs_data_get_int(s, "crad"); x->ccx = (int)obs_data_get_int(s, "ccx"); x->ccy = (int)obs_data_get_int(s, "ccy"); x->ctile = (int)obs_data_get_int(s, "ctile");
}
static void tw_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "angle", 2.0); obs_data_set_default_int(s, "cangle", 18); obs_data_set_default_double(s, "rad", 0.7); obs_data_set_default_int(s, "crad", 20);
    obs_data_set_default_double(s, "cx", 0.0); obs_data_set_default_int(s, "ccx", 10); obs_data_set_default_double(s, "cy", 0.0); obs_data_set_default_int(s, "ccy", 17);
    obs_data_set_default_double(s, "tile", 1.0); obs_data_set_default_int(s, "ctile", 21);
}
static void tw_rd(void* d, gs_effect_t*) {
    auto* x = (tw_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_angle"), x->angle + ((m.get_cc(x->dev, x->ch, x->cangle) - 0.5f) * 10.0f) + (note_hit * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rad"), (std::max)(0.05f, x->rad + (m.get_cc(x->dev, x->ch, x->crad) * 2.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), x->cx + (m.get_cc(x->dev, x->ch, x->ccx) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), x->cy + (m.get_cc(x->dev, x->ch, x->ccy) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_tile"), (std::max)(0.1f, x->tile + (m.get_cc(x->dev, x->ch, x->ctile) * 4.0f)));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* tw_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Orveny", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "angle", "Csavarasi Szog", -10.0, 10.0, 0.1); obs_properties_add_int(p, "cangle", "-> CC Szog [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "rad", "Orveny Sugar", 0.05, 3.0, 0.05); obs_properties_add_int(p, "crad", "-> CC Sugar [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cx", "Origo X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccx", "-> CC Origo X [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cy", "Origo Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccy", "-> CC Origo Y [17]", 0, 127, 1);
    obs_properties_add_float_slider(p, "tile", "Csempezes", 0.1, 5.0, 0.05); obs_properties_add_int(p, "ctile", "-> CC Csempezes [21]", 0, 127, 1);
    return p;
}

// =========================================================================
// 19. ZOROPR
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

struct zo_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, angle, slit, zoom, cx, cy; int cangle, cslit, czoom, ccx, ccy; };
static const char* zo_n(void*) { return "[VIZZable] ZOROPR"; }
static void* zo_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (zo_d*)bzalloc(sizeof(zo_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->zoom = 1.0f;
    d->cangle = 18; d->cslit = 20; d->czoom = 21; d->ccx = 10; d->ccy = 17;
    obs_enter_graphics(); d->eff = gs_effect_create(zo_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void zo_ds(void* d) { auto* x = (zo_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void zo_up(void* d, obs_data_t* s) {
    auto* x = (zo_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->angle = (float)obs_data_get_double(s, "angle"); x->slit = (float)obs_data_get_double(s, "slit"); x->zoom = (float)obs_data_get_double(s, "zoom"); x->cx = (float)obs_data_get_double(s, "cx"); x->cy = (float)obs_data_get_double(s, "cy");
    x->cangle = (int)obs_data_get_int(s, "cangle"); x->cslit = (int)obs_data_get_int(s, "cslit"); x->czoom = (int)obs_data_get_int(s, "czoom"); x->ccx = (int)obs_data_get_int(s, "ccx"); x->ccy = (int)obs_data_get_int(s, "ccy");
}
static void zo_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "angle", 45.0); obs_data_set_default_int(s, "cangle", 18); obs_data_set_default_double(s, "slit", 0.0); obs_data_set_default_int(s, "cslit", 20);
    obs_data_set_default_double(s, "zoom", 1.0); obs_data_set_default_int(s, "czoom", 21); obs_data_set_default_double(s, "cx", 0.0); obs_data_set_default_int(s, "ccx", 10); obs_data_set_default_double(s, "cy", 0.0); obs_data_set_default_int(s, "ccy", 17);
}
static void zo_rd(void* d, gs_effect_t*) {
    auto* x = (zo_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_angle"), x->angle + (m.get_cc(x->dev, x->ch, x->cangle) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_slit"), x->slit + (m.get_cc(x->dev, x->ch, x->cslit) * 2.0f) + (note_hit * 0.8f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.1f, x->zoom * (1.0f + m.get_cc(x->dev, x->ch, x->czoom) * 3.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cx"), x->cx + (m.get_cc(x->dev, x->ch, x->ccx) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_cy"), x->cy + (m.get_cc(x->dev, x->ch, x->ccy) * 2.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* zo_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Vagas Loket", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "angle", "Vagas Szog", -180.0, 180.0, 1.0); obs_properties_add_int(p, "cangle", "-> CC Szog [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "slit", "Res Szelesseg", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cslit", "-> CC Res [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "zoom", "Zoom", 0.1, 5.0, 0.05); obs_properties_add_int(p, "czoom", "-> CC Zoom [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cx", "Origo X", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccx", "-> CC Origo X [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "cy", "Origo Y", -2.0, 2.0, 0.05); obs_properties_add_int(p, "ccy", "-> CC Origo Y [17]", 0, 127, 1);
    return p;
}

// =========================================================================
// 20. DRTYFEEDR
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

struct df_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, drive, bleed, zoom, rot, mx; int cdrive, cbleed, czoom, crot, cmx; };
static const char* df_n(void*) { return "[VIZZable] DRTYFEEDR"; }
static void* df_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (df_d*)bzalloc(sizeof(df_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->zoom = 1.0f; d->mx = 1.0f;
    d->cdrive = 18; d->cbleed = 20; d->czoom = 21; d->crot = 19; d->cmx = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(df_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void df_ds(void* d) { auto* x = (df_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void df_up(void* d, obs_data_t* s) {
    auto* x = (df_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->drive = (float)obs_data_get_double(s, "drive"); x->bleed = (float)obs_data_get_double(s, "bleed"); x->zoom = (float)obs_data_get_double(s, "zoom"); x->rot = (float)obs_data_get_double(s, "rot"); x->mx = (float)obs_data_get_double(s, "mx");
    x->cdrive = (int)obs_data_get_int(s, "cdrive"); x->cbleed = (int)obs_data_get_int(s, "cbleed"); x->czoom = (int)obs_data_get_int(s, "czoom"); x->crot = (int)obs_data_get_int(s, "crot"); x->cmx = (int)obs_data_get_int(s, "cmx");
}
static void df_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "drive", 0.0); obs_data_set_default_int(s, "cdrive", 18); obs_data_set_default_double(s, "bleed", 0.0); obs_data_set_default_int(s, "cbleed", 20);
    obs_data_set_default_double(s, "zoom", 1.0); obs_data_set_default_int(s, "czoom", 21); obs_data_set_default_double(s, "rot", 0.0); obs_data_set_default_int(s, "crot", 19); obs_data_set_default_double(s, "mx", 1.0); obs_data_set_default_int(s, "cmx", 7);
}
static void df_rd(void* d, gs_effect_t*) {
    auto* x = (df_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_drive"), x->drive + (m.get_cc(x->dev, x->ch, x->cdrive) * 4.0f) + (note_hit * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_bleed"), x->bleed + (m.get_cc(x->dev, x->ch, x->cbleed) * 3.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.1f, x->zoom * (1.0f + m.get_cc(x->dev, x->ch, x->czoom) * 2.0f) - (note_hit * 0.3f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rot"), x->rot + (m.get_cc(x->dev, x->ch, x->crot) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_mix"), (std::clamp)(x->mx + (m.get_cc(x->dev, x->ch, x->cmx) * 1.0f), 0.0f, 1.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* df_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Overdrive Flash", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "drive", "Saturacio / Overdrive", 0.0, 4.0, 0.05); obs_properties_add_int(p, "cdrive", "-> CC Drive [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "bleed", "Szinverszes (Bleed)", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cbleed", "-> CC Bleed [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "zoom", "Drift Zoom", 0.1, 4.0, 0.05); obs_properties_add_int(p, "czoom", "-> CC Zoom [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "rot", "Drift Forgatas", -180.0, 180.0, 1.0); obs_properties_add_int(p, "crot", "-> CC Forgatas [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "mx", "Mix", 0.0, 1.0, 0.02); obs_properties_add_int(p, "cmx", "-> CC Mix [7]", 0, 127, 1);
    return p;
}

// =========================================================================
// 21. FEEDR & BUFFR
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
"    for (int i = 1; i <= 5; ++i) {\n"
"        float fi = float(i);\n"
"        float z = pow(p_zoom, fi * 0.3);\n"
"        float2 r_uv = float2(uv.x * cosA - uv.y * sinA, uv.x * sinA + uv.y * cosA) * z + 0.5;\n"
"        float w = pow(p_decay, fi);\n"
"        float4 echo = image.Sample(def_s, r_uv);\n"
"        col = max(col, echo * w);\n"
"    }\n"
"    return float4(saturate(col.rgb), col.a);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct fd_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, decay, rot, zoom, chroma; int cdecay, crot, czoom, cchroma; };
static const char* fd_n(void*) { return "[VIZZable] FEEDR & BUFFR"; }
static void* fd_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (fd_d*)bzalloc(sizeof(fd_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->decay = 0.8f; d->zoom = 1.05f;
    d->cdecay = 18; d->crot = 19; d->czoom = 20; d->cchroma = 21;
    obs_enter_graphics(); d->eff = gs_effect_create(fd_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void fd_ds(void* d) { auto* x = (fd_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void fd_up(void* d, obs_data_t* s) {
    auto* x = (fd_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->decay = (float)obs_data_get_double(s, "decay"); x->rot = (float)obs_data_get_double(s, "rot"); x->zoom = (float)obs_data_get_double(s, "zoom"); x->chroma = (float)obs_data_get_double(s, "chroma");
    x->cdecay = (int)obs_data_get_int(s, "cdecay"); x->crot = (int)obs_data_get_int(s, "crot"); x->czoom = (int)obs_data_get_int(s, "czoom"); x->cchroma = (int)obs_data_get_int(s, "cchroma");
}
static void fd_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "decay", 0.8); obs_data_set_default_int(s, "cdecay", 18); obs_data_set_default_double(s, "rot", 0.0); obs_data_set_default_int(s, "crot", 19);
    obs_data_set_default_double(s, "zoom", 1.05); obs_data_set_default_int(s, "czoom", 20); obs_data_set_default_double(s, "chroma", 0.0); obs_data_set_default_int(s, "cchroma", 21);
}
static void fd_rd(void* d, gs_effect_t*) {
    auto* x = (fd_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_decay"), (std::clamp)(x->decay + (m.get_cc(x->dev, x->ch, x->cdecay) * 0.3f) + (note_hit * 0.3f), 0.0f, 0.98f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_rot"), x->rot + (m.get_cc(x->dev, x->ch, x->crot) * 90.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_zoom"), (std::max)(0.5f, x->zoom + ((m.get_cc(x->dev, x->ch, x->czoom) - 0.5f) * 1.0f)));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_chroma"), x->chroma + (m.get_cc(x->dev, x->ch, x->cchroma) * 3.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* fd_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Feedback Echo", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "decay", "Visszacsatolas Lecsenges", 0.0, 0.98, 0.02); obs_properties_add_int(p, "cdecay", "-> CC Lecsenges [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "rot", "Echo Forgatas", -90.0, 90.0, 1.0); obs_properties_add_int(p, "crot", "-> CC Forgatas [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "zoom", "Echo Zoom", 0.5, 2.0, 0.02); obs_properties_add_int(p, "czoom", "-> CC Zoom [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "chroma", "Kromatikus Szetszoras", 0.0, 3.0, 0.05); obs_properties_add_int(p, "cchroma", "-> CC Kromatika [21]", 0, 127, 1);
    return p;
}

// =========================================================================
// 22. WAVR
// =========================================================================
static const char* wv_code = 
"uniform float4x4 ViewProj;\nuniform texture2d image;\nsampler_state def_s { Filter = Linear; AddressU = Mirror; AddressV = Mirror; };\n"
"uniform float p_amp_x;\nuniform float p_amp_y;\nuniform float p_freq_x;\nuniform float p_freq_y;\nuniform float p_speed;\nuniform float p_deform;\nuniform float p_angle;\nuniform float p_chroma;\n"
"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"VertData VS(VertData v) { VertData o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
"float4 PS(VertData v) : TARGET {\n"
"    float rad = p_angle * 0.01745329;\n"
"    float cosA = cos(rad); float sinA = sin(rad);\n"
"    float2 uv_rot = float2((v.uv.x - 0.5) * cosA - (v.uv.y - 0.5) * sinA, (v.uv.x - 0.5) * sinA + (v.uv.y - 0.5) * cosA) + 0.5;\n"
"    float wave_x = sin(uv_rot.y * p_freq_y + p_speed);\n"
"    float wave_y = cos(uv_rot.x * p_freq_x + p_speed);\n"
"    if (p_deform > 0.01) {\n"
"        wave_x = lerp(wave_x, tan(frac(uv_rot.y * p_freq_y * 0.3 + p_speed * 0.3) * 3.14 - 1.57), p_deform * 0.3);\n"
"        wave_y = lerp(wave_y, tan(frac(uv_rot.x * p_freq_x * 0.3 + p_speed * 0.3) * 3.14 - 1.57), p_deform * 0.3);\n"
"    }\n"
"    float2 disp = float2(wave_x * p_amp_x, wave_y * p_amp_y);\n"
"    float2 uv_main = v.uv + disp;\n"
"    if (p_chroma > 0.001) {\n"
"        float2 uv_r = v.uv + disp * (1.0 + p_chroma * 0.5);\n"
"        float2 uv_b = v.uv + disp * (1.0 - p_chroma * 0.5);\n"
"        float r = image.Sample(def_s, uv_r).r;\n"
"        float g = image.Sample(def_s, uv_main).g;\n"
"        float b = image.Sample(def_s, uv_b).b;\n"
"        return float4(r, g, b, 1.0);\n"
"    }\n"
"    return image.Sample(def_s, uv_main);\n"
"}\ntechnique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

struct wv_d { obs_source_t* ctx; gs_effect_t* eff; int dev, ch; float n_int, ax, ay, fx, fy, sp, deform, angle, chroma; int cax, cay, cfx, cfy, cdeform, cangle, cchroma; float phase; };
static const char* wv_n(void*) { return "[VIZZable] WAVR"; }
static void* wv_cr(obs_data_t* s, obs_source_t* c) {
    auto* d = (wv_d*)bzalloc(sizeof(wv_d)); d->ctx = c; d->dev = -1; d->ch = 0; d->n_int = 0.5f; d->ax = 0.02f; d->ay = 0.02f; d->fx = 10.0f; d->fy = 10.0f; d->sp = 1.0f;
    d->cax = 18; d->cay = 20; d->cfx = 19; d->cfy = 21; d->cdeform = 16; d->cangle = 10; d->cchroma = 7;
    obs_enter_graphics(); d->eff = gs_effect_create(wv_code, NULL, NULL); obs_leave_graphics();
    obs_source_update(c, s); return d;
}
static void wv_ds(void* d) { auto* x = (wv_d*)d; if (x->eff) { obs_enter_graphics(); gs_effect_destroy(x->eff); obs_leave_graphics(); } bfree(x); }
static void wv_up(void* d, obs_data_t* s) {
    auto* x = (wv_d*)d; x->dev = (int)obs_data_get_int(s, "dev"); x->ch = (int)obs_data_get_int(s, "ch"); x->n_int = (float)obs_data_get_double(s, "n_int");
    x->ax = (float)obs_data_get_double(s, "ax"); x->ay = (float)obs_data_get_double(s, "ay"); x->fx = (float)obs_data_get_double(s, "fx"); x->fy = (float)obs_data_get_double(s, "fy");
    x->sp = (float)obs_data_get_double(s, "sp"); x->deform = (float)obs_data_get_double(s, "deform"); x->angle = (float)obs_data_get_double(s, "angle"); x->chroma = (float)obs_data_get_double(s, "chroma");
    x->cax = (int)obs_data_get_int(s, "cax"); x->cay = (int)obs_data_get_int(s, "cay"); x->cfx = (int)obs_data_get_int(s, "cfx"); x->cfy = (int)obs_data_get_int(s, "cfy");
    x->cdeform = (int)obs_data_get_int(s, "cdeform"); x->cangle = (int)obs_data_get_int(s, "cangle"); x->cchroma = (int)obs_data_get_int(s, "cchroma");
}
static void wv_def(obs_data_t* s) {
    obs_data_set_default_int(s, "dev", -1); obs_data_set_default_int(s, "ch", 0); obs_data_set_default_double(s, "n_int", 0.5);
    obs_data_set_default_double(s, "ax", 0.02); obs_data_set_default_int(s, "cax", 18); obs_data_set_default_double(s, "ay", 0.02); obs_data_set_default_int(s, "cay", 20);
    obs_data_set_default_double(s, "fx", 10.0); obs_data_set_default_int(s, "cfx", 19); obs_data_set_default_double(s, "fy", 10.0); obs_data_set_default_int(s, "cfy", 21);
    obs_data_set_default_double(s, "sp", 1.0); obs_data_set_default_double(s, "deform", 0.0); obs_data_set_default_int(s, "cdeform", 16);
    obs_data_set_default_double(s, "angle", 0.0); obs_data_set_default_int(s, "cangle", 10); obs_data_set_default_double(s, "chroma", 0.0); obs_data_set_default_int(s, "cchroma", 7);
}
static void wv_rd(void* d, gs_effect_t*) {
    auto* x = (wv_d*)d; if (!x->eff || !obs_source_process_filter_begin(x->ctx, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) return;
    auto& m = MidiCore::instance(); float note_hit = m.get_note(x->dev, x->ch) * x->n_int;
    x->phase += x->sp * 0.05f;
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_amp_x"), (x->ax + (m.get_cc(x->dev, x->ch, x->cax) * 0.4f)) + (note_hit * 0.3f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_amp_y"), (x->ay + (m.get_cc(x->dev, x->ch, x->cay) * 0.4f)) + (note_hit * 0.3f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_freq_x"), x->fx + (m.get_cc(x->dev, x->ch, x->cfx) * 60.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_freq_y"), x->fy + (m.get_cc(x->dev, x->ch, x->cfy) * 60.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_speed"), x->phase);
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_deform"), x->deform + (m.get_cc(x->dev, x->ch, x->cdeform) * 2.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_angle"), x->angle + (m.get_cc(x->dev, x->ch, x->cangle) * 360.0f));
    gs_effect_set_float(gs_effect_get_param_by_name(x->eff, "p_chroma"), x->chroma + (m.get_cc(x->dev, x->ch, x->cchroma) * 3.0f));
    obs_source_process_filter_end(x->ctx, x->eff, 0, 0);
}
static obs_properties_t* wv_pr(void*) {
    auto* p = obs_properties_create(); auto* p_dev = obs_properties_add_list(p, "dev", "MIDI Bemenet", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT); MidiCore::populate_devices(p_dev);
    obs_properties_add_int(p, "ch", "MIDI Csatorna (0 = Omni)", 0, 16, 1); obs_properties_add_float_slider(p, "n_int", "Note Utes Hullam Loket", 0.0, 3.0, 0.05);
    obs_properties_add_float_slider(p, "sp", "Hullamzas Sebesseg", -10.0, 10.0, 0.1);
    obs_properties_add_float_slider(p, "ax", "Amplitudo X", 0.0, 0.5, 0.01); obs_properties_add_int(p, "cax", "-> CC Amplitudo X [18]", 0, 127, 1);
    obs_properties_add_float_slider(p, "ay", "Amplitudo Y", 0.0, 0.5, 0.01); obs_properties_add_int(p, "cay", "-> CC Amplitudo Y [20]", 0, 127, 1);
    obs_properties_add_float_slider(p, "fx", "Frekvencia X", 1.0, 100.0, 0.5); obs_properties_add_int(p, "cfx", "-> CC Frekvencia X [19]", 0, 127, 1);
    obs_properties_add_float_slider(p, "fy", "Frekvencia Y", 1.0, 100.0, 0.5); obs_properties_add_int(p, "cfy", "-> CC Frekvencia Y [21]", 0, 127, 1);
    obs_properties_add_float_slider(p, "deform", "Deformacio", 0.0, 2.0, 0.02); obs_properties_add_int(p, "cdeform", "-> CC Deformacio [16]", 0, 127, 1);
    obs_properties_add_float_slider(p, "angle", "Hullam Doles", -180.0, 180.0, 1.0); obs_properties_add_int(p, "cangle", "-> CC Doles [10]", 0, 127, 1);
    obs_properties_add_float_slider(p, "chroma", "Kromatikus Szetszoras", 0.0, 2.0, 0.02); obs_properties_add_int(p, "cchroma", "-> CC Kromatika [7]", 0, 127, 1);
    return p;
}

static obs_source_info s_to, s_bl, s_br, s_brt, s_cm, s_cr, s_ex, s_fi, s_hs, s_kl, s_pn, s_px, s_rg, s_sc, s_sl, s_sp, s_st, s_tw, s_zo, s_df, s_fd, s_wv;

bool obs_module_load(void) {
    MidiCore::instance().init();
    s_to = reg_f("vz_2tonr", to_n, to_cr, to_ds, to_up, to_def, to_pr, to_rd); obs_register_source(&s_to);
    s_bl = reg_f("vz_blurr", bl_n, bl_cr, bl_ds, bl_up, bl_def, bl_pr, bl_rd); obs_register_source(&s_bl);
    s_br = reg_f("vz_brcosr", br_n, br_cr, br_ds, br_up, br_def, br_pr, br_rd); obs_register_source(&s_br);
    s_brt = reg_f("vz_breathr", brt_n, brt_cr, brt_ds, brt_up, brt_def, brt_pr, brt_rd); obs_register_source(&s_brt);
    s_cm = reg_f("vz_clrmapr", cm_n, cm_cr, cm_ds, cm_up, cm_def, cm_pr, cm_rd); obs_register_source(&s_cm);
    s_cr = reg_f("vz_cropr", cr_n, cr_cr, cr_ds, cr_up, cr_def, cr_pr, cr_rd); obs_register_source(&s_cr);
    s_ex = reg_f("vz_exposr", ex_n, ex_cr, ex_ds, ex_up, ex_def, ex_pr, ex_rd); obs_register_source(&s_ex);
    s_fi = reg_f("vz_fisheyr", fi_n, fi_cr, fi_ds, fi_up, fi_def, fi_pr, fi_rd); obs_register_source(&s_fi);
    s_hs = reg_f("vz_hueshiftr", hs_n, hs_cr, hs_ds, hs_up, hs_def, hs_pr, hs_rd); obs_register_source(&s_hs);
    s_kl = reg_f("vz_kaleidr", kl_n, kl_cr, kl_ds, kl_up, kl_def, kl_pr, kl_rd); obs_register_source(&s_kl);
    s_pn = reg_f("vz_pinchr", pn_n, pn_cr, pn_ds, pn_up, pn_def, pn_pr, pn_rd); obs_register_source(&s_pn);
    s_px = reg_f("vz_pixel8r", px_n, px_cr, px_ds, px_up, px_def, px_pr, px_rd); obs_register_source(&s_px);
    s_rg = reg_f("vz_rgbr", rg_n, rg_cr, rg_ds, rg_up, rg_def, rg_pr, rg_rd); obs_register_source(&s_rg);
    s_sc = reg_f("vz_scanlines", sc_n, sc_cr, sc_ds, sc_up, sc_def, sc_pr, sc_rd); obs_register_source(&s_sc);
    s_sl = reg_f("vz_slicr", sl_n, sl_cr, sl_ds, sl_up, sl_def, sl_pr, sl_rd); obs_register_source(&s_sl);
    s_sp = reg_f("vz_sprinklr", sp_n, sp_cr, sp_ds, sp_up, sp_def, sp_pr, sp_rd); obs_register_source(&s_sp);
    s_st = reg_f("vz_strobr", st_n, st_cr, st_ds, st_up, st_def, st_pr, st_rd); obs_register_source(&s_st);
    s_tw = reg_f("vz_twistr", tw_n, tw_cr, tw_ds, tw_up, tw_def, tw_pr, tw_rd); obs_register_source(&s_tw);
    s_zo = reg_f("vz_zoropr", zo_n, zo_cr, zo_ds, zo_up, zo_def, zo_pr, zo_rd); obs_register_source(&s_zo);
    s_df = reg_f("vz_drtyfeedr", df_n, df_cr, df_ds, df_up, df_def, df_pr, df_rd); obs_register_source(&s_df);
    s_fd = reg_f("vz_feedr", fd_n, fd_cr, fd_ds, fd_up, fd_def, fd_pr, fd_rd); obs_register_source(&s_fd);
    s_wv = reg_f("vz_wavr", wv_n, wv_cr, wv_ds, wv_up, wv_def, wv_pr, wv_rd); obs_register_source(&s_wv);
    return true;
}

void obs_module_unload(void) {
    MidiCore::instance().shutdown();
}
