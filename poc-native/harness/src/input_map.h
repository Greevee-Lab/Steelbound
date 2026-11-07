#pragma once
/* input_map.h - Eingabe-Profile: Quelle -> Controller-Element (W76 CTRL-MAP).
 *
 * Quellen: Tastatur (VK, ueber key_down -> Replay-faehig), Maus (relativ +
 * Tasten), XInput-Gamepad (Tasten, Sticks, Trigger, D-Pad). Elemente: alle
 * aus CONTROLLER.md §8 (bi0-38, Aiming/Rotation/Sight, 3 Pedale, Tuner,
 * Gang). Doku: poc-native/docs/EINGABE.md.
 *
 * Harness-unabhaengig (nur C + Win32): kein hlog/hcfg/key_down-Import, alles
 * wird als Snapshot injiziert. Dadurch ist derselbe Code im
 * Vergleichs-Selbsttest (input_selftest.c) prüfbar.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 24-Byte-SBC-Block (Layout wie sbc_in_t in hooks_int.h). */
#pragma pack(push, 1)
typedef struct {
    uint16_t buttons[3];
    uint8_t pad1, aiming_x;
    uint8_t pad2, aiming_y;
    uint8_t pad3;
    int8_t rot_lever;
    uint8_t pad4;
    int8_t sight_x;
    uint8_t pad5;
    int8_t sight_y;
    uint8_t pad6, pedal_l;
    uint8_t pad7, pedal_m;
    uint8_t pad8, pedal_r;
    uint8_t tuner, gear;
} imap_block_t;
#pragma pack(pop)

/* Tastaturabfrage (Hook: key_down aus hooks_bots.c -> Replay-Slots +
 * echte Tastatur; Test: Stub). */
typedef int (*imap_key_fn)(int vk, void *ctx);

/* Gamepad-Zustand (XInput-Abbild, ohne SDK-Abhaengigkeit im Header). */
typedef struct {
    uint16_t buttons; /* Bits IMAP_PAD_* */
    uint8_t lt, rt;   /* Trigger 0..255 */
    int16_t lx, ly, rx, ry; /* Sticks -32768..32767 */
} imap_pad_state_t;

/* Tasten-Bits (Werte wie XINPUT_GAMEPAD_*). */
enum {
    IMAP_PAD_DU = 0x0001, IMAP_PAD_DD = 0x0002,
    IMAP_PAD_DL = 0x0004, IMAP_PAD_DR = 0x0008,
    IMAP_PAD_START = 0x0010, IMAP_PAD_BACK = 0x0020,
    IMAP_PAD_L3 = 0x0040, IMAP_PAD_R3 = 0x0080,
    IMAP_PAD_LB = 0x0100, IMAP_PAD_RB = 0x0200,
    IMAP_PAD_A = 0x1000, IMAP_PAD_B = 0x2000,
    IMAP_PAD_X = 0x4000, IMAP_PAD_Y = 0x8000
};

/* Eingabe-Snapshot fuer einen Poll (Hook fuellt aus key_down/Maus/Pad). */
typedef struct {
    imap_key_fn key;
    void *ctx;
    int replay_active; /* 1 = Replay/Slots aktiv (echte Maus tot) */
    unsigned frame;    /* Present-Frame (Maus-Einmal-Guard) */
    /* cfg-Gatter (Mausschalter + Taster/Toggle, aus g_cfg): */
    int mouse_aim, mouse_sight, mouse_sens;
    int mouse_invert_x, mouse_invert_y, mouse_buttons;
    int sbc_momentary;
    /* W132 PAD-ARB (cfg-Gatter aus g_cfg): pad_axis_arb = 1 (Default):
     * Pad gegen Maus "zuletzt aktives Geraet gewinnt" je Achse (nur wenn
     * die Achse Pad-Quellen hat, ein Pad da ist und die Maus liefert),
     * 0 = alt (betragsgroesste Auslenkung gewinnt). 0 = bitgenau alt. */
    int pad_axis_arb;
    int pad_arb_dead;   /* Pad-Bewegung > N Spiel-Einheiten holt die Achse */
    int pad_mouse_wake; /* Mausbewegung >= N Hebelstufen holt sie zurueck */
    /* W143: globale Totzonen (0..0.9, aus pad_dz_stick/_trig %); wirken als
     * Untergrenze ueber der Profil-Totzone. 0 = bitgenau alt. */
    float pad_dz_stick, pad_dz_trig;
    /* Gamepad (pollt der Hook pro Frame, s. imap_pad_poll): */
    imap_pad_state_t pad;
    int pad_present; /* 0 = kein Geraet -> Pad-Quellen tragen nichts bei */
    /* W159: Original-Controller als Belegungs-Quelle (SBC:<taste>).
     * Layout wie sbc_state_t: sbc[0] = bi0..15, [1] = bi16..31,
     * [2] = bi32..38. sbc_present = 0 -> SBC-Quellen tragen nichts bei. */
    uint16_t sbc[3];
    int sbc_present;
    /* W79 INPUT-REAL (Spielerbetrieb; Aufrufer ohne diese Felder lassen sie
     * auf 0 = bisheriges Verhalten bitgenau): */
    int mouse_raw;      /* 1 = Maus-Deltas aus raw_dx/raw_dy (Raw Input,
                         * mouse_capture=1) statt GetCursorPos-Differenzen */
    int raw_dx, raw_dy; /* Raw-Input-Zaehler seit dem letzten Poll */
    int real_blocked;   /* 1 = echte Maus (Tasten + Deltas) gesperrt
                         * (Fenster ohne Fokus / Maus freigegeben) */
    int (*real_vk)(int vk); /* echte Maustaste lesen (input_source=1:
                             * Fenster-Nachrichten); NULL = GetAsyncKeyState */
    int mouse_abs;      /* W87 mouse_mode=absolute: Maus-Aiming-Akku behaelt
                         * Bruchteile (m_fx/m_fy, kein Abschneiden kleiner
                         * Deltas); die Vorsteuerung auf das Fadenkreuz macht
                         * der Hook (hooks_input.c). 0 = bitgenau alt */
    /* Akkord-Unterdrueckung (fuellt imap_profile_apply pro Poll: Atome
     * aktiver Akkorde; deren reine Basis-Bindungen schweigen, z. B.
     * feuert LB+RB = Magazin weder Hauptwaffe noch Lock-On). Aufrufer
     * lassen das Feld auf 0. */
    int nsupp;
    struct {
        int kind, code;
    } supp[16];
} imap_snap_t;

