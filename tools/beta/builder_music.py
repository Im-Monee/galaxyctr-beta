"""The game's music in the builder window, from the user's own disc.

The game's background music is streamed from AudioRes/Stream: .ast files ("STRM"), 16-bit PCM
at 32 kHz in blocks, with the point the music loops back to. builder_assets.make() turns the
pieces the window plays into WAV files (convert_ast); Music plays one of them through Qt's audio
output, as the game does: from the start, then round and round from the loop point to the end.

The window's button pauses and resumes it. A paused piece keeps its place: the next sample to
play is counted here, not by the audio device, so it goes on exactly where it stopped.
"""

import array
import struct
import sys
import time
import wave
from pathlib import Path

# The pieces the window plays: its name and the game's stream (Game/AudioLib/AudBgmSetting.cpp
# and Game/GameAudio/AudStageBgmWrap.cpp for which stream is which music).
MUSIC = {
    "title": "SMG_title_strm.ast",  # STM_TITLE: the title screen (TitleSequenceProduct::exeBgmPrepare)
    "observatory": "SMG_astroout01_strm.ast",  # STM_ASTRO_OUT: the Comet Observatory, outside
}

# ##############################################################################################
# # The music's volume against the sound effects                                              #
# #                                                                                            #
# # The game plays a stream at its sound table volume times the stream manager's 2.6          #
# # (AudParams::masterVolStream), and a system sound effect at its own table volume (already  #
# # in the WAV, builder_sounds.render) times the sound effect manager's 1.2 (masterVolSe) and #
# # its category's 1.2 (scCtgVolume, SE_SY in the first row). The sound effects are written  #
# # at builder_sounds.GAIN, so the music plays at the same share of the game's level.        #
# ##############################################################################################
STREAM_TABLE_VOLUME = {"title": 90 / 255.0, "observatory": 55 / 255.0}  # STM_TITLE, STM_ASTRO_OUT in SMR.szs
MASTER_STREAM, MASTER_SE, CATEGORY_SE_SY = 2.6, 1.2, 1.2
SE_GAIN = 0.3  # builder_sounds.GAIN


def volume(name):
    return SE_GAIN * MASTER_STREAM * STREAM_TABLE_VOLUME.get(name, 0.3) / (MASTER_SE * CATEGORY_SE_SY)


# ##############################################################################################
# # The stream files                                                                           #
# #                                                                                            #
# # Header (0x40 bytes, big-endian): "STRM", the size of the blocks, format (1 = PCM16), bits, #
# # channels, a flag, the sample rate, the number of samples, the loop start and end (samples) #
# # and the size of the first block. Each block: "BLCK", its size per channel, 0x18 bytes, then #
# # that many bytes for each channel in turn.                                                  #
# ##############################################################################################
def read_ast(data):
    """(rate, channels, loop_start, loop_end, samples): samples is an array('h') of interleaved
    frames in this computer's byte order."""
    if data[:4] != b"STRM":
        raise ValueError("not a stream file")
    fmt, bits, channels = struct.unpack_from(">HHH", data, 8)
    rate, total, loop_start, loop_end = struct.unpack_from(">IIII", data, 0x10)
    if fmt != 1 or bits != 16:
        raise ValueError("only 16-bit PCM streams are read")
    parts = [array.array("h") for _ in range(channels)]
    at = 0x40
    while at + 0x20 <= len(data) and data[at:at + 4] == b"BLCK":
        size = struct.unpack_from(">I", data, at + 4)[0]
        at += 0x20
        for part in parts:
            part.frombytes(data[at:at + size])
            at += size
    out = array.array("h", bytes(2 * channels * total))
    for c, part in enumerate(parts):
        if sys.byteorder == "little":
            part.byteswap()
        out[c::channels] = part[:total]
    return rate, channels, loop_start, min(loop_end, total), out


def convert_ast(source, target):
    """Writes the stream source as the WAV file target; {rate, loop_start, loop_end} for the
    manifest."""
    rate, channels, loop_start, loop_end, samples = read_ast(Path(source).read_bytes())
    if sys.byteorder == "big":
        samples.byteswap()
    with wave.open(str(target), "wb") as out:
        out.setnchannels(channels)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(samples.tobytes())
    return {"rate": rate, "channels": channels, "loop_start": loop_start, "loop_end": loop_end}


# ##############################################################################################
# # Playing                                                                                    #
# ##############################################################################################
def _qt():
    from PySide6.QtCore import QIODevice, QObject, QTimer
    from PySide6.QtMultimedia import QAudio, QAudioFormat, QAudioSink, QMediaDevices
    return QIODevice, QObject, QTimer, QAudio, QAudioFormat, QAudioSink, QMediaDevices


