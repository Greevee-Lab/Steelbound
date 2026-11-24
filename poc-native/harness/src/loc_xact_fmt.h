#pragma once
/* loc_xact_fmt.h - W298 LOC-AUDIO: XACT-1-Dateiformate von Line of Contact
 * (Wave-Bank XWB v3, Sound-Bank XSB v11), reine Parser ohne Windows-/Spiel-
 * Abhaengigkeit (Selbsttest: tests/loc_audio_selftest.c, synthetische Daten).
 *
 * XWB v3 (alle LoC-Baenke, belegt an der World-Disc):
 *   0x00 "WBND", 0x04 Version 3, 0x08 4 Segmente {Offset, Groesse}:
 *   Bankdaten (0x28), Eintraege, (leer), Wellendaten.
 *   Bankdaten: Flags (1 = Streaming), Anzahl, Name[16], Eintragsgroesse (0x18),
 *   Namensgroesse, Ausrichtung, kompakt (0).
 *   Eintrag (0x18): Flags (Bit 1 = Schleife), Miniformat, Offset, Laenge (Bytes,
 *   relativ zu den Wellendaten), Schleifenbeginn, Schleifenlaenge (Bytes).
 *   Miniformat: Bit 0-1 Codec (0 PCM, 1 Xbox-ADPCM, 2 WMA), Bit 2-4 Kanaele,
 *   Bit 5-30 Rate, Bit 31 16 Bit (nur PCM).
 * XSB v11 (Bank.xsb): 0x08 Offset Wave-Bank-Namen (je 16 B), 0x1C Zahl der
 *   Cue-Eintraege, 0x1E Zahl der Cues, 0x22 Zahl der Wave-Banks, 0x28 Name;
 *   ab 0x38 Cue-Index (0x14 B: Flags, Eintrag s16 (<0 = Zufallstabelle bei
 *   +8), Namensoffset), danach Cue-Eintraege (0x14 B: +0 Welle/Bank bzw.
 *   Offset, +4 Daempfung (Byte, 1/10 dB, Annahme), +6 Tonhoehe s16
 *   (1/4096 Oktave, Annahme), +0xA Kategorie, +0xB Flags (0x8 einfach, 0x10
 *   Tabelle, sonst Sound-Liste)). Struktur wie vgmstream (XSB_XACT1_2_MAX). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LXF_CODEC_PCM 0
#define LXF_CODEC_ADPCM 1
#define LXF_CODEC_WMA 2

typedef struct {
    char name[17];
    uint32_t flags; /* 1 = Streaming-Bank */
    uint32_t count;
    uint32_t entry_off, entry_size; /* Eintragstabelle (Dateioffset, Elementgroesse) */
    uint32_t data_off, data_len;    /* Wellendaten (Dateioffset) */
    uint32_t align;
} lxf_bank_t;

typedef struct {
    int codec, ch, rate, bits;
    uint32_t off, len;          /* absoluter Dateioffset + Laenge der Daten */
    int loop;                   /* Eintrags-Flag Schleife */
    uint32_t loop_off, loop_len; /* Bytes relativ zum Wellenbeginn (0/0 = ganz) */
} lxf_wave_t;

/* d/n = Dateianfang (mindestens bis Ende der Eintragstabelle). 0 = ok. */
int lxf_xwb_parse(const uint8_t *d, uint32_t n, lxf_bank_t *b);
/* Bytes, die fuer lxf_xwb_parse vorliegen muessen (aus den ersten 0x50 B). */
uint32_t lxf_xwb_header_size(const uint8_t *d, uint32_t n);
/* Eintrag i (d/n wie oben). 0 = ok. */
int lxf_xwb_wave(const lxf_bank_t *b, const uint8_t *d, uint32_t n, uint32_t i, lxf_wave_t *w);
/* WAVEFORMATEX (18 B, Xbox: ADPCM = Tag 0x69, Block 36*ch, 64 Samples) */
void lxf_wave_wfx(const lxf_wave_t *w, uint8_t wfx[20]);

typedef struct {
    const uint8_t *d;
    uint32_t n;
    int n_cues, n_ent, n_wb;
    uint32_t wb_off, idx_off, ent_off;
    uint32_t t3d_off; /* W456: Kopf +0x10 = 3D-Parametertabelle (0x28 B je Satz) */
} lxf_xsb_t;

