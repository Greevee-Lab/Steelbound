#!/usr/bin/env python3
"""test_edition.py - Selbsttest clean_check / Positivliste / package_edition (W116).

Nur synthetische Daten (zur Laufzeit erzeugt, nichts aus dem Spiel):
erfundene "XBE" (Zufallsbytes mit XBEH-Kopf), deren Ausschnitt in eine
Testdatei eingebettet wird, .env mit erfundener Webhook-URL, PNG, absoluter
Pfad, LFS-Zeiger, verbotener Ordner - alles muss erkannt werden; ein
sauberes Staging muss bestehen. Muster, die clean_check selbst finden wuerde
(Webhook, Nutzerpfad), werden hier nur stueckweise zusammengesetzt, damit
diese Datei im Community-Export sauber bleibt.

  python poc-native/tools/edition/tests/test_edition.py      (Exit 0 = gruen)
"""
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
sys.path.insert(0, TOOLS)
import clean_check  # noqa: E402
import edition_common as ec  # noqa: E402

RNG = random.Random(116)


def rand_bytes(n):
    return bytes(RNG.getrandbits(8) for _ in range(n))


def fake_webhook():
    host = "disc" + "ord" + ".com"
    return "https://" + host + "/api/" + "webhooks/" + "1234567890123456" + "/" + "Fake-Token_" + "x" * 20


def fake_user_path():
    return "C:" + "/" + "Users" + "/testnutzer/projekt/datei.c"


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="w116_test_")
        self.ref = os.path.join(self.tmp, "ref")
        self.exp = os.path.join(self.tmp, "export")
        os.makedirs(self.ref)
        os.makedirs(self.exp)
        self.xbe_bytes = b"XBEH" + rand_bytes(64 * 1024)
        self.xbe = os.path.join(self.ref, "default.xbe")
        with open(self.xbe, "wb") as f:
            f.write(self.xbe_bytes)
        self.allow_empty = os.path.join(self.ref, "allow_empty.tsv")
        open(self.allow_empty, "w").close()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def put(self, rel, data):
        p = os.path.join(self.exp, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data if isinstance(data, bytes) else data.encode("utf-8"))
        return p

    def check(self, extra=None, xbe=True):
        rep = os.path.join(self.tmp, "report.md")
        args = [self.exp, "--report", rep, "--quiet",
                "--assets-allow", self.allow_empty, "--clean-allow", self.allow_empty]
        if xbe:
            args += ["--xbe", self.xbe]
        else:
            args += ["--xbe", os.path.join(self.ref, "fehlt.xbe")]
        args += extra or []
        rc = clean_check.main(args)
        with open(os.path.join(self.tmp, "report.json"), encoding="utf-8") as f:
            js = json.load(f)
        self.assertTrue(os.path.isfile(rep))
        return rc, js

    def cats(self, js):
        return {(f["kategorie"], f["datei"]) for f in js["funde"]}

    def clean_files(self):
        self.put("src/main.c", "int main(void)\n{\n    return 0;\n}\n")
        self.put("README.md", "# Test\n\nNur eigener Text, relative Pfade: src/main.c\n")
        self.put("cfg/sb_native.cfg", "xbe_path=\nmedia_root=\nwidth=1920\n")


