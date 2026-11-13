// Private build: the upstream repository does not publish VoidOverlayShader.h (precompiled bytecode).
// This replacement compiles tiny shaders at runtime through d3dcompiler_47.dll, loaded dynamically so
// the DLL's import table stays system-only. Motes are gone; only the fullscreen blit used by the
// seamless dissolve is real. g_VoidPS is kept as a no-op (additive black) so the old paths still link.
#pragma once

#include <d3dcommon.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace overlayshaders
{
    inline constexpr const char* kVS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
)";

    inline constexpr const char* kVoidPS = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 main(VSOut i) : SV_Target { return float4(0.0, 0.0, 0.0, 0.0); }
)";

    inline constexpr const char* kBlitPS = R"(
cbuffer Params : register(b0)
{
    float2 resolution; float age; float phase;
    float phaseAge; float bright; float breath; float pulseStrength;
    float motes; float pulseMs; float moteScale; float blitAlpha;
};
Texture2D    snap : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 main(VSOut i) : SV_Target
{
    // [EaseIn] moteScale carries the zoom of the held picture (0 or 1 = none)
    float z = max(moteScale, 1.0);
    float3 c = snap.Sample(samp, (i.uv - 0.5) / z + 0.5).rgb;
    return float4(c, saturate(blitAlpha));
}
)";


    // ---- travel effects (private build): eyes / ink / magic. Same cbuffer as the blit shader.
    //  phase: 0 = building up, 1 = holding over the load, 2 = clearing.  bright: 0..1 progress of
    //  the current phase.  age: seconds since arming.  During phases 0/1 the snapshot (t0) is the
    //  picture underneath; during phase 2 the output alpha lets the LIVE frame through instead.
    inline constexpr const char* kFxCommon = R"(
cbuffer Params : register(b0)
{
    float2 resolution; float age; float phase;
    float phaseAge; float bright; float breath; float pulseStrength;
    float motes; float pulseMs; float moteScale; float blitAlpha;
};
Texture2D    snap : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float hash21(float2 p)
{
    // no short period: the lattice is scrambled through three coprime-ish dot products
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}
// smooth noise from a quintic-blended lattice: no visible cell edges
float snoise(float2 p)
{
    float2 i = floor(p); float2 f = frac(p); f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = hash21(i), b = hash21(i + float2(1, 0)), c = hash21(i + float2(0, 1)), d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
// soft cloud field: octaves are rotated against each other so no grid direction shows through
float cloud(float2 p)
{
    float v = 0.0, a = 0.5;
    [unroll] for (int k = 0; k < 4; k++) {
        v += a * snoise(p);
        p = float2(p.x * 0.80 - p.y * 0.60, p.x * 0.60 + p.y * 0.80) * 1.93 + float2(31.7, 11.9);
        a *= 0.5;
    }
    return v / 0.9375;
}
float vnoise(float2 p)
{
    float2 i = floor(p); float2 f = frac(p); f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + float2(1, 0)), c = hash21(i + float2(0, 1)), d = hash21(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
float fbm(float2 p)
{
    float v = 0.0, a = 0.5;
    [unroll] for (int k = 0; k < 5; k++) { v += a * vnoise(p); p = p * 2.03 + float2(17.1, 9.3); a *= 0.5; }
    return v;
}
float coverOf() { float c = (phase < 0.5) ? bright : ((phase < 1.5) ? 1.0 : 1.0 - bright); c = saturate(c); return c * c * c * (c * (c * 6.0 - 15.0) + 10.0); }
)";

    // Eyes: lids close from top and bottom (corners first), hold shut, open again on the new cell.
    inline constexpr const char* kEyesPS = R"(
float4 main(VSOut i) : SV_Target
{
    float c = coverOf();
    float x = i.uv.x * 2.0 - 1.0;
    float A = 0.5 + 0.5 * (1.0 - x * x);          // half-height of the opening when fully open
    float h = (1.0 - c) * A;
    float d = h - abs(i.uv.y - 0.5);              // > 0 inside the opening
    float lid = 1.0 - smoothstep(0.0, 0.012, d);
    float shade = lerp(0.45, 1.0, smoothstep(0.0, 0.22, d));
    if (phase < 0.5 || phase > 1.5) {
        return float4(0.0, 0.0, 0.0, max(lid, 1.0 - shade));  // closing and opening: the live frame shows through the opening
    }
    float3 col = snap.Sample(samp, i.uv).rgb * shade;
    col = lerp(col, float3(0.0, 0.0, 0.0), lid);
    return float4(col, 1.0);
}
)";

    // Ink: Apocrypha. Black ink swallows the picture from the edges, veins drift while it holds,
    // then it drains away off the new cell.
    inline constexpr const char* kInkPS = R"(