/* W132 PAD-ARB: einachsiger Arbitrierungs-Zustand (gleiche Logik wie
 * imap_arb_t/W128, ein Geraet gegen die Maus). memset(0) = uninitialisiert. */
typedef struct {
    uint8_t owns;  /* 1 = Geraet (Pad) besitzt die Achse, 0 = Maus */
    uint8_t init;
    int ref;       /* Geraete-Stand bei der letzten Maus-Uebernahme */
    int last;      /* Geraete-Stand bei der letzten Geraete-Bewegung */
    float m_last;  /* Mausstellung im vorigen Poll (Hebelstufen) */
    float m_move;  /* Mausbewegung seit der Geraete-Uebernahme */
} imap_arb1_t;

/* Zustand eines Pfades (Gang/Tuner/Toggles/Flanken/Maus-Akku). */
typedef struct {
    uint8_t gear, tuner;
    uint16_t toggle; /* sbc_momentary=0: bi4/bi5-Rastzustand */
    int prev_ign, prev_hatch, prev_qup, prev_qdn, prev_tup, prev_tdn;
    int m_ax, m_ay;     /* Maus-Aiming-Akku (uint8, Mitte 0x7F) */
    int m_sx, m_sy;     /* Maus-Sight-Akku (int8, Mitte 0) */
    unsigned m_step_frame;
    int m_have_last, m_last_x, m_last_y;
    uint8_t tog_latch, tog_prev; /* W79: Kippschalter-Rastzustand (bi34-38) */
    float m_fx, m_fy;   /* W87: Bruchteil des Aiming-Akkus (|f| <= 0,5;
                         * virtueller Hebel = m_ax + m_fx), nur mouse_abs */
    imap_arb1_t pad_arb[4]; /* W132: Pad gegen Maus (aim_x/y, sight_x/y) */
    uint8_t m_aim_src;  /* W87: Bit0/Bit1 = aiming_x/y kam in diesem Poll
                         * von der Maus (Hook darf vorsteuern) */
    int prev_vr_recenter; /* W159: Pegel der Aktion vr_recenter im Vor-Poll */
} imap_state_t;

void imap_state_init(imap_state_t *st); /* gear=8(N), tuner=0, Akkus Mitte */
/* W79 INPUT-REAL (key_toggles=1, nur ohne Replay): die 5 Kippschalter
 * bi34..bi38 (FILT CONTROL, OXYGEN SUPPLY, FUEL FLOW, BUFFER MATERIAL,
 * VT-LOCATION) als Rastschalter wie am Original-Controller: Flanke auf
 * (Taste '1'..'5' ODER bereits abgebildetes Bit, z. B. Profil/Pad) klappt
 * den Schalter um, das Bit bleibt bis zur naechsten Flanke stehen.
 * Rueckgabe: Bitmaske der umgeklappten Schalter (Bit i = bi34+i). */
unsigned imap_toggles_apply(imap_block_t *blk, imap_state_t *st,
                            const imap_snap_t *snap);

