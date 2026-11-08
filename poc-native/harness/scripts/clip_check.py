"""W210: validate local clip artifacts without publishing paths or game pixels.
Usage: python scripts/clip_check.py <package-directory> [--fallback] [--removed]
ffprobe + full ffmpeg decode if installed; otherwise MP4 mvhd/tkhd container check.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess


def boxes(data, start=0, end=None):
    end = len(data) if end is None else end
    while start + 8 <= end:
        size, kind = struct.unpack_from('>I4s', data, start)
        head = 8
        if size == 1:
            size = struct.unpack_from('>Q', data, start + 8)[0]
            head = 16
        if size == 0:
            size = end - start
        assert size >= head and start + size <= end, 'Invalid MP4 box'
        yield kind, data[start + head:start + size]
        start += size
    assert start == end, 'Trailing incomplete MP4 box'


def check(package, fallback=False, removed=False, synthetic=False):
    mp4 = package / 'clip.mp4'
    jpgs = sorted(package.glob('clip_*.jpg'))
    if removed:
        assert not mp4.exists() and not jpgs and not (package / 'clip.txt').exists()
        return {'removed': True}
    if fallback:
        from PIL import Image
        assert not mp4.exists() and len(jpgs) == 50
        dimensions = set()
        for i, path in enumerate(jpgs):
            assert path.name == f'clip_{i:03d}.jpg'
            with Image.open(path) as im:
                dimensions.add(im.size)
                im.verify()
        assert len(dimensions) == 1
        w, h = dimensions.pop()
        size = sum(p.stat().st_size for p in jpgs)
        note = (package / 'clip.txt').read_text()
        assert 'MP4 unavailable' in note and '100 ms' in note
        result = dict(format='JPG', frames=50, duration=5.0, width=w, height=h, bytes=size)
    else:
        assert mp4.exists() and not jpgs, 'MP4 missing or safety JPGs not removed'
        data = mp4.read_bytes()
        top = dict(boxes(data))
        assert b'ftyp' in top and b'moov' in top and b'mdat' in top
        moov = dict(boxes(top[b'moov']))
        mvhd = moov[b'mvhd']
        if mvhd[0] == 1:
            scale, duration = struct.unpack_from('>IQ', mvhd, 20)
        else:
            scale, duration = struct.unpack_from('>II', mvhd, 12)
        trak = dict(boxes(moov[b'trak']))
        w, h = struct.unpack_from('>II', trak[b'tkhd'], len(trak[b'tkhd']) - 8)
        result = dict(format='MP4', duration=duration / scale, width=w >> 16, height=h >> 16, bytes=len(data))
        assert b'avc1' in top[b'moov'], 'No AVC track'
        if shutil.which('ffprobe'):
            probe = subprocess.run(['ffprobe', '-v', 'error', '-show_streams', '-of', 'json', str(mp4)], capture_output=True, check=True)
            streams = json.loads(probe.stdout)['streams']
            assert len(streams) == 1 and streams[0]['codec_name'] == 'h264'
            assert streams[0]['nb_frames'] == '150' and streams[0]['r_frame_rate'] == '30/1'
            result['frames'] = 150
        if shutil.which('ffmpeg'):
            decoded = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(mp4), '-f', 'null', '-'], capture_output=True)
            assert decoded.returncode == 0 and not decoded.stderr, 'Full video decode failed'
            result['decoded'] = True
    if synthetic:
        if fallback:
            from PIL import Image
            with Image.open(jpgs[0]) as im:
                first = im.convert('RGB').getpixel((0, 0))
            with Image.open(jpgs[-1]) as im:
                last = im.convert('RGB').getpixel((0, 0))
        else:
            # Decode the small synthetic test to 1x1 RGB frames. No game pixels printed.
            proc = subprocess.run(['ffmpeg', '-v', 'error', '-i', str(mp4), '-vf', 'scale=1:1',
                                   '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'], capture_output=True, check=True)
            assert len(proc.stdout) == 150 * 3
            first, last = proc.stdout[:3], proc.stdout[-3:]
        assert first[2] > 200 and first[0] < 50 and 40 < first[1] < 90, 'First frame color'
        assert last[0] > 200 and last[2] < 50 and 40 < last[1] < 90, 'Last frame color'
        result['synthetic_colors_without_marker'] = True
    assert 0 < result['bytes'] < 9500000
    assert 0 < result['width'] <= 1280 and 0 < result['height'] <= 720
    assert abs(result['duration'] - 5.0) < 0.001
    return result


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('package', type=Path)
    ap.add_argument('--fallback', action='store_true')
    ap.add_argument('--removed', action='store_true')
    ap.add_argument('--synthetic', action='store_true')
    args = ap.parse_args()
    try:
        print(json.dumps(check(args.package, args.fallback, args.removed, args.synthetic), sort_keys=True))
    except Exception as exc:
        # Do not echo user paths or decoder stderr in shareable evidence.
        print('CLIP CHECK FAILED: ' + type(exc).__name__)
        raise SystemExit(1)
