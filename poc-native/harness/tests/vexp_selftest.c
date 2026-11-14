/* W255 VIDEO-EXPORT: Selbsttest ohne Spiel/Fenster/D3D.
 *   vexp_selftest                 -> Zeitachse, Farbwandlung, Bitrate (rein)
 *   vexp_selftest <Ordner>        -> zusaetzlich synthetische MP4 (1080p30 mit Ton,
 *                                    2160p60 ohne Ton) in <Ordner> (lokal, nie ins Repo)
 * Pruefung der Dateien: python scripts/video_check.py <datei> --frames N --size WxH. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "vexp_enc.h"

static int fails;
#define CHECK(c, m) do { if (!(c)) { printf("  FEHLER: %s\n", m); fails++; } } while (0)

static void t_timeline(void)
{
    vx_tl_t t;
    unsigned c, i, sum;
    /* 30 FPS Ausgabe, Spiel-Ticks alle 2 VBlanks: jedes Bild genau 1 Ausgabebild */
    vx_tl_init(&t, 2.0);
    CHECK(vx_tl_feed(&t, 100.0) == 0, "erstes Bild belegt nichts");
    for (i = 1, sum = 0; i <= 10; i++) { c = vx_tl_feed(&t, 100.0 + 2.0 * i); CHECK(c == 1, "30/30: 1 Bild je Tick"); sum += c; }
    CHECK(sum == 10 && t.n == 10, "30/30: 10 Bilder");
    CHECK(vx_tl_end(&t, 122.0) == 1, "Ende: letztes Bild 1x");
    /* 60 FPS, Ticks alle 2 VBlanks ohne Zwischenbilder: jedes Bild 2x */
    vx_tl_init(&t, 1.0);
    vx_tl_feed(&t, 0.0);
    for (i = 1; i <= 5; i++) { CHECK(vx_tl_feed(&t, 2.0 * i) == 2, "60 ohne Interp: 2 je Tick"); }
    /* 60 FPS mit K=2 Zwischenbildern je Tick (Zeiten c0+1, c0+2) */
    vx_tl_init(&t, 1.0);
    vx_tl_feed(&t, 10.0);
    for (i = 0; i < 6; i++) {
        double c0 = 10.0 + 2.0 * i, c1 = c0 + 2.0;
        CHECK(vx_tl_feed(&t, vx_batch_time(c0, c1, 1, 2)) == 1, "60 mit Interp: Zwischenbild 1x");
        CHECK(vx_tl_feed(&t, vx_batch_time(c0, c1, 2, 2)) == 1, "60 mit Interp: Tickbild 1x");
    }
    CHECK(t.n == 12, "60 mit Interp: 12 Bilder in 6 Ticks");
    /* Luecke (Ladehaenger 6 VBlanks) bei 30 FPS: altes Bild 3x gehalten */
    vx_tl_init(&t, 2.0);
    vx_tl_feed(&t, 0.0);
    CHECK(vx_tl_feed(&t, 6.0) == 3, "Luecke: 3x halten");
    /* 120 Bilder/s Eingang (K=4) bei 30 FPS: Zwischenbilder entfallen, keines doppelt */
    vx_tl_init(&t, 2.0);
    vx_tl_feed(&t, 0.0);
    for (i = 1, sum = 0; i <= 4 * 15; i++) { sum += vx_tl_feed(&t, 0.5 * i); }
    CHECK(sum == 15, "K=4 bei 30 FPS: 15 Bilder in 15 Ticks");
    CHECK(vx_batch_time(4.0, 6.0, 1, 1) == 6.0, "Batch 1 Bild = Tickzeit");
}

static void t_color(void)
{
    unsigned char px[4 * 4], nv[4 + 2];
    int i;
    const struct { unsigned char r, g, b, y, u, v; } c[] = {
        { 0, 0, 0, 16, 128, 128 }, { 255, 255, 255, 235, 128, 128 }, { 128, 128, 128, 126, 128, 128 },
        { 255, 0, 0, 63, 102, 240 }, { 0, 255, 0, 172, 42, 26 }, { 0, 0, 255, 32, 240, 118 },
    };
    for (i = 0; i < (int)(sizeof c / sizeof c[0]); i++) {
        int k;
        char m[80];
        for (k = 0; k < 4; k++) { px[k * 4] = c[i].r; px[k * 4 + 1] = c[i].g; px[k * 4 + 2] = c[i].b; px[k * 4 + 3] = 255; }
        vx_rgba_to_nv12(px, 2, 2, nv);
        _snprintf_s(m, sizeof m, _TRUNCATE, "NV12 Farbe %d: Y=%d U=%d V=%d", i, nv[0], nv[4], nv[5]);
        CHECK(abs(nv[0] - c[i].y) <= 1 && abs(nv[4] - c[i].u) <= 1 && abs(nv[5] - c[i].v) <= 1, m);
        CHECK(nv[0] == nv[1] && nv[0] == nv[2] && nv[0] == nv[3], "NV12: gleiche Luma im 2x2-Block");
    }
    CHECK(vx_auto_kbps(1920, 1080, 30) >= 9000 && vx_auto_kbps(1920, 1080, 30) <= 9700, "Auto-Bitrate 1080p30 ~9,3 Mbit/s");
    CHECK(vx_auto_kbps(3840, 2160, 60) > 70000 && vx_auto_kbps(7680, 4320, 60) == 100000, "Auto-Bitrate 4K60 ~75 Mbit/s, Deckel 100");
    CHECK(vx_auto_kbps(320, 240, 30) == 2000, "Auto-Bitrate Untergrenze");
}