/* ---- Legacy-Abbildung (1:1 aus hooks_input.c extrahiert) ----
 * Exakt die bisherige feste Belegung (BUTTONMAP.md): Tasten, 256+bi-
 * Virtuals, I/H-Taster/Toggle, Pfeile (-> Sight immer, -> Aiming nur ohne
 * SHIFT), Maus (Akku + L/R/M), A/D-Rotation, W/S-Pedale, LCtrl/X1/X2-Slide (W110), Q/E-Gang,
 * T/G-Tuner. Wird vom Hook aufgerufen, solange input_profile leer ist,
 * und ist die Referenzseite des Vergleichstests. */
void imap_legacy_apply(imap_block_t *blk, imap_state_t *st,
                       const imap_snap_t *snap);

/* ---- Profil-System ---- */
typedef enum {
    IMAP_A_KEY,     /* code = VK (0..512, inkl. Replay-Virtuals) */
    IMAP_A_MOUSEBTN,/* code 0/1/2 = links/rechts/mitte, 3/4 = X1/X2 (W110) */
    IMAP_A_PADBTN,  /* code = IMAP_PAD_*-Bit (ohne D-Pad) */
    IMAP_A_PADDPAD, /* code = IMAP_PAD_DU/DD/DL/DR-Bit */
    IMAP_A_PADTRIG, /* code 0/1 = LT/RT als Taste (Schwelle 50 %) */
    IMAP_A_SBC      /* W159: code = Original-Controller-Taste bi0..38 */
} imap_atom_kind_t;

typedef struct {
    imap_atom_kind_t kind;
    int code;
} imap_atom_t;

/* Eine Belegungs-Alternative: Atom (+ Optional-Akkord) mit SHIFT-Bedingung.
 * cond: 0 = egal, 1 = nur mit SHIFT, 2 = nur ohne SHIFT. */
typedef struct {
    imap_atom_t a, b;
    int chord; /* 1 = a UND b muessen aktiv sein (z. B. BACK + Taste) */
    int cond;
} imap_chord_t;

#define IMAP_MAX_ALT 4
typedef struct {
    int n;
    imap_chord_t alt[IMAP_MAX_ALT];
} imap_bind_t; /* aktiv = ODER aller Alternativen */

/* Analogquelle (Stick/Trigger) mit Totzone/Kurve/Invertierung. */
typedef struct {
    int src; /* 0=LX 1=LY 2=RX 3=RY 4=LT 5=RT, -1 = unbenutzt */
    float dz;    /* Totzone 0..0.9 (reskaliert) */
    float curve; /* Exponent 0.5..3 (1 = linear) */
    int inv;     /* 1 = invertiert */
} imap_analog_t;

typedef struct {
    imap_bind_t neg, pos; /* digitale Endlagen (neg gewinnt bei beiden) */
    int mouse; /* 0 = keine, 1 = Maus-Akku X, 2 = Maus-Akku Y */
    int mouse_sight; /* -1 = egal, 0 = nur Aim-Modus, 1 = nur Sight-Modus */
    imap_analog_t pad[2];
} imap_axis_map_t;

typedef enum {
    IMAP_AX_AIM_X, IMAP_AX_AIM_Y, IMAP_AX_ROT,
    IMAP_AX_SIGHT_X, IMAP_AX_SIGHT_Y,
    IMAP_AX_PEDAL_L, IMAP_AX_PEDAL_M, IMAP_AX_PEDAL_R,
    IMAP_AX_COUNT
} imap_axis_t;

typedef struct {
    char name[64];
    imap_bind_t btn[39];
    imap_axis_map_t axis[IMAP_AX_COUNT];
    imap_bind_t gear_up, gear_down, tuner_up, tuner_down;
    imap_bind_t gear_direct[7]; /* 0=R 1=N 2..6=1..5 */
    /* W159: Client-Aktion statt Controller-Element: "vr_recenter"
     * (VR: Blick zentrieren) feuert auf der steigenden Flanke. */
    imap_bind_t vr_recenter;
    int has_pad; /* 1 = Profil referenziert Pad-Quellen */
    int has_sbc; /* 1 = Profil referenziert SBC-Quellen */
} imap_profile_t;

/* Eingebaute Profile (Fallback, falls Datei fehlt): "keyboard_default",
 * "xbox_pad_default". 0 = ok, -1 = unbekannt. */
int imap_profile_builtin(const char *name, imap_profile_t *out);

/* Profil laden: Datei profiles/<name>.ini (CWD, dann Exe-Ordner) oder
 * direkter Pfad (mit / \ oder .ini-Endung). 0 = Datei, 1 = eingebauter
 * Fallback, -1 = weder Datei noch eingebaut bekannt. */
int imap_profile_load(const char *name_or_path, imap_profile_t *out);

