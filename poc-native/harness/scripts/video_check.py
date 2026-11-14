"""W255 VIDEO-EXPORT: exportierte MP4 pruefen (ohne Pfade/Bildinhalte in der Ausgabe).

Usage:
  python scripts/video_check.py <datei.mp4> [--size 1920x1080] [--fps 30] [--frames N] [--audio]
                                [--stills OUT_PREFIX --at 0.2,0.5,0.8 --width 640]
Benoetigt ffprobe (ffmpeg fuer --stills / --decode). Ausgabe: JSON mit Codec, Profil,
Aufloesung, Bildrate, Bildzahl (Pakete), Dauer, Tonspur, Groesse. Exit 1 bei Abweichung.
--decode dekodiert die ganze Datei (Fehlerzaehler). --stills schreibt verkleinerte PNGs
(Belegbilder; nur bereinigte Spielbilder, keine Desktop-Inhalte).
"""
import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


def probe(path):
    exe = shutil.which('ffprobe')
    if not exe:
        raise SystemExit('ffprobe fehlt')
    out = subprocess.run([exe, '-v', 'error', '-count_packets', '-show_streams', '-show_format', '-of', 'json',
                          str(path)], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def rate(s):
    a, _, b = (s or '0/1').partition('/')
    return float(a) / float(b or 1) if float(b or 1) else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mp4')
    ap.add_argument('--size')
    ap.add_argument('--fps', type=float)
    ap.add_argument('--frames', type=int)
    ap.add_argument('--audio', action='store_true')
    ap.add_argument('--decode', action='store_true')
    ap.add_argument('--stills')
    ap.add_argument('--at', default='0.25,0.5,0.75')
    ap.add_argument('--width', type=int, default=640)
    a = ap.parse_args()
    p = Path(a.mp4)
    info = probe(p)
    v = next((s for s in info['streams'] if s['codec_type'] == 'video'), None)
    au = next((s for s in info['streams'] if s['codec_type'] == 'audio'), None)
    res = {
        'file': p.name, 'bytes': p.stat().st_size, 'container': info['format'].get('format_name'),
        'duration': round(float(info['format'].get('duration', 0)), 3),
    }
    errs = []
    if not v:
        errs.append('keine Videospur')
    else:
        res.update(video=v.get('codec_name'), profile=v.get('profile'), width=v.get('width'),
                   height=v.get('height'), fps=round(rate(v.get('avg_frame_rate')), 3),
                   frames=int(v.get('nb_read_packets', v.get('nb_frames', 0))),
                   v_duration=round(float(v.get('duration', 0)), 3), pix_fmt=v.get('pix_fmt'),
                   color=v.get('color_space'), range=v.get('color_range'),
                   kbps=round(int(v.get('bit_rate', 0)) / 1000) if v.get('bit_rate') else None)
        if v.get('codec_name') != 'h264':
            errs.append('Video nicht H.264')
        if a.size and f"{v.get('width')}x{v.get('height')}" != a.size:
            errs.append(f"Groesse {v.get('width')}x{v.get('height')} statt {a.size}")
        if a.fps and abs(rate(v.get('avg_frame_rate')) - a.fps) > 0.01:
            errs.append(f"Bildrate {res['fps']} statt {a.fps}")
        if a.frames is not None and res['frames'] != a.frames:
            errs.append(f"Bildzahl {res['frames']} statt {a.frames}")
        if res['fps'] and abs(res['frames'] / res['fps'] - res['v_duration']) > 1.5 / res['fps']:
            errs.append('Bildzahl passt nicht zur Dauer (keine konstante Bildrate?)')
    if au:
        res.update(audio=au.get('codec_name'), a_rate=int(au.get('sample_rate', 0)), a_ch=au.get('channels'),
                   a_duration=round(float(au.get('duration', 0)), 3))
        if v and abs(res['a_duration'] - res['v_duration']) > 0.05:
            errs.append(f"Tonlaenge {res['a_duration']} s weicht von Video {res['v_duration']} s ab")
    elif a.audio:
        errs.append('keine Tonspur')
    if a.decode:
        ff = shutil.which('ffmpeg')
        r = subprocess.run([ff, '-v', 'error', '-i', str(p), '-f', 'null', '-'], capture_output=True, text=True)
        res['decode_errors'] = len([ln for ln in r.stderr.splitlines() if ln.strip()])
        if r.returncode or res['decode_errors']:
            errs.append('Dekodierfehler')
    if a.stills and v:
        ff = shutil.which('ffmpeg')
        shots = []
        for i, f in enumerate(float(x) for x in a.at.split(',')):
            out = f"{a.stills}{i + 1}.png"
            subprocess.run([ff, '-v', 'error', '-y', '-ss', f"{res['v_duration'] * f:.3f}", '-i', str(p), '-frames:v', '1',
                            '-vf', f"scale={a.width}:-2", out], check=True)
            shots.append(Path(out).name)
        res['stills'] = shots
    res['ok'] = not errs
    if errs:
        res['errors'] = errs
    print(json.dumps(res, ensure_ascii=False))
    return 0 if not errs else 1


if __name__ == '__main__':
    sys.exit(main())