/* synthetisches Bild: Farbverlauf + laufender Balken (Bewegung je Bild sichtbar) */
static void fill(unsigned char *p, unsigned w, unsigned h, unsigned f)
{
    unsigned x, y, bar = (f * 16u) % w;
    for (y = 0; y < h; y++) {
        unsigned char *r = p + (size_t)y * w * 4;
        for (x = 0; x < w; x++, r += 4) {
            int on = x >= bar && x < bar + w / 20u;
            r[0] = on ? 255 : (unsigned char)(x * 255u / w);
            r[1] = on ? 255 : (unsigned char)(y * 255u / h);
            r[2] = on ? 255 : 96;
            r[3] = 255;
        }
    }
}

static int encode(const char *dir, const char *name, unsigned w, unsigned h, unsigned fps, int audio, unsigned images,
                  unsigned rep, int hw)
{
    char p8[600], info[200];
    wchar_t pw[600];
    unsigned i, frames = 0;
    double secs = 0;
    unsigned long long afr = 0, adrop = 0, at = 0;
    DWORD t0 = GetTickCount();
    int ok;
    _snprintf_s(p8, sizeof p8, _TRUNCATE, "%s\\%s", dir, name);
    MultiByteToWideChar(CP_ACP, 0, p8, -1, pw, 600);
    if (!vxe_open(pw, w, h, fps, 0, audio, hw, info, sizeof info)) {
        printf("  %s: Encoder nicht verfuegbar: %s\n", name, info);
        return 0;
    }
    printf("  %s: %s\n", name, info);
    if (audio) { vxe_audio_skip(441); } /* 10 ms Versatz wie im Spiel */
    for (i = 0; i < images; i++) {
        unsigned char *b = vxe_frame_begin();
        if (!b) { break; }
        fill(b, w, h, i);
        vxe_frame_end(rep);
        if (audio) { /* Sinus 440 Hz passend zur Bildzeit (+Versatz) */
            unsigned long long until = (unsigned long long)(i + 1u) * rep * VXE_RATE / fps + 441u;
            short buf[882 * 2];
            while (at < until) {
                unsigned n = (unsigned)((until - at) < 882u ? (until - at) : 882u), k;
                for (k = 0; k < n; k++) {
                    short s = (short)(8000.0 * sin(2.0 * 3.14159265358979 * 440.0 * (double)(at + k) / VXE_RATE));
                    buf[k * 2] = s; buf[k * 2 + 1] = s;
                }
                vxe_audio(buf, n);
                at += n;
            }
        }
    }
    ok = vxe_close(&frames, &secs, &afr, &adrop);
    printf("  %s: ok=%d hr=0x%08lX Bilder=%u Dauer=%.3f s Ton-Frames=%llu verworfen=%llu (%.1f s, %.1f ms/Bild)\n", name, ok, vxe_hr(),
           frames, secs, afr, adrop, (GetTickCount() - t0) / 1000.0, vxe_ms_per_frame());
    CHECK(ok, "Encoder schliesst sauber");
    CHECK(frames == images * rep, "Bildzahl = Bilder x Wiederholung");
    if (audio) { CHECK(afr == (unsigned long long)frames * VXE_RATE / fps, "Tonlaenge = Videolaenge"); }
    CHECK(adrop == 0, "kein Ton verworfen");
    return ok;
}

int main(int argc, char **argv)
{
    printf("vexp_selftest (W255 VIDEO-EXPORT)\n");
    t_timeline();
    t_color();
    if (argc > 1) {
        CreateDirectoryA(argv[1], NULL);
        encode(argv[1], "vexp_1080p30.mp4", 1920, 1080, 30, 1, 60, 1, 1);      /* 2,0 s, 60 Bilder */
        encode(argv[1], "vexp_1080p30_rep.mp4", 1920, 1080, 30, 1, 20, 3, 1);  /* 2,0 s, gehalten */
        encode(argv[1], "vexp_2160p60.mp4", 3840, 2160, 60, 0, 60, 1, 1);      /* 1,0 s 4K60 ohne Ton */
        encode(argv[1], "vexp_1080p30_sw.mp4", 1920, 1080, 30, 1, 30, 1, 0);   /* Software-Encoder */
        encode(argv[1], "vexp_2160p30_sw.mp4", 3840, 2160, 30, 0, 30, 1, 0);   /* Software 4K */
    }
    printf("vexp_selftest: %d Fehler\n", fails);
    return fails ? 1 : 0;
}