/* Profiltext parsen (Dateiinhalt, Format s. EINGABE.md). */
int imap_profile_parse(const char *text, const char *name, imap_profile_t *out);

/* Strukturvergleich (Selbsttest: Datei == eingebaut). 0 = gleich. */
int imap_profile_equal(const imap_profile_t *a, const imap_profile_t *b);

/* Profil-Abbildung: Snapshot -> SBC-Block (Zusammenfuehrung: Tasten ODER,
 * Achsen = betragsmaessig groesste Auslenkung, Gleichstand: digital vor
 * Maus vor Pad). */
void imap_profile_apply(imap_block_t *blk, imap_state_t *st,
                        const imap_profile_t *prof, const imap_snap_t *snap);
/* W159: Client-Aktion "vr_recenter" (VR: Blick zentrieren). 1 = steigende
 * Flanke in diesem Poll (Hook ruft sb_vr_recenter("Belegung")), sonst 0.
 * Unbelegt -> 0. Ohne VR wirkt der Aufruf nicht (vr.c prueft vr_mode). */
int imap_action_vr_recenter(const imap_profile_t *prof, imap_state_t *st,
                            const imap_snap_t *snap);

/* ---- W128: Achsen-Arbitrierung "zuletzt aktives Geraet gewinnt" ----
 * Original-Controller (SBC) gegen Maus, je Achse (aim_x, aim_y, sight_x,
 * sight_y). Die Maus liefert auf ihren Achsen jeden Poll eine Stellung
 * (Akku; absolut zusaetzlich Vorsteuerung) - sie ist nie "neutral".
 * Regeln (nur Achsen, die in diesem Poll von der Maus kamen):
 *  - SBC bewegt sich um mehr als dead (Spiel-Einheiten) gegenueber dem
 *    Stand bei der letzten Maus-Uebernahme -> SBC besitzt die Achse.
 *  - Maus bewegt sich (Akku-Summe) um >= wake Hebelstufen, waehrend der
 *    SBC ruht -> Maus besitzt die Achse wieder (SBC-Bewegung > dead setzt
 *    die Summe zurueck = Hysterese).
 * Achsen ohne Maus-Quelle (Tastatur/Pfeile, Bots): alte Regel W75 - SBC nur,
 * wenn der Block im Leerlauf steht. Rein, ohne Geraet testbar. */
#define IMAP_ARB_N 4
typedef struct {
    uint8_t init;
    uint8_t sbc_owns[IMAP_ARB_N];
    int sbc_ref[IMAP_ARB_N];   /* SBC-Stand bei der letzten Maus-Uebernahme */
    int sbc_last[IMAP_ARB_N];  /* SBC-Stand bei der letzten SBC-Bewegung */
    float m_last[IMAP_ARB_N];  /* Mausakku im vorigen Poll */
    float m_move[IMAP_ARB_N];  /* Mausbewegung seit der SBC-Uebernahme */
} imap_arb_t;

typedef struct {
    int cur[IMAP_ARB_N];        /* Blockwert vor SBC (Tastatur/Maus) */
    int idle[IMAP_ARB_N];       /* Leerlaufwert des Blocks (0x7F bzw. 0) */
    unsigned mouse_src;         /* Bit i = Achse i kam von der Maus */
    float mpos[IMAP_ARB_N];     /* Mausakku je Achse (Hebelstufen) */
    int sbc[IMAP_ARB_N];        /* SBC-Wert (Spiel-Einheiten) */
    int sbc_center[IMAP_ARB_N]; /* SBC-Neutral (0x80 bzw. 0) */
    int dead;                   /* SBC-Bewegungsschwelle */
    float wake;                 /* Maus-Bewegungsschwelle */
} imap_arb_in_t;

void imap_arb_axes(imap_arb_t *a, const imap_arb_in_t *in, int out[IMAP_ARB_N]);
/* W132 PAD-ARB: einachsige Arbitrierung Pad gegen Maus (gleiche Logik wie
 * imap_arb_axes). mouse/pad in Offset-Einheiten (0 = neutral), mpos =
 * Mausstellung in Hebelstufen, dead/wake wie W128. Rueckgabe: Gewinner;
 * *wism = 1, wenn die Maus gewann (NULL ok). a = NULL -> Maus. */
int imap_arb_pad1(imap_arb1_t *a, int mouse, int pad, float mpos,
                  int dead, float wake, int *wism);

/* ---- Gamepad-Poll (Hook-Seite) ----
 * Laedt xinput1_4 (Fallback 1_3/9_1_0) dynamisch, fragt Port 0..3, cached
 * pro Frame. 1 = Geraet da (*out gueltig), 0 = keins (sauber inaktiv). */
int imap_pad_poll(unsigned frame, imap_pad_state_t *out);

#ifdef __cplusplus
}
#endif