#define LXF_MAX_TRACKS 8
typedef struct {
    int ntrack;
    struct {
        int wave, bank; /* Wave-Index, Index in die XSB-Bankliste */
        int loopc;      /* W413: Schleifenzahl aus der XSB (lxf_track_loop_mode): -1 = keine Angabe (Wellen-Flag),
                         * 0 = einmal (direkter Eintrag, Sound-Spur 0), 0xFFFF = endlos, sonst n Wiederholungen */
    } track[LXF_MAX_TRACKS];
    int db100;    /* Lautstaerke (<= 0) */
    int pitch;    /* 1/4096 Oktave */
    int category; /* XSB-Kategorie (Eintrag +0xA) */
    int entry;    /* gewaehlter Cue-Eintrag */
    /* W356 (belegt an der XACT-Bibliothek des Spiels, Stimmen-Pegel 0x1C4208): Eintrag +4 als 16 Bit:
     * Bit 0-8 Lautstaerke (SetVolume -16 mB je Stufe), Bit 9-15 LFE-Mixbin (-50 mB je Stufe);
     * +0xF Pegel des I3DL2-Mixbins (-256 mB je Stufe, nur Stimmen mit diesem Bin = 3D). */
    int db100_xact; /* -16 x Bit 0-8, >= -10000 */
    int lfe_mb;     /* -50 x Bit 9-15, >= -10000 */
    int room_mb;    /* -256 x (+0xF), >= -10000 */
    /* W456 BOOTSND (#93): Huellkurven-Ereignis der Sound-Liste (Typ 0x0A, 12 B ab +8), belegt am Ereignis-Verteiler
     * der Bibliothek (0x1C26E2, Fall 10/11 -> SetEG 0x1C861D): +8 Delay, +10 Attack, +12 Hold, +14 Decay, +16 Release
     * (u16, 512 Samples bei 48 kHz je Einheit), +18 Sustain (0..255), +19 Modus (Startphase, 1 = Delay).
     * Typ 0x0A = Amplitude (dwEG 1); 0x0B (Multi, + Tonhoehen-/Filterskala) kommt in Bank.xsb nicht vor. */
    int eg_on;
    int eg[7]; /* Modus, Delay, Attack, Hold, Decay, Release, Sustain */
    /* W456: 3D-Parameter des Eintrags, belegt an der Bibliothek (0x1C6702: Eintrag +0xB Bit 0 -> 0x1C4E15): Satz =
     * Tabelle (Kopf +0x10) + (Eintrag +0xC) x 0x28: +0/+2 Kegelwinkel, +4 Kegel-Aussenpegel, +8 MinDistance (float),
     * +0xC MaxDistance, +0x10 DistanceFactor, +0x14 Rolloff, +0x18 Doppler, +0x1C Modus, +0x1D/+0x1E Rolloff-Kurve
     * (Setter 0x1C927A/92C3/934C/9306/938F/941B/93D5/90FE/9461 -> DS3DBUFFER ab Stimme +0x48). Bank.xsb: 1042
     * Eintraege Satz 0 (Min 100, Max 100000, Rolloff 1, Modus normal), 38 Satz 118 (Min 1). */
    int has3d;          /* Eintrag +0xB Bit 0 und gueltiger Satz */
    float min3d, max3d, rolloff3d;
    int mode3d;
} lxf_cue_t;

/* W413 LOC-SOUND-LOOP (GitHub #65): Schleife einer Spur aus Wellen-Flag und XSB-Schleifenzahl. Belegt an Bank.xsb:
 * Sound-Spuren tragen bei +6 eine 16-Bit-Zahl (230 x 0xFFFF, alle mit Schleifen-Welle; 26 x 0, davon 12 mit
 * Schleifen-Welle; 2 x 5); direkte Eintraege (Flag 0x08) haben keine - die 90 mit Schleifen-Welle sind die "_2"-Varianten
 * endloser Sounds (z. B. VT01_003_2 = Ausklang des MG-Feuers, vom Spiel ohne Handle gestartet). */
enum { LXF_LOOP_ONCE = 0, LXF_LOOP_INF = 1, LXF_LOOP_N = 2 };
int lxf_track_loop_mode(int wave_loop, int loopc);

/* W363: Laufzeit-Parameter (RPC) je Spur: Summe der Kurven eines Parameters fuer einen Cue-Eintrag. */
typedef struct {
    int vol_mb;  /* Ziel 0: Lautstaerke in mB (zur Eintrags-Lautstaerke, Summe <= 0 geklemmt) */
    int pitch;   /* Ziel 1: Tonhoehe in 1/4096 Oktave (zur Eintrags-Tonhoehe, Summe -32767..8191) */
    int filt_hz; /* Ziel 2: Tiefpass-Eckfrequenz in Hz (0..8192, DLS2) */
    int has;     /* Bit 0/1/2 = Ziel 0/1/2 belegt */
} lxf_rpc_t;
int lxf_xsb_parse(const uint8_t *d, uint32_t n, lxf_xsb_t *x); /* 0 = ok */
const char *lxf_xsb_cue_name(const lxf_xsb_t *x, int cue);      /* NULL = keiner */
int lxf_xsb_find(const lxf_xsb_t *x, const char *name);         /* -1 = fehlt */
/* Name der Bank i (in buf, 17 B). */
void lxf_xsb_bank_name(const lxf_xsb_t *x, int i, char buf[17]);
/* Cue aufloesen; rnd waehlt Varianten (Zufallstabellen). 0 = ok. */
int lxf_xsb_cue(const lxf_xsb_t *x, int cue, uint32_t rnd, lxf_cue_t *c);
/* W363: Kurve (24 B) an x auswerten wie die XACT-Bibliothek (0x1C5392). */
int lxf_rpc_eval(const uint8_t *cv, int xi);
/* W363: RPC "name" mit Wert value fuer Cue-Eintrag entry; Rueckgabe = Zahl ausgewerteter Kurven. */
int lxf_xsb_rpc(const lxf_xsb_t *x, int entry, const char *name, float value, lxf_rpc_t out[LXF_MAX_TRACKS]);

#ifdef __cplusplus
}
#endif