class TestFindings(Base):
    def test_all_bad_things_detected(self):
        self.clean_files()
        chunk = self.xbe_bytes[5000:5000 + 300]
        self.put("bin/app.dll", rand_bytes(1000) + chunk + rand_bytes(1000))
        lit = self.xbe_bytes[9000:9000 + 24]
        self.put("src/tab.c", "static const unsigned char t[] = {\n    "
                 + ", ".join("0x%02X" % b for b in lit) + "\n};\n")
        self.put(".env", "DISCORD_WEBHOOK_URL=" + fake_webhook() + "\n")
        self.put("docs/notiz.md", "Siehe " + fake_webhook() + " und " + fake_user_path() + "\n")
        self.put("img/logo.png", b"\x89PNG\r\n\x1a\n" + rand_bytes(200))
        self.put("img/getarnt.dat", b"\x89PNG\r\n\x1a\n" + rand_bytes(200))
        self.put("data/TEX.bin", b"version https://git-lfs.github.com/spec/v1\noid sha256:"
                 + b"0" * 64 + b"\nsize 1\n")
        self.put("shots/a.txt", "x\n")
        self.put("game/default.xbe", b"XBEH" + rand_bytes(100))
        rc, js = self.check()
        self.assertEqual(rc, 1)
        c = self.cats(js)
        self.assertIn(("eingebettet", "bin/app.dll"), c)
        self.assertIn(("eingebettet", "src/tab.c"), c)
        self.assertIn(("geheimnis", ".env"), c)
        self.assertIn(("geheimnis", "docs/notiz.md"), c)
        self.assertIn(("pfad", "docs/notiz.md"), c)
        self.assertIn(("medien", "img/logo.png"), c)
        self.assertIn(("medien", "img/getarnt.dat"), c)
        self.assertIn(("lfs", "data/TEX.bin"), c)
        self.assertIn(("spieldatei", "shots/"), c)
        self.assertIn(("spieldatei", "game/default.xbe"), c)
        # saubere Dateien ohne Fund
        for f in js["funde"]:
            self.assertNotIn(f["datei"], ("src/main.c", "README.md", "cfg/sb_native.cfg"))
        # Geheimnis-Werte nie im Bericht
        with open(os.path.join(self.tmp, "report.md"), encoding="utf-8") as f:
            md = f.read()
        raw = json.dumps(js)
        self.assertNotIn("Fake-Token_", md)
        self.assertNotIn("Fake-Token_", raw)
        # eingebettet: Lage stimmt (Datei-Offset 1000 = XBE 5000)
        emb = [f for f in js["funde"] if f["kategorie"] == "eingebettet"
               and f["datei"] == "bin/app.dll"][0]
        self.assertIn("default.xbe @0x", emb["detail"])
        self.assertEqual(js["pruefungen"]["eingebettet"]["status"], "ausgefuehrt")

    def test_chunk_at_guarantee_length_detected(self):
        # 79 Bytes (W + S - 1) werden garantiert erkannt
        chunk = self.xbe_bytes[20001:20001 + 79]
        self.put("bin/x.bin", rand_bytes(333) + chunk + rand_bytes(77))
        rc, js = self.check()
        self.assertEqual(rc, 1)
        self.assertIn(("eingebettet", "bin/x.bin"), self.cats(js))

    def test_skip_without_xbe_is_not_pass(self):
        self.clean_files()
        rc, js = self.check(xbe=False)
        self.assertEqual(rc, 2)
        self.assertEqual(js["pruefungen"]["eingebettet"]["status"], "uebersprungen")

    def test_asset_allowlist(self):
        self.clean_files()
        png = b"\x89PNG\r\n\x1a\n" + rand_bytes(100)
        self.put("res/icon.png", png)
        import hashlib
        allow = os.path.join(self.ref, "assets.tsv")
        with open(allow, "w", encoding="utf-8") as f:
            f.write("res/icon.png\t%s\teigenes Icon (W116-Test)\n" % hashlib.sha256(png).hexdigest())
        rep = os.path.join(self.tmp, "r2.md")
        rc = clean_check.main([self.exp, "--report", rep, "--quiet", "--xbe", self.xbe,
                               "--assets-allow", allow, "--clean-allow", self.allow_empty])
        self.assertEqual(rc, 0)
        with open(allow, "w", encoding="utf-8") as f:  # ohne Herkunft -> ungueltig
            f.write("res/icon.png\t%s\t\n" % hashlib.sha256(png).hexdigest())
        rc = clean_check.main([self.exp, "--report", rep, "--quiet", "--xbe", self.xbe,
                               "--assets-allow", allow, "--clean-allow", self.allow_empty])
        self.assertEqual(rc, 1)


class TestNet(Base):
    def test_network_binary_and_source(self):
        import _socket  # Python-eigene Netz-DLL: importiert ws2_32.dll
        self.clean_files()
        shutil.copyfile(_socket.__file__, os.path.join(self.exp, "netz.pyd"))
        self.put("tool.py", "imp" + "ort url" + "lib.request\n")
        self.put("src/dl.c", 'static const char *v = "http' + '://example.org/datei.zip";\n')
        rc, js = self.check()
        self.assertEqual(rc, 1)
        c = self.cats(js)
        self.assertIn(("netz", "netz.pyd"), c)
        self.assertIn(("netz", "tool.py"), c)
        self.assertIn(("netz", "src/dl.c"), c)
        self.assertGreaterEqual(js["pruefungen"]["netz"]["pe_binaries"], 1)

    def test_embedded_allow_needs_matching_sha(self):
        import hashlib
        chunk = self.xbe_bytes[30000:30000 + 200]
        self.put("bin/rt.dll", rand_bytes(500) + chunk + rand_bytes(500))
        allow = os.path.join(self.ref, "clean_allow.tsv")
        ok_sha = hashlib.sha256(self.xbe_bytes[29968:29968 + 264]).hexdigest()
        for sha, want in ((ok_sha, 0), ("0" * 64, 1)):
            with open(allow, "w", encoding="utf-8") as f:
                f.write("eingebettet\tbin/rt.dll\txbe:%d+264:%s\toeffentliche Laufzeit (Test)\n"
                        % (29968, sha))
            rep = os.path.join(self.tmp, "r3.md")
            rc = clean_check.main([self.exp, "--report", rep, "--quiet", "--xbe", self.xbe,
                                   "--assets-allow", self.allow_empty, "--clean-allow", allow])
            self.assertEqual(rc, want)