float4 main(VSOut i) : SV_Target
{
    float c = coverOf();
    float aspect = resolution.x / max(resolution.y, 1.0);
    float2 p = i.uv * float2(aspect, 1.0);
    float t = age * 0.55;                                   // unhurried
    float n = cloud(p * 2.6 + float2(t * 0.15, -t * 0.10));
    float n2 = cloud(p * 5.5 - float2(t * 0.22, t * 0.16));
    float field = n * 0.7 + n2 * 0.3;
    float edgeDist = min(min(i.uv.x, 1.0 - i.uv.x), min(i.uv.y, 1.0 - i.uv.y)) * 2.0;
    float edge = 1.0 - smoothstep(0.0, 0.7, edgeDist);
    float m = saturate(field * 0.75 + edge * 0.35);
    float v = c * 1.25 - (1.0 - m);
    float ink = smoothstep(0.0, 0.14, v);
    float rim = smoothstep(0.0, 0.14, v) - smoothstep(0.08, 0.34, v);
    float veins = pow(saturate(cloud(p * 3.4 + float2(t * 0.30, t * 0.19))), 3.0) * 1.6;
    float pulse = 0.5 + 0.5 * sin(t * 1.3);
    float3 inkCol = float3(0.008, 0.016, 0.012) + float3(0.08, 0.26, 0.20) * veins * (0.6 + 0.4 * pulse);
    float3 rimCol = float3(0.20, 0.65, 0.50);
    inkCol += rimCol * rim * 0.9;
    if (phase < 0.5 || phase > 1.5) {
        return float4(inkCol, ink);
    }
    float3 base = snap.Sample(samp, i.uv).rgb;
    float3 col = lerp(base, inkCol, ink);
    return float4(col, 1.0);
}
)";

    // Magic: fast travel. A vortex of motes spirals in toward the centre and the view brightens to a white-blue flash
    // (build-up, over the live scene), a slow nebula turns while the world loads (hold), then the flash fades and the
    // motes burst outward as the new place appears (clearing).
    inline constexpr const char* kMagicPS = R"(
float4 main(VSOut i) : SV_Target
{
    float c = coverOf();
    float aspect = resolution.x / max(resolution.y, 1.0);
    float2 uv = i.uv;
    float2 p = (uv - 0.5) * float2(aspect, 1.0);
    float r = length(p);
    float ang = atan2(p.y, p.x);
    float t = age;
    bool clearing = phase > 1.5;
    bool holding = phase > 0.5 && phase < 1.5;
    // motes on spiral tracks: angle drifts with radius and time; during the build-up they fall inward, during the clearing they fly out
    float sp = 0.0;
    [unroll] for (int k = 0; k < 4; k++) {
        float fk = (float)k;
        float dir = clearing ? -1.0 : 1.0;
        float rr = r + dir * t * (0.22 + 0.08 * fk) + fk * 0.37;            // moving inward over time
        float aa = ang + rr * (2.2 + 0.6 * fk) + fk * 1.7;                   // spiral
        float2 cell = float2(floor(aa * (5.0 + 2.0 * fk) / 3.14159), floor(rr * (9.0 + 3.0 * fk)));
        float2 f = float2(frac(aa * (5.0 + 2.0 * fk) / 3.14159), frac(rr * (9.0 + 3.0 * fk))) - 0.5;
        float2 off = float2(hash21(cell), hash21(cell + 11.0)) - 0.5;
        float dd = length(f - off * 0.7);
        float tw = 0.55 + 0.45 * sin(t * 5.0 + hash21(cell + 3.0) * 6.28);
        float vis = clearing ? (1.0 - c) : c;                                 // motes appear as the magic rises, vanish as it clears
        sp += smoothstep(0.11, 0.0, dd) * tw * smoothstep(0.0, 0.35, vis);
    }
    // the glow closes in from the edges as a soft ring, then floods the centre
    float ring = smoothstep(1.15 - c * 1.5, 0.45 - c * 0.9, r);
    float flood = smoothstep(0.55, 1.0, c);                                   // the last part of the build-up whites out
    float nebula = fbm(p * 2.2 + float2(t * 0.12, -t * 0.09)) * 0.6 + fbm(p * 5.0 - float2(t * 0.2, t * 0.15)) * 0.4;
    float3 cold = float3(0.25, 0.45, 1.00);
    float3 warm = float3(0.70, 0.55, 1.00);
    float3 glow = lerp(cold, warm, nebula) * (0.5 + 0.5 * nebula);
    float3 col = glow * ring + float3(0.92, 0.96, 1.0) * sp;
    if (holding) {
        // full cover: the nebula turns slowly with a gentle pulse, motes drift
        float pulse = 0.85 + 0.15 * sin(t * 1.1);
        col = glow * pulse + float3(0.92, 0.96, 1.0) * sp * 0.6;
        float3 base = snap.Sample(samp, uv).rgb;
        float keep = 0.18 * (1.0 - smoothstep(0.0, 0.9, r));                 // a ghost of the old place lingers at the centre
        return float4(lerp(col, base, keep), 1.0);
    }
    float flash = flood * (clearing ? 1.0 : 1.0);
    col = lerp(col, float3(1.0, 1.0, 1.0), flash);
    float alpha = saturate(max(ring, sp) * (clearing ? 1.0 : 1.0));
    alpha = max(alpha, flood);
    return float4(col, alpha);
}
)";

    // ---- additional styles. Contract as above: while building up (phase 0) and clearing (phase 2) the
    //  output is blended over the live frame by its alpha; while holding (phase 1) it must be opaque.

    // Fade: plain fade to black and back.
    inline constexpr const char* kFadePS = R"(
