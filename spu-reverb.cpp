// PlayStation 1 SPU reverb for Korg minilogue xd
//
// Nachbau des PSX SPU-Reverb-Hardwareblocks (Nocash-Spec):
//   Input-Gain -> Same/Different-Side IIR-Reflexion -> 4-Tap Comb (Early Echo)
//   -> 2 All-Pass-Filter (Late Reverb) -> Output.
// Ein gemeinsamer Ringpuffer, ein Schreibzeiger (pos) rueckt pro Core-Schritt
// um 1 vor, alle Zugriffe relativ zu pos (wie die SPU-BufferAddress).
//
// Der SPU-Reverb rechnet original mit 22050 Hz (ein Formeldurchlauf pro Kanal
// alle 1/22050 s). Dieser Port laeuft den Kern ebenfalls mit 22050 Hz und
// resampled zum 48-kHz-Host: Eingang per Boxcar dezimiert, Wet-Ausgang linear
// interpoliert. Dadurch bleiben Originaloffsets, 11-kHz-Bandbreite und
// 16-Bit-Saettigung erhalten -> authentischer PSX-"Grit".
// Der trockene Pfad bleibt in voller 48-kHz-Bandbreite.
//
// TIME        -> Preset (Room, Studio S/M/L, Hall, Half Echo, Space Echo)
// DEPTH       -> Wet/Dry-Mix
// SHIFT+DEPTH -> Decay/Tail-Laenge (skaliert vWALL; Maximum = authentisch)

#include "userrevfx.h"
#include "float_math.h"

// --- SPU-Registergrid-Indizes (Reihenfolge exakt wie in der Nocash-Spec) ------
enum {
  G_dAPF1, G_dAPF2, G_vIIR,   G_vCOMB1, G_vCOMB2,  G_vCOMB3,  G_vCOMB4,  G_vWALL,
  G_vAPF1, G_vAPF2, G_mLSAME, G_mRSAME, G_mLCOMB1, G_mRCOMB1, G_mLCOMB2, G_mRCOMB2,
  G_dLSAME,G_dRSAME,G_mLDIFF, G_mRDIFF, G_mLCOMB3, G_mRCOMB3, G_mLCOMB4, G_mRCOMB4,
  G_dLDIFF,G_dRDIFF,G_mLAPF1, G_mRAPF1, G_mLAPF2,  G_mRAPF2,  G_vLIN,    G_vRIN
};

struct SpuPreset {
  uint32_t size;      // Reverb-Arbeitsbereich in Bytes (@22050)
  uint16_t g[32];     // Registergrid
};

// Presets aus der Nocash-Spec, aufsteigend nach Hall-Laenge.
static const SpuPreset k_presets[] = {
  // Room (26C0h)
  { 0x26C0u, {
    0x007D,0x005B,0x6D80,0x54B8,0xBED0,0x0000,0x0000,0xBA80,
    0x5800,0x5300,0x04D6,0x0333,0x03F0,0x0227,0x0374,0x01EF,
    0x0334,0x01B5,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
    0x0000,0x0000,0x01B4,0x0136,0x00B8,0x005C,0x8000,0x8000 } },
  // Studio Small (1F40h)
  { 0x1F40u, {
    0x0033,0x0025,0x70F0,0x4FA8,0xBCE0,0x4410,0xC0F0,0x9C00,
    0x5280,0x4EC0,0x03E4,0x031B,0x03A4,0x02AF,0x0372,0x0266,
    0x031C,0x025D,0x025C,0x018E,0x022F,0x0135,0x01D2,0x00B7,
    0x018F,0x00B5,0x00B4,0x0080,0x004C,0x0026,0x8000,0x8000 } },
  // Studio Medium (4840h)
  { 0x4840u, {
    0x00B1,0x007F,0x70F0,0x4FA8,0xBCE0,0x4510,0xBEF0,0xB4C0,
    0x5280,0x4EC0,0x0904,0x076B,0x0824,0x065F,0x07A2,0x0616,
    0x076C,0x05ED,0x05EC,0x042E,0x050F,0x0305,0x0462,0x02B7,
    0x042F,0x0265,0x0264,0x01B2,0x0100,0x0080,0x8000,0x8000 } },
  // Studio Large (6FE0h)
  { 0x6FE0u, {
    0x00E3,0x00A9,0x6F60,0x4FA8,0xBCE0,0x4510,0xBEF0,0xA680,
    0x5680,0x52C0,0x0DFB,0x0B58,0x0D09,0x0A3C,0x0BD9,0x0973,
    0x0B59,0x08DA,0x08D9,0x05E9,0x07EC,0x04B0,0x06EF,0x03D2,
    0x05EA,0x031D,0x031C,0x0238,0x0154,0x00AA,0x8000,0x8000 } },
  // Hall (ADE0h)
  { 0xADE0u, {
    0x01A5,0x0139,0x6000,0x5000,0x4C00,0xB800,0xBC00,0xC000,
    0x6000,0x5C00,0x15BA,0x11BB,0x14C2,0x10BD,0x11BC,0x0DC1,
    0x11C0,0x0DC3,0x0DC0,0x09C1,0x0BC4,0x07C1,0x0A00,0x06CD,
    0x09C2,0x05C1,0x05C0,0x041A,0x0274,0x013A,0x8000,0x8000 } },
  // Half Echo (3C00h)
  { 0x3C00u, {
    0x0017,0x0013,0x70F0,0x4FA8,0xBCE0,0x4510,0xBEF0,0x8500,
    0x5F80,0x54C0,0x0371,0x02AF,0x02E5,0x01DF,0x02B0,0x01D7,
    0x0358,0x026A,0x01D6,0x011E,0x012D,0x00B1,0x011F,0x0059,
    0x01A0,0x00E3,0x0058,0x0040,0x0028,0x0014,0x8000,0x8000 } },
  // Space Echo (F6C0h)
  { 0xF6C0u, {
    0x033D,0x0231,0x7E00,0x5000,0xB400,0xB000,0x4C00,0xB000,
    0x6000,0x5400,0x1ED6,0x1A31,0x1D14,0x183B,0x1BC2,0x16B2,
    0x1A32,0x15EF,0x15EE,0x1055,0x1334,0x0F2D,0x11F6,0x0C5D,
    0x1056,0x0AE1,0x0AE0,0x07A2,0x0464,0x0232,0x8000,0x8000 } },
};
static const uint32_t k_num_presets = sizeof(k_presets) / sizeof(k_presets[0]);