class Music:
    """The window's music: play(name) starts a piece of the manifest's music, set_paused keeps
    its place, fade_out stops it over some frames of the game (60 a second)."""

    def __init__(self, folder, manifest, log=print):
        self.folder = Path(folder)
        self.pieces = manifest.get("music", {})
        self.log = log
        self.sink = self.device = None
        self.current = None
        self.paused = False
        self.places = {}  # name -> the next frame to play, kept when the piece changes
        self.cache = {}

    def has(self, name):
        return name in self.pieces

    def _pcm(self, name):
        if name not in self.cache:
            info = self.pieces[name]
            with wave.open(str(self.folder / info["file"]), "rb") as w:
                self.cache[name] = (w.readframes(w.getnframes()), w.getframerate(), w.getnchannels())
        return self.cache[name]

    def play(self, name):
        """Starts name where it was left (from the start the first time); the piece that played
        keeps its place."""
        if name == self.current or name not in self.pieces:
            return
        try:
            self._stop_sink()
            QIODevice, _QObject, _QTimer, _QAudio, QAudioFormat, QAudioSink, QMediaDevices = _qt()
            pcm, rate, channels = self._pcm(name)
            info = self.pieces[name]
            fmt = QAudioFormat()
            fmt.setSampleRate(rate)
            fmt.setChannelCount(channels)
            fmt.setSampleFormat(QAudioFormat.SampleFormat.Int16)
            output = QMediaDevices.defaultAudioOutput()
            if output.isNull() or not output.isFormatSupported(fmt):
                self.log(f"No music: the sound output does not take {rate} Hz stereo.")
                return
            self.device = _loop_device(QIODevice)(pcm, channels * 2, info["loop_start"], info["loop_end"],
                                                  self.places.get(name, 0))
            self.device.open(QIODevice.OpenModeFlag.ReadOnly)
            self.sink = QAudioSink(output, fmt)
            self.sink.setBufferSize(rate * channels * 2 // 5)  # 0.2 s ahead
            self.sink.setVolume(volume(name))
            self.current = name
            self.sink.start(self.device)
            if self.paused:
                self.sink.suspend()
        except Exception as error:  # noqa: BLE001 - no music is no harm
            self.log(f"No music: {error!r}")
            self.sink = self.device = None
            self.current = None

    def _stop_sink(self):
        if self.sink is not None:
            if self.current is not None and self.device is not None:
                self.places[self.current] = self.device.played_frame(self.sink)
            self.sink.stop()
            self.sink.deleteLater()
            self.sink = None
        if self.device is not None:
            self.device.close()
            self.device = None
        self.current = None

    def set_paused(self, paused):
        """Pauses or resumes the music where it is."""
        self.paused = paused
        if self.sink is None:
            return
        if paused:
            self.sink.suspend()
        else:
            self.sink.resume()

    def fade_out(self, frames, then=None):
        """Lowers the music to silence over frames (60 a second), stops it and forgets its place
        (the game's stopStageBGM), then calls then."""
        sink, name = self.sink, self.current
        # No music playing (none started, or a program without Qt Multimedia, where _qt()
        # fails): nothing to fade. Asked for Qt first, the title screen's Space bar stopped here.
        if sink is None:
            if then:
                then()
            return
        _QIODevice, _QObject, QTimer, *_rest = _qt()
        # Timed by the clock, not by the timer's ticks: while the window draws a busy frame
        # (the title's flash) ticks are lost, and a fade counted in ticks was still loud when
        # the next piece cut it off.
        length = max(1, int(frames)) / 60.0
        start = time.monotonic()

        def step():
            if self.sink is not sink:
                timer.stop()
                return
            left = max(0.0, 1.0 - (time.monotonic() - start) / length)
            sink.setVolume(volume(name) * left)
            if left <= 0:
                timer.stop()
                self._stop_sink()
                self.places.pop(name, None)
                if then:
                    then()
        timer = QTimer()
        timer.setInterval(16)
        timer.timeout.connect(step)
        timer.start()
        self._fade_timer = timer  # kept alive while it runs

    def stop(self):
        self._stop_sink()


def _loop_device(QIODevice):
    class LoopDevice(QIODevice):
        """The piece's samples, read by the audio output: from the start to the end, then from
        the loop point on, round and round. frame is the next frame handed out."""

        def __init__(self, pcm, frame_bytes, loop_start, loop_end, frame=0):
            super().__init__()
            self.pcm, self.frame_bytes = pcm, frame_bytes
            self.loop_start, self.loop_end = loop_start, loop_end
            self.frame = frame if frame < loop_end else loop_start
            self.looped = False

        def readData(self, maxlen):
            frames = maxlen // self.frame_bytes
            out = bytearray()
            while frames > 0:
                take = min(frames, self.loop_end - self.frame)
                start = self.frame * self.frame_bytes
                out += self.pcm[start:start + take * self.frame_bytes]
                self.frame += take
                frames -= take
                if self.frame >= self.loop_end:
                    self.frame = self.loop_start
                    self.looped = True
            return bytes(out)

        def writeData(self, data):
            return -1

        def bytesAvailable(self):
            return 1 << 30

        def isSequential(self):
            return True

        def played_frame(self, sink):
            """The frame the listener has reached: what was handed out, less what the output
            still holds in its buffer (back across the loop point when it has looped)."""
            held = max(0, sink.bufferSize() - sink.bytesFree()) // self.frame_bytes
            frame = self.frame - held
            if frame < self.loop_start and self.looped:
                frame = self.loop_end - (self.loop_start - frame)
            return max(0, frame)

    return LoopDevice