float4 main(VSOut i) : SV_Target
{
    float c = coverOf();
    if (phase > 0.5 && phase < 1.5) return float4(0.0, 0.0, 0.0, 1.0);
    return float4(0.0, 0.0, 0.0, c);
}
)";

    // Light: the view blooms to warm white from the centre and fades back out of it (portals, Sovngarde, wayshrines).
    inline constexpr const char* kLightPS = R"(
float4 main(VSOut i) : SV_Target
{
    float c = coverOf();
    float aspect = resolution.x / max(resolution.y, 1.0);
    float2 p = (i.uv - 0.5) * float2(aspect, 1.0);
    float r = saturate(length(p) / 0.95);
    float3 light = float3(1.0, 0.975, 0.92);
    if (phase > 0.5 && phase < 1.5) {
        float shimmer = 0.97 + 0.03 * sin(age * 1.7 + r * 4.0);
        return float4(light * shimmer, 1.0);
    }
    float a = saturate(c * (1.7 - 0.7 * r));
    a = a * a * (3.0 - 2.0 * a);
    return float4(light, a);
}
)";

    // Drowsy: heavy, soft-edged lids that flutter once on the way down; the view swims a little (sleep, waking up).
    inline constexpr const char* kDrowsyPS = R"(
float4 main(VSOut i) : SV_Target
{
    float c = coverOf();
    if (phase > 0.5 && phase < 1.5) return float4(0.0, 0.0, 0.0, 1.0);
    float flutter = 0.16 * sin(c * 9.42) * (1.0 - c) * c * 4.0;      // a half-blink on the way
    float k = saturate(c + flutter);
    float x = i.uv.x * 2.0 - 1.0;
    float A = 0.5 + 0.5 * (1.0 - x * x);
    float h = (1.0 - k) * A;
    float d = h - abs(i.uv.y - 0.5);
    float lid = 1.0 - smoothstep(0.0, 0.10, d);                      // wide soft edge
    float haze = 0.55 * k * (1.0 - smoothstep(0.0, 0.45, d));        // darkening that runs ahead of the lids
    return float4(0.0, 0.0, 0.0, saturate(max(lid, haze)));
}
)";

    // Soul: pale violet mist rolls in from the edges and thins out again (Soul Cairn).
    inline constexpr const char* kSoulPS = R"(
float4 main(VSOut i) : SV_Target
{
    float c = coverOf();
    float aspect = resolution.x / max(resolution.y, 1.0);
    float2 p = i.uv * float2(aspect, 1.0);
    float t = age * 0.5;
    // broad, soft blobs of colour only: nothing fine enough to read as a pattern
    float a1 = cloud(p * 1.1 + float2(t * 0.06, -t * 0.04));
    float a2 = cloud(p * 1.7 - float2(t * 0.05, t * 0.07) + 9.0);
    float a3 = cloud(p * 0.7 + float2(-t * 0.03, t * 0.05) + 21.0);
    float3 deep = float3(0.09, 0.07, 0.17);
    float3 violet = float3(0.38, 0.30, 0.62);
    float3 pale = float3(0.70, 0.66, 0.88);
    float3 col = lerp(deep, violet, smoothstep(0.25, 0.75, a1));
    col = lerp(col, pale, smoothstep(0.45, 0.90, a2) * 0.55);
    col *= 0.85 + 0.30 * a3;
    float edgeDist = min(min(i.uv.x, 1.0 - i.uv.x), min(i.uv.y, 1.0 - i.uv.y)) * 2.0;
    float edge = 1.0 - smoothstep(0.0, 0.9, edgeDist);
    float lead = saturate(a1 * 0.6 + edge * 0.45);
    float mist = smoothstep(0.0, 0.40, c * 1.4 - (1.0 - lead));
    if (phase > 0.5 && phase < 1.5) return float4(col, 1.0);
    return float4(col, mist);
}
)";

    // Wipe: darkness travels across the view in one direction and carries on out the other side (lifts).
    //  pulseStrength = +1 when the player moves up (the dark comes down from the top), -1 when moving down.
    inline constexpr const char* kWipePS = R"(