// Core-Rate relativ zum 48-kHz-Host.
static const float k_core_rate = 22050.f / 48000.f;
// 1/0x8000 fuer Volume-Normalisierung.
static const float k_vol_scale = 1.f / 32768.f;

// Groesster Puffer @22050: Space Echo = F6C0h/2 = 31584 Samples. Reserve bis 2^15.
#define MAX_N 32768U
static float s_buf[MAX_N] __sdram;

// Laufzeit-Koeffizienten (aus Preset aufbereitet).
struct RevRT {
  float viir, vcomb1, vcomb2, vcomb3, vcomb4, vwall, vapf1, vapf2, vlin, vrin;
  int32_t dAPF1, dAPF2, dLSAME, dRSAME, dLDIFF, dRDIFF;
  int32_t mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2;
  int32_t mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4;
  int32_t mLAPF1, mRAPF1, mLAPF2, mRAPF2;
  int32_t N;
};
static RevRT s_rt;

static uint32_t s_preset;   // aktueller Preset-Index
static uint32_t s_pos;      // Schreibzeiger (Core-Rate)
static float    s_mix;      // DEPTH: Wet-Anteil
static float    s_decay;    // SHIFT+DEPTH: vWALL-Skalierung 0..1

// Resampler-Zustand 48k <-> 22050.
static float    s_phase;    // Bruchteil bis zum naechsten Core-Sample [0,1)
static float    s_inAccL, s_inAccR; // Eingangs-Akkumulator (Boxcar-Dezimation)
static uint32_t s_inCount;
static float    s_outL0, s_outR0;   // vorheriges Core-Wet-Sample
static float    s_outL1, s_outR1;   // aktuelles Core-Wet-Sample

// Ringpuffer-Zugriff relativ zu pos, gewrappt in [0, N). off in (-N, N).
static inline float &rbuf(uint32_t pos, int32_t off, int32_t N) {
  int32_t i = (int32_t)pos + off;
  if (i >= N) i -= N; else if (i < 0) i += N;
  return s_buf[i];
}

// Registeroffset (/8 Bytes) -> Sample @22050: reg*8 Byte / 2 Byte/Sample = reg*4.
static inline int32_t off_of(const SpuPreset &p, int idx) {
  return (int32_t)p.g[idx] * 4;
}
static inline float vol_of(const SpuPreset &p, int idx) {
  return (float)(int16_t)p.g[idx] * k_vol_scale;
}