class TestClean(Base):
    def test_clean_staging_passes(self):
        self.clean_files()
        # Zufallsdaten ohne XBE-Bezug, kurze Byte-Literale, Xbox-Laufwerk
        self.put("bin/tool.dll", rand_bytes(50000))
        self.put("src/patch.c", "/* Xbox-Pfad D:/Media ist kein Rechnerpfad */\n"
                 "static const unsigned char e[5] = { 0xBF, 0x05, 0x40, 0x00, 0x80 };\n")
        rc, js = self.check()
        self.assertEqual(rc, 0, js["funde"])
        self.assertEqual(js["ergebnis"], "BESTANDEN")


class TestDeny(Base):
    def test_deny_exact_and_embedded_never_allowed(self):
        # W178 COMM-BG: Sperrliste greift exakt (SHA-256) und eingebettet
        # (48-Byte-Signatur aus der Dateimitte); keine Freigabe moeglich.
        import hashlib
        blob = b"TESTD" + rand_bytes(1995)
        sig = blob[len(blob) // 2 - 24:len(blob) // 2 + 24].hex()
        deny = os.path.join(self.ref, "deny.tsv")
        with open(deny, "w", encoding="utf-8") as f:
            f.write("%s\t%s\tsynthetischer Sperr-Eintrag (W178-Test)\n"
                    % (hashlib.sha256(blob).hexdigest(), sig))
        self.clean_files()
        self.put("img/alt.bin", blob)
        self.put("bin/app.dll", rand_bytes(500) + blob[900:1100] + rand_bytes(500))
        allow = os.path.join(self.ref, "clean_allow.tsv")
        with open(allow, "w", encoding="utf-8") as f:
            f.write("gesperrt\timg/alt.bin\t.*\tFreigabeversuch (muss scheitern)\n")
        rc, js = self.check(extra=["--clean-allow", allow, "--clean-deny", deny])
        self.assertEqual(rc, 1)
        c = self.cats(js)
        self.assertIn(("gesperrt", "img/alt.bin"), c)
        self.assertIn(("gesperrt", "bin/app.dll"), c)
        det = {f["datei"]: f["detail"] for f in js["funde"]
               if f["kategorie"] == "gesperrt"}
        self.assertIn("SHA-256", det["img/alt.bin"])
        self.assertIn("eingebettet", det["bin/app.dll"])
        for f in js["funde"]:
            if f["kategorie"] == "gesperrt":
                self.assertNotIn("freigabe", f)


class TestBgAllow(unittest.TestCase):
    def test_repo_bg_allow_matches_files(self):
        # W178 COMM-BG: assets_allow-Eintraege stimmen mit den Dateien im
        # Repo (bzw. Export) ueberein. Nur lesend (Hashes vergleichen).
        # LFS-Zeiger: Soll ist die OID aus dem Zeiger (kein Netz).
        import hashlib
        import re
        root = os.path.dirname(os.path.dirname(os.path.dirname(TOOLS)))
        allow = {}
        with open(os.path.join(root, "editions", "assets_allow.tsv"),
                  encoding="utf-8") as f:
            for ln in f:
                ln = ln.rstrip("\r\n")
                if not ln.strip() or ln.lstrip().startswith("#"):
                    continue
                p = ln.split("\t")
                if len(p) >= 3 and p[2].strip():
                    allow[p[0].replace("\\", "/").lower()] = p[1].strip().lower()
        for rel in ("poc-native/launcher/res/bg_nutzer.png",
                    "poc-native/launcher/res/bg_nutzer.webp"):
            with open(os.path.join(root, rel), "rb") as f:
                data = f.read()
            if data.startswith(b"version https://git-lfs.github.com/spec/v1"):
                m = re.search(rb"^oid sha256:([0-9a-f]{64})", data, re.M)
                self.assertIsNotNone(m, rel + " (LFS-Zeiger ohne OID?)")
                want = m.group(1).decode("ascii")
            else:
                want = hashlib.sha256(data).hexdigest()
            self.assertIn(rel, allow, rel + " fehlt in assets_allow.tsv")
            self.assertEqual(allow[rel], want, rel + " (SHA weicht ab)")


class TestInclude(unittest.TestCase):
    def test_glob_and_negation(self):
        tmp = tempfile.mkdtemp(prefix="w116_inc_")
        try:
            inc = os.path.join(tmp, "x.include")
            with open(inc, "w", encoding="utf-8") as f:
                f.write("# Kommentar\npoc-native/harness/src/**\n"
                        "!poc-native/harness/src/sb_private_paths.h\n"
                        "editions/community/README.md => README.md\n"
                        "a/*.c\n")
            rules = ec.load_include(inc)
            files = ["poc-native/harness/src/hcfg.c", "poc-native/harness/src/sub/x.h",
                     "poc-native/harness/src/sb_private_paths.h",
                     "editions/community/README.md", "a/b.c", "a/sub/c.c", "README.md"]
            sel = ec.select(files, rules)
            self.assertEqual(sel, {
                "poc-native/harness/src/hcfg.c": "poc-native/harness/src/hcfg.c",
                "poc-native/harness/src/sub/x.h": "poc-native/harness/src/sub/x.h",
                "editions/community/README.md": "README.md",
                "a/b.c": "a/b.c"})
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_package_refuses_out_inside_repo(self):
        root = ec.repo_root()
        r = subprocess.run([sys.executable, os.path.join(TOOLS, "package_edition.py"),
                            "--edition", "community", "--out",
                            os.path.join(root, "w116_staging_im_repo")],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertIn("ausserhalb", r.stderr)
        self.assertFalse(os.path.exists(os.path.join(root, "w116_staging_im_repo")))


class TestAlpha(unittest.TestCase):
    """W180: Positiv-Pruefung des Alpha-Binaerpakets (make_alpha.verify_stage)."""

    def setUp(self):
        import make_alpha
        self.ma = make_alpha
        self.tmp = tempfile.mkdtemp(prefix="w180_test_")
        self.st = os.path.join(self.tmp, "Steelbound-alpha1")
        for rel in ["sb_launcher.exe", "sb_ldr.exe", "sb_emu.dll", "sb_native.cfg",
                    "README.md", "LICENSE", "THIRD_PARTY.md", "VERSION.txt",
                    "profiles/keyboard_default.ini", "lang/en.lang", "lang/de.lang",
                    "docs/ALPHA_TESTING.md"]:
            self.put(rel)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def put(self, rel, data=b"x"):
        p = os.path.join(self.st, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)

    def test_clean_stage_passes(self):
        self.assertEqual(self.ma.verify_stage(self.st), [])

    def test_forbidden_files_detected(self):
        for rel in ["sb_emu.pdb", "sb_launcher.ilk", "logs/sb_harness.log",
                    "runs/a/shot.txt", "user.cfg", "portable.txt", "default.xbe",
                    "docs/shot.png", "profiles/sub/x.ini", "extra.exe"]:
            self.put(rel)
        errs = "\n".join(self.ma.verify_stage(self.st))
        for rel in ["sb_emu.pdb", "sb_launcher.ilk", "logs/sb_harness.log",
                    "runs/a/shot.txt", "user.cfg", "portable.txt", "default.xbe",
                    "docs/shot.png", "profiles/sub/x.ini", "extra.exe"]:
            self.assertIn(rel, errs)

    def test_missing_detected(self):
        os.remove(os.path.join(self.st, "sb_emu.dll"))
        shutil.rmtree(os.path.join(self.st, "lang"))
        errs = self.ma.verify_stage(self.st)
        self.assertIn("fehlt: sb_emu.dll", errs)
        self.assertIn("fehlt (leer): lang/", errs)

    def test_check_view_moves_binaries(self):
        view = self.ma.check_view(self.st, os.path.join(self.tmp, "view"))
        have = set(self.ma.list_files(view))
        self.assertIn("bin/sb_emu.dll", have)
        self.assertIn("bin/sb_launcher.exe", have)
        self.assertIn("lang/en.lang", have)
        self.assertNotIn("sb_emu.dll", have)


if __name__ == "__main__":
    unittest.main(verbosity=2)