float4 main(VSOut i) : SV_Target
{
    // The picture of the place you left stays up while the next one loads; then it is drawn away in the
    // direction it would fall out of view: downward when the lift goes up, upward when it goes down.
    float up = pulseStrength >= 0.0 ? 1.0 : -1.0;
    if (phase < 1.5) {
        return float4(snap.Sample(samp, i.uv).rgb, 1.0);
    }
    float k = saturate(bright);                               // 0..1 through the wipe
    k = k * k * (3.0 - 2.0 * k);
    float y = up > 0.0 ? i.uv.y : 1.0 - i.uv.y;              // 0 = the side the new place appears from
    float soft = 0.12;
    float edge = k * (1.0 + soft);
    float keep = smoothstep(edge - soft, edge, y);            // 1 = old picture still here
    // the old picture also slides a little the same way, so it reads as moving past rather than being erased
    float2 uv = i.uv - float2(0.0, up * k * 0.10);   // going up: the old picture sinks
    float3 col = snap.Sample(samp, saturate(uv)).rgb;
    col *= 1.0 - 0.35 * (1.0 - keep);                         // darkens slightly as it goes
    return float4(col, keep);
}
)";

    // Dream: a pale lavender haze gathers in slow drifting clouds, fills the view, and thins out again from the
    // centre (visions, dreams, other minds).
    inline constexpr const char* kDreamPS = R"(
float4 main(VSOut i) : SV_Target
{
    float c = coverOf();
    float aspect = resolution.x / max(resolution.y, 1.0);
    float2 p = (i.uv - 0.5) * float2(aspect, 1.0);
    float r = saturate(length(p) / 0.95);
    float t = age * 0.5;
    float clouds = cloud(p * 1.3 + float2(t * 0.07, -t * 0.05));
    float fine = cloud(p * 2.6 - float2(t * 0.09, t * 0.07) + 5.0);
    float3 col = lerp(float3(0.62, 0.56, 0.86), float3(0.93, 0.90, 1.0), saturate(clouds * 0.9 + fine * 0.35));
    col *= 0.94 + 0.06 * sin(t * 0.9 + clouds * 5.0);
    if (phase > 0.5 && phase < 1.5) return float4(col, 1.0);
    float lead = clouds * 0.55 + r * 0.45;
    float a = smoothstep(0.0, 0.45, c * 1.45 - (1.0 - lead));
    return float4(col, a);
}
)";

    // Dissolve: the door look for loads that are not doors. The picture of the place you left stays up while
    // the next one loads, then fades away evenly.
    inline constexpr const char* kDissolvePS = R"(
float4 main(VSOut i) : SV_Target
{
    float3 col = snap.Sample(samp, i.uv).rgb;
    if (phase < 1.5) return float4(col, 1.0);
    return float4(col, coverOf());
}
)";

    using D3DCompileFn = HRESULT(WINAPI*)(const void*, SIZE_T, const char*, const void*, void*, const char*,
        const char*, UINT, UINT, ID3DBlob**, ID3DBlob**);

    inline D3DCompileFn LoadCompiler()
    {
        static D3DCompileFn fn = nullptr;
        static bool tried = false;
        if (!tried) {
            tried = true;
            HMODULE mod = ::LoadLibraryW(L"d3dcompiler_47.dll");
            if (mod) {
                fn = reinterpret_cast<D3DCompileFn>(::GetProcAddress(mod, "D3DCompile"));
            }
        }
        return fn;
    }

    // Returns the bytecode blob (caller releases) or nullptr; a_err receives the compiler message.
    inline ID3DBlob* Compile(const char* a_src, const char* a_entry, const char* a_target, std::string& a_err)
    {
        auto fn = LoadCompiler();
        if (!fn) {
            a_err = "d3dcompiler_47.dll / D3DCompile not available";
            return nullptr;
        }
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = fn(a_src, std::strlen(a_src), "overlay", nullptr, nullptr, a_entry, a_target,
            (1 << 11) /* D3DCOMPILE_OPTIMIZATION_LEVEL3 */, 0, &code, &err);
        if (err) {
            a_err.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
            err->Release();
        }
        if (FAILED(hr) || !code) {
            if (code) {
                code->Release();
            }
            return nullptr;
        }
        return code;
    }
}