static void rebuild_rt(void) {
  const SpuPreset &p = k_presets[s_preset];
  s_rt.viir   = vol_of(p, G_vIIR);
  s_rt.vcomb1 = vol_of(p, G_vCOMB1);
  s_rt.vcomb2 = vol_of(p, G_vCOMB2);
  s_rt.vcomb3 = vol_of(p, G_vCOMB3);
  s_rt.vcomb4 = vol_of(p, G_vCOMB4);
  s_rt.vwall  = vol_of(p, G_vWALL) * (0.4f + 0.6f * s_decay); // Tail-Steuerung
  s_rt.vapf1  = vol_of(p, G_vAPF1);
  s_rt.vapf2  = vol_of(p, G_vAPF2);
  s_rt.vlin   = vol_of(p, G_vLIN);
  s_rt.vrin   = vol_of(p, G_vRIN);
  s_rt.dAPF1  = off_of(p, G_dAPF1);
  s_rt.dAPF2  = off_of(p, G_dAPF2);
  s_rt.dLSAME = off_of(p, G_dLSAME);
  s_rt.dRSAME = off_of(p, G_dRSAME);
  s_rt.dLDIFF = off_of(p, G_dLDIFF);
  s_rt.dRDIFF = off_of(p, G_dRDIFF);
  s_rt.mLSAME = off_of(p, G_mLSAME);
  s_rt.mRSAME = off_of(p, G_mRSAME);
  s_rt.mLCOMB1= off_of(p, G_mLCOMB1);
  s_rt.mRCOMB1= off_of(p, G_mRCOMB1);
  s_rt.mLCOMB2= off_of(p, G_mLCOMB2);
  s_rt.mRCOMB2= off_of(p, G_mRCOMB2);
  s_rt.mLDIFF = off_of(p, G_mLDIFF);
  s_rt.mRDIFF = off_of(p, G_mRDIFF);
  s_rt.mLCOMB3= off_of(p, G_mLCOMB3);
  s_rt.mRCOMB3= off_of(p, G_mRCOMB3);
  s_rt.mLCOMB4= off_of(p, G_mLCOMB4);
  s_rt.mRCOMB4= off_of(p, G_mRCOMB4);
  s_rt.mLAPF1 = off_of(p, G_mLAPF1);
  s_rt.mRAPF1 = off_of(p, G_mRAPF1);
  s_rt.mLAPF2 = off_of(p, G_mLAPF2);
  s_rt.mRAPF2 = off_of(p, G_mRAPF2);
  int32_t n = (int32_t)(p.size >> 1);           // Samples @22050
  if (n > (int32_t)MAX_N) n = (int32_t)MAX_N;
  s_rt.N = n;
  if (s_pos >= (uint32_t)s_rt.N) s_pos = 0;
}

// Ein SPU-Reverb-Schritt @22050. Eingang (inL,inR), Wet-Ausgang (outL,outR).
static inline void core_step(float inL, float inR, float *outL, float *outR) {
  const RevRT &rt = s_rt;
  const int32_t N = rt.N;
  uint32_t pos = s_pos;

  #define RD(off) rbuf(pos, (off), N)

  const float Lin = rt.vlin * inL;
  const float Rin = rt.vrin * inR;

  // --- Same Side Reflection (L->L, R->R) ---
  const float sameL1 = RD(rt.mLSAME - 1);
  const float sameR1 = RD(rt.mRSAME - 1);
  RD(rt.mLSAME) = clip1m1f((Lin + RD(rt.dLSAME) * rt.vwall - sameL1) * rt.viir + sameL1);
  RD(rt.mRSAME) = clip1m1f((Rin + RD(rt.dRSAME) * rt.vwall - sameR1) * rt.viir + sameR1);

  // --- Different Side Reflection (R->L, L->R) ---
  const float diffL1 = RD(rt.mLDIFF - 1);
  const float diffR1 = RD(rt.mRDIFF - 1);
  RD(rt.mLDIFF) = clip1m1f((Lin + RD(rt.dRDIFF) * rt.vwall - diffL1) * rt.viir + diffL1);
  RD(rt.mRDIFF) = clip1m1f((Rin + RD(rt.dLDIFF) * rt.vwall - diffR1) * rt.viir + diffR1);

  // --- Early Echo (Comb, Input aus Puffer) ---
  float Lout = rt.vcomb1 * RD(rt.mLCOMB1) + rt.vcomb2 * RD(rt.mLCOMB2)
             + rt.vcomb3 * RD(rt.mLCOMB3) + rt.vcomb4 * RD(rt.mLCOMB4);
  float Rout = rt.vcomb1 * RD(rt.mRCOMB1) + rt.vcomb2 * RD(rt.mRCOMB2)
             + rt.vcomb3 * RD(rt.mRCOMB3) + rt.vcomb4 * RD(rt.mRCOMB4);

  // --- Late Reverb APF1 ---
  {
    const float rL = RD(rt.mLAPF1 - rt.dAPF1);
    const float rR = RD(rt.mRAPF1 - rt.dAPF1);
    Lout = Lout - rt.vapf1 * rL;  RD(rt.mLAPF1) = clip1m1f(Lout);  Lout = Lout * rt.vapf1 + rL;
    Rout = Rout - rt.vapf1 * rR;  RD(rt.mRAPF1) = clip1m1f(Rout);  Rout = Rout * rt.vapf1 + rR;
  }
  // --- Late Reverb APF2 ---
  {
    const float rL = RD(rt.mLAPF2 - rt.dAPF2);
    const float rR = RD(rt.mRAPF2 - rt.dAPF2);
    Lout = Lout - rt.vapf2 * rL;  RD(rt.mLAPF2) = clip1m1f(Lout);  Lout = Lout * rt.vapf2 + rL;
    Rout = Rout - rt.vapf2 * rR;  RD(rt.mRAPF2) = clip1m1f(Rout);  Rout = Rout * rt.vapf2 + rR;
  }

  if (++pos >= (uint32_t)N) pos = 0;
  s_pos = pos;

  // 16-Bit-Ausgangssaettigung
  *outL = clip1m1f(Lout);
  *outR = clip1m1f(Rout);

  #undef RD
}

