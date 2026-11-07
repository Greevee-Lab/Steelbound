/* render_selftest_xbe.h - W152 COMM-SELFTEST, erzeugt von
 * scripts/selftest_vectors.py aus render_selftest_data.h +
 * der eigenen default.xbe (PAL, 6631424 Bytes).
 * Enthaelt NUR Offsets + Pruefsummen (Fakten, keine Spielbytes):
 * je Vektor (Handle, Datei-Offset, Wortzahl, FNV-1a32). Der
 * Community-Build liest die Vektoren daraus zur Laufzeit und
 * verweigert bei Pruefsummen-Fehler (falsche XBE-Version).
 * NICHT VON HAND AENDERN - bei Aenderung der Vektoren neu erzeugen. */
#ifndef RENDER_SELFTEST_XBE_H
#define RENDER_SELFTEST_XBE_H
/* Ein Vektor in der XBE-Datei. */
typedef struct { unsigned handle, off, nwords, fnv; } st_xbe_vec_t;
static const st_xbe_vec_t st_xbe_vs[] = {
    { 0xAB6EAB1, 0x2A0ED4, 76, 0x35C159F6 },
    { 0xAB6F011, 0x2A11C4, 140, 0xE93C516A },
    { 0xAB6F381, 0x2A13FC, 172, 0x038DAB05 },
    { 0xAB6F771, 0x2A16B4, 204, 0x9FBC602D },
    { 0xAB6FBF1, 0x2A19EC, 236, 0xE325C9F6 },
    { 0xAB70671, 0x2A21DC, 300, 0xC2256A49 },
    { 0xAB75FF1, 0x2A6594, 160, 0xBECD0E3A },
    { 0xAB763B1, 0x2A681C, 192, 0x894D970E },
    { 0xAB767F1, 0x2A6B24, 224, 0x2FC9C366 },
    { 0xAB76CC1, 0x2A6EAC, 256, 0x92966403 },
    { 0xAB7D3C1, 0x2AC6D4, 28, 0x5023650D },
    { 0xAB7D561, 0x2AC74C, 24, 0xB4411E7E },
    { 0xAB7D6F1, 0x2ABC3C, 36, 0x2BB91C34 },
    { 0xAB7D8B1, 0x2ABC3C, 36, 0x2BB91C34 },
    { 0xAB7DA71, 0x2ABCD4, 40, 0xAD4ED99B },
    { 0xAB7DC41, 0x2ABD7C, 24, 0xBF4CAD5D },
    { 0xAB7DDD1, 0x2ABD7C, 24, 0xBF4CAD5D },
    { 0xAB7E131, 0x2ABEF4, 40, 0x05A2B311 },
    { 0xAB7E301, 0x2ABF98, 8, 0xC8D1F4F6 },
    { 0xAB7E451, 0x2ABFBC, 12, 0x9D87ED53 },
    { 0xAB7E5B1, 0x2ABFF4, 36, 0x9077929C },
    { 0xAB7E771, 0x2AC08C, 40, 0x3A1A7B07 },
    { 0xAB7E941, 0x2AC134, 20, 0x99BB71B5 },
    { 0xAB7EAC1, 0x2AC18C, 28, 0x84BAE07F },
    { 0xAB7EC61, 0x2AC204, 28, 0xCF245331 },
    { 0xAB7F011, 0x2AC354, 32, 0xD34386E8 },
    { 0xAB7F421, 0x2AC08C, 40, 0x3A1A7B07 },
};
static const st_xbe_vec_t st_xbe_ps[] = {
    { 0xAB7F850, 0x2AC7B0, 60, 0x8CF65F7B },
    { 0xAB7F960, 0x2AC8A0, 60, 0x52AE952A },
    { 0xAB7FC90, 0x2ACB70, 60, 0x12E3855B },
    { 0xAB7FDA0, 0x2ACC60, 60, 0xC9AA9626 },
    { 0xAB7FFC0, 0x2ACE40, 60, 0xCCBA1AAD },
    { 0xAB800D0, 0x2ACF30, 60, 0x86C06341 },
    { 0xAB801E0, 0x2AD020, 60, 0x87F7D7FB },
    { 0xAB802F0, 0x2AD110, 60, 0x6CEB7057 },
    { 0xAB80400, 0x2AD200, 60, 0x986E1B8E },
    { 0xAB80510, 0x2AD2F0, 60, 0x72C6CD23 },
    { 0xAB80620, 0x2AD3E0, 60, 0x0A7DF746 },
    { 0xAB80730, 0x2AD4D0, 60, 0x2E85E7AB },
    { 0xAB80950, 0x2AD6B0, 60, 0x8680C0C6 },
    { 0xAB813F0, 0x2AE010, 60, 0xE56ECA91 },
    { 0xAB81610, 0x2AD2F0, 60, 0x72C6CD23 },
    { 0xAB81940, 0x2AE4C0, 60, 0x5130C2E7 },
    { 0xAB81B60, 0x2AE6A0, 60, 0x26228952 },
    { 0xAB82600, 0x2AD4D0, 60, 0x2E85E7AB },
};
#define ST_XBE_NVS 27
#define ST_XBE_NPS 18
#define ST_XBE_VS_WORDS 2456
#endif /* RENDER_SELFTEST_XBE_H */
