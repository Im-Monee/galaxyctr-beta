"""Converts the game's THP movies (MovieData/*.thp) for the 3DS.

The video (Motion-JPEG, 640x368 at 59.94 fps) becomes H.264 for the New 3DS hardware decoder
(MVD, platform/3ds/host/movie.c): baseline profile, every other frame (29.97 fps, the port's
frame rate cap), scaled to the 400x194 the movie covers on the top screen and padded to 512x256
(the texture it is drawn from). The audio (DSP-ADPCM, the 3DS DSP's own format) is kept as is.

The container keeps THP's layout, so the game's player (THPSimplePlayerWrapper) reads it as on
the Wii, little-endian:
- the header, the component table and the audio info as in THP, version VERSION;
- the video info keeps the THP's size (the game draws the movie over it); its third word, the
  video type, holds the encoded size (width << 16 | height);
- one record per THP frame: next record size, previous record size, the component sizes, then
  the components. A video component holds the H.264 NAL units of a picture (3-byte start
  codes), for even frames only; odd frames have an empty one and keep the picture. The audio
  component is the THP's, its record header made little-endian.

Needs ffmpeg (with libx264) on the PATH or given with --ffmpeg.

    python tools/3ds/builder/movie.py <in.thp> <out.thp> [--ffmpeg PATH]
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile

VERSION = 0x3D50001  # H.264 + DSP-ADPCM; the Wii's THP is 0x11000
ENCODED_WIDTH, ENCODED_HEIGHT = 400, 194
PADDED_WIDTH, PADDED_HEIGHT = 512, 256
# x264: baseline (what MVD decodes), one reference frame, an IDR every 4 seconds.
X264 = ["-c:v", "libx264", "-profile:v", "baseline", "-level", "2.1", "-preset", "veryslow", "-crf", "27",
        "-maxrate", "700k", "-bufsize", "1400k", "-x264-params", "ref=1:keyint=120:min-keyint=30:aud=1:repeat-headers=1"]


# ##############################################################################################
# # Reading a THP and encoding its pictures                                                    #
# #                                                                                            #
# # The THP's header, components and frames; its pictures (Motion JPEG on the Wii) are encoded #
# # again by ffmpeg as H.264 at the decoder's size, one picture every other frame (30 a        #
# # second), with the settings above.                                                          #
# ##############################################################################################
class MovieError(Exception):
    pass


def find_ffmpeg(path=None):
    if path:
        return path
    found = shutil.which("ffmpeg")
    if found:
        return found
    # winget's install (Gyan.FFmpeg) adds an alias a running shell may not see yet.
    packages = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Packages")
    for root, _, files in os.walk(packages) if os.path.isdir(packages) else []:
        if "ffmpeg.exe" in files:
            return os.path.join(root, "ffmpeg.exe")
    return None


def read_thp(data):
    """(header fields, component kinds, video info, audio info, [(video bytes, audio bytes)])."""
    if data[:4] != b"THP\0":
        raise MovieError("not a THP file")
    h = struct.unpack_from(">4sIIIfIIIIIII", data, 0)
    _, version, buf_size, audio_max, rate, frames, first_size, _, comp_off, _, movie_off, _ = h
    count = struct.unpack_from(">I", data, comp_off)[0]
    kinds = list(data[comp_off + 4:comp_off + 4 + count])
    off = comp_off + 0x14
    video_info = audio_info = None
    for kind in kinds:
        if kind == 0:
            video_info = struct.unpack_from(">III", data, off)
            off += 12
        elif kind == 1:
            audio_info = struct.unpack_from(">IIII", data, off)
            off += 16
        else:
            raise MovieError("unknown component %d" % kind)
    records = []
    pos, size = movie_off, first_size
    for _ in range(frames):
        next_size = struct.unpack_from(">I", data, pos)[0]
        sizes = struct.unpack_from(">%dI" % count, data, pos + 8)
        p = pos + 8 + 4 * count
        video = audio = b""
        for kind, n in zip(kinds, sizes):
            if kind == 0:
                video = data[p:p + n]
            else:
                tracks = audio_info[3] if audio_info else 1
                audio = data[p:p + n * tracks]
            p += n
        records.append((video, audio))
        pos += size
        size = next_size
    return {"rate": rate, "frames": frames, "audio_max": audio_max}, kinds, video_info, audio_info, records


def audio_little_endian(block, tracks):
    """THP audio records with their header (THPAudioRecordHeader) little-endian."""
    out = bytearray(block)
    step = len(block) // max(tracks, 1)
    for t in range(max(tracks, 1)):
        base = t * step
        if len(block) < base + 0x50:
            continue
        a, b = struct.unpack_from(">II", block, base)
        struct.pack_into("<II", out, base, a, b)
        values = struct.unpack_from(">36h", block, base + 8)
        struct.pack_into("<36h", out, base + 8, *values)
    return bytes(out)


def encode_video(ffmpeg, src, frames, progress=None):
    """The H.264 access units of THP frames 0, 2, 4...: lists of NAL units (without start codes).
    progress(fraction) follows the pictures ffmpeg has encoded."""
    expected = (frames + 1) // 2
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "video.h264")
        errors = os.path.join(tmp, "ffmpeg.log")
        vf = ("select=not(mod(n\\,2)),scale=%d:%d:flags=lanczos,pad=%d:%d:0:0:black,setpts=N/(30000/1001)/TB"
              % (ENCODED_WIDTH, ENCODED_HEIGHT, PADDED_WIDTH, PADDED_HEIGHT))
        cmd = [ffmpeg, "-v", "error", "-nostats", "-progress", "pipe:1", "-y", "-i", src, "-an", "-vf", vf,
               "-r", "30000/1001", "-pix_fmt", "yuv420p"] + X264 + ["-f", "h264", out]
        # No console window for ffmpeg when the converter runs from the GUI on Windows.
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        with open(errors, "w") as log:
            process = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=log, stdin=subprocess.DEVNULL,
                                       text=True, creationflags=flags)
            for line in process.stdout:
                if line.startswith("frame=") and progress:
                    progress(min(1.0, int(line[6:].strip() or 0) / max(expected, 1)))
            code = process.wait()
        if code:
            raise MovieError("ffmpeg failed (%d): %s" % (code, open(errors).read().strip()[-400:]))
        stream = open(out, "rb").read()
    nals = split_nals(stream)
    units, current = [], None
    for nal in nals:
        kind = nal[0] & 0x1F
        if kind == 9:  # access unit delimiter: a new picture
            current = []
            units.append(current)
            continue
        if kind == 6:  # SEI: not needed by the decoder
            continue
        if current is None:
            current = []
            units.append(current)
        current.append(nal)
    if len(units) != expected:
        raise MovieError("%d pictures encoded, %d expected" % (len(units), expected))
    return units


def split_nals(stream):
    out, i, n = [], 0, len(stream)
    starts = []
    while True:
        j = stream.find(b"\0\0\1", i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else n
        nal = stream[s:e].rstrip(b"\0")
        if nal:
            out.append(nal)
    return out


def pad(data, align):
    return data + b"\0" * (-len(data) % align)


# ##############################################################################################
# # Writing the 3DS movie                                                                      #
# #                                                                                            #
# # The THP's layout with the H.264 pictures in place of the JPEG ones and the audio records   #
# # made little-endian (the layout is in the docstring above).                                 #
# ##############################################################################################
def convert(src, dst, ffmpeg=None, progress=None):
    """progress(fraction) while encoding (most of the time) and writing."""
    ffmpeg = find_ffmpeg(ffmpeg)
    if ffmpeg is None:
        raise MovieError("ffmpeg not found")
    data = open(src, "rb").read()
    info, kinds, video_info, audio_info, records = read_thp(data)
    pictures = encode_video(ffmpeg, src, info["frames"], progress and (lambda f: progress(0.95 * f)))
    tracks = audio_info[3] if audio_info else 1
    del data

    bodies = []
    for i, (_, audio) in enumerate(records):
        comps, sizes = [], []
        for kind in kinds:
            if kind == 0:
                video = b"".join(b"\0\0\1" + nal for nal in pictures[i // 2]) if i % 2 == 0 else b""
                video = pad(video, 4)
                comps.append(video)
                sizes.append(len(video))
            else:
                block = audio_little_endian(audio, tracks)
                comps.append(block)
                sizes.append(len(block) // max(tracks, 1))
        bodies.append((sizes, b"".join(comps)))
    record_sizes = [len(pad(b"\0" * (8 + 4 * len(kinds)) + body, 32)) for _, body in bodies]

    header_size = 0x30
    comp_off = header_size
    info_size = 0x14 + sum(12 if k == 0 else 16 for k in kinds)
    movie_off = comp_off + ((info_size + 31) // 32) * 32
    out = bytearray(movie_off)
    total = sum(record_sizes)
    struct.pack_into("<4sIIIfIIIIIII", out, 0, b"THP\0", VERSION, max(record_sizes), info["audio_max"], info["rate"], info["frames"],
                     record_sizes[0], total, comp_off, 0, movie_off, movie_off + total - record_sizes[-1])
    struct.pack_into("<I", out, comp_off, len(kinds))
    out[comp_off + 4:comp_off + 4 + len(kinds)] = bytes(kinds)
    p = comp_off + 0x14
    for kind in kinds:
        if kind == 0:
            struct.pack_into("<III", out, p, video_info[0], video_info[1], (ENCODED_WIDTH << 16) | ENCODED_HEIGHT)
            p += 12
        else:
            struct.pack_into("<IIII", out, p, *audio_info)
            p += 16
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(out)
        for i, (sizes, body) in enumerate(bodies):
            next_size = record_sizes[i + 1] if i + 1 < len(bodies) else 0
            prev_size = record_sizes[i - 1] if i > 0 else 0
            rec = struct.pack("<II%dI" % len(sizes), next_size, prev_size, *sizes) + body
            f.write(pad(rec, 32))
    if progress:
        progress(1.0)
    return {"frames": info["frames"], "pictures": len(pictures), "bytes": movie_off + total}


def is_converted(path):
    with open(path, "rb") as f:
        head = f.read(8)
    return len(head) == 8 and head[:4] == b"THP\0" and struct.unpack_from("<I", head, 4)[0] == VERSION


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    ff = sys.argv[sys.argv.index("--ffmpeg") + 1] if "--ffmpeg" in sys.argv else None
    if ff in args:
        args.remove(ff)
    result = convert(args[0], args[1], ff)
    print("%s: %d frames, %d pictures, %.1f MB" % (os.path.basename(args[1]), result["frames"], result["pictures"], result["bytes"] / 1e6))