static inline void reset_resampler(void) {
  s_phase = 0.f;
  s_inAccL = s_inAccR = 0.f;
  s_inCount = 0;
  s_outL0 = s_outR0 = s_outL1 = s_outR1 = 0.f;
}

void REVFX_INIT(uint32_t platform, uint32_t api) {
  (void)platform; (void)api;
  s_preset = 0;
  s_pos    = 0;
  s_mix    = 0.5f;
  s_decay  = 1.f;
  for (uint32_t i = 0; i < MAX_N; i++) s_buf[i] = 0.f;
  reset_resampler();
  rebuild_rt();
}

void REVFX_PROCESS(float *xn, uint32_t frames) {
  float * __restrict x = xn;
  const float mix = s_mix;
  const float r   = k_core_rate;

  float    phase = s_phase;
  float    accL = s_inAccL, accR = s_inAccR;
  uint32_t cnt  = s_inCount;
  float    o0L = s_outL0, o0R = s_outR0, o1L = s_outL1, o1R = s_outR1;

  for (uint32_t i = 0; i < frames; i++, x += 2) {
    const float inL = x[0];
    const float inR = x[1];

    // Eingang fuer Dezimation sammeln
    accL += inL; accR += inR; cnt++;

    phase += r;
    if (phase >= 1.f) {              // Zeit fuer ein neues 22050-Core-Sample
      phase -= 1.f;
      const float inv = 1.f / (float)cnt;
      const float cinL = accL * inv;  // Boxcar-dezimierter Eingang
      const float cinR = accR * inv;
      accL = accR = 0.f; cnt = 0;
      o0L = o1L; o0R = o1R;
      core_step(cinL, cinR, &o1L, &o1R);
    }

    // Wet linear zwischen den beiden letzten Core-Samples interpolieren
    const float wetL = o0L + phase * (o1L - o0L);
    const float wetR = o0R + phase * (o1R - o0R);

    // Trockener Pfad bleibt voll, nur Wet gemischt
    x[0] = clip1m1f(inL + mix * (wetL - inL));
    x[1] = clip1m1f(inR + mix * (wetR - inR));
  }

  s_phase = phase;
  s_inAccL = accL; s_inAccR = accR; s_inCount = cnt;
  s_outL0 = o0L; s_outR0 = o0R; s_outL1 = o1L; s_outR1 = o1R;
}

void REVFX_PARAM(uint8_t index, int32_t value) {
  const float f = q31_to_f32(value);
  switch (index) {
    case k_user_revfx_param_time: {
      uint32_t idx = (uint32_t)(f * (float)k_num_presets);
      if (idx >= k_num_presets) idx = k_num_presets - 1;
      if (idx != s_preset) { s_preset = idx; rebuild_rt(); }
      break;
    }
    case k_user_revfx_param_depth:
      s_mix = f;
      break;
    case k_user_revfx_param_shift_depth:
      s_decay = f;
      rebuild_rt();
      break;
    default:
      break;
  }
}

void REVFX_SUSPEND(void) {}

void REVFX_RESUME(void) {
  // Alten Hall-Schwanz + Resampler leeren, sonst Knackser beim Wiedereinschalten.
  for (uint32_t i = 0; i < MAX_N; i++) s_buf[i] = 0.f;
  s_pos = 0;
  reset_resampler();
}
