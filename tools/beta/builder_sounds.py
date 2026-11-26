"""The menu sound effects of the builder window, played from the user's own disc.

The window's buttons sound like the game's (builder_layout.ButtonController picks the same
sounds as the game's ButtonPaneController users). None of those sounds is in the builder: like the
layouts, they are made on the user's computer from the disc, in AudioRes. A sound effect there is
a JAudio2 sequence (bytecode in SMR.szs) that plays notes on instruments (banks of wave samples
in AudioRes/Waves/*.aw), so render() runs a small copy of the game's sound driver over it and
writes the result as a WAV file:

  sequence   JASSeqParser and JASSeqCtrl: notes, waits, child tracks, registers, parameters
  track      JASTrack: gate notes (tied and swept notes), timed parameters, the tempo
  channel    JASChannel and JASBank::noteOn: instrument and velocity regions, envelopes
             (JASOscillator), key sweeps, the volume and pitch of every DSP update
  wave       JASWSParser (wave info) and AFC ADPCM (the 3DS mixer, platform/3ds/rvl/dsp.cpp)

Like the game, the driver runs in sub-frames of 80 samples at 32 kHz: the tracks tick, then
every channel updates its volume and pitch, then the sub-frame is mixed. Filters, reverb sends,
panning, vibrato and tremolo are left out: the window plays the sounds in mono.

Every name below is the decomp's (src/JSystem/JAudio2), so each step can be checked against it.
"""

import struct
import sys
import wave
from pathlib import Path

RATE = 32000.0  # JASDriver::sDacRate (32 kHz mode)
# The game mixes its sound effects under music and voices; played alone at full scale they were
# much louder than the game's menus. The written files are scaled by this.
GAIN = 0.3
SUBFRAME = 80  # samples per sub-frame (JASDriver::getSubFrames() * 0x50 / subframes)

# The sounds the window uses: the file name it plays and the game's sound name.
SOUNDS = {
    "cursor": "SE_SY_TALK_FOCUS_ITEM",  # YesNoController: the cursor moves onto a yes/no button
    "yes": "SE_SY_TALK_SELECT_YES",  # YesNoController: yes is pressed
    "no": "SE_SY_TALK_SELECT_NO",  # YesNoController: no is pressed
    "button_on": "SE_SY_BUTTON_CURSOR_ON",  # BackButton and the map buttons: the cursor is on
    "cancel": "SE_SY_GALAXY_DECIDE_CANCEL",  # BackButton: pressed
    "galaxy_ok": "SE_SY_GALAXY_DECIDE_OK",  # GalaxyConfirmLayout: the Start button is pressed
    "pause_item": "SE_SY_SELECT_PAUSE_ITEM",  # PauseMenu: the cursor moves onto an item
    "pause_off": "SE_SY_PAUSE_OFF",  # PauseMenu: an item is chosen
    "ok": "SE_SY_TALK_OK",  # SysInfoWindow: the A button closes a message
    "mii_scroll": "SE_SY_FILE_SEL_MIISEL_SCRL",  # MiiSelect: an arrow turns the page
    "star_get": "SE_SY_STAR_GET",  # PowerStar: a Power Star is collected (the build is done)
    "failure": "SE_SY_FILE_SEL_NG",  # FileSelector: what was chosen cannot be done (the build stopped)
    "game_start": "SE_SY_GAME_START",  # TitleSequenceProduct: the title screen is left
    "notice": "SE_SY_CTRL_GUIDE_APPEAR",  # PlayerActionGuidance: a control guide comes in
    "letter": "SE_SY_FILE_SEL_UPPER_DECIDE",  # PauseMenu: Luigi's letter button is pressed
}


def _paths():
    root = Path(__file__).resolve().parents[2]
    for folder in (root / "tools" / "3ds" / "builder",):
        if str(folder) not in sys.path:
            sys.path.insert(0, str(folder))


_paths()


def _u8(d, p):
    return d[p]


def _u16(d, p):
    return struct.unpack_from(">H", d, p)[0]


def _s16(d, p):
    return struct.unpack_from(">h", d, p)[0]


def _u24(d, p):
    return int.from_bytes(d[p:p + 3], "big")


def _u32(d, p):
    return struct.unpack_from(">I", d, p)[0]


def _f32(d, p):
    return struct.unpack_from(">f", d, p)[0]


# ##############################################################################################
# # The audio archive                                                                          #
# #                                                                                            #
# # SMR.szs (tools/3ds/builder/baa.py has every layout): the sound table (bst), its names      #
# # (bstn), the sound effect sequences (bsc), the wave systems (ws) and the banks (bnk).       #
# ##############################################################################################
class Archive:
    def __init__(self, audio_res):
        import baa
        import yaz0
        self.folder = Path(audio_res)
        data = (self.folder / "SMR.szs").read_bytes()
        if yaz0.is_compressed(data):
            data = yaz0.decompress(data)
        self.data = data
        commands, _ = baa.read_commands(data)
        self.wave_systems = {}
        self.banks = {}  # bank number -> Bank
        bank_offsets = []
        for code, params, _raw in commands:
            if code == "bst ":
                self.bst = params[0]
            elif code == "bstn":
                self.bstn = params[0]
            elif code == "bsc ":
                self.bsc = params[0]
            elif code == "ws  ":
                self.wave_systems[params[0]] = params[1]
            elif code == "bnk ":
                bank_offsets.append((params[0], params[1]))
        self._waves = {}
        self._aw = {}
        for ws_id, offset in bank_offsets:
            bank = Bank(data, offset, ws_id)
            self.banks[bank.number] = bank

    def names(self):
        """{sound name: (section, group, index)} from the name table (JAUSoundNameTable)."""
        d, start = self.data, self.bstn
        root = start + _u32(d, start + 12)
        out = {}
        for s in range(_u32(d, root)):
            section = start + _u32(d, root + 4 + 4 * s)
            for g in range(_u32(d, section)):
                group = start + _u32(d, section + 8 + 4 * g)
                for i in range(_u32(d, group)):
                    name = start + _u32(d, group + 8 + 4 * i)
                    out[d[name:d.index(b"\0", name)].decode("ascii", "replace")] = (s, g, i)
        return out

    def volume(self, section, group, index):
        """The sound table's volume of a sound (JAUStdSoundInfo::getSoundInfo_: u8 / 255)."""
        d, start = self.data, self.bst
        root = start + _u32(d, start + 12)
        sec = start + _u32(d, root + 4 + 4 * section)
        grp = start + _u32(d, sec + 4 + 4 * group)
        word = _u32(d, grp + 8 + 4 * index)
        return d[start + (word & 0xFFFFFF) + 1] / 255.0

    def sequence(self, group, index):
        """(base, start) of a sound effect's bytecode (JAUSeqCollection::getSeqData)."""
        d, start = self.data, self.bsc
        table = start + _u32(d, start + 8 + 4 * group)
        return start, start + _u32(d, table + 4 + 4 * index)

    def wave(self, bank, wave_id):
        """(info, samples) of a wave of a bank's wave system, decoded once."""
        key = (bank.ws, wave_id)
        if key not in self._waves:
            info = self._wave_info(bank.ws, wave_id)
            if info is None:
                self._waves[key] = None
            else:
                aw = self._aw.get(info["file"])
                if aw is None:
                    aw = self._aw[info["file"]] = (self.folder / "Waves" / info["file"]).read_bytes()
                self._waves[key] = (info, decode_wave(aw, info))
        return self._waves[key]

    def _wave_info(self, ws_id, wave_id):
        """JASWSParser::createBasicWaveBank: wave id (low half of the control wave) -> TWave."""
        d = self.data
        start = self.wave_systems[ws_id]
        archive_bank = start + _u32(d, start + 0x10)
        ctrl_group = start + _u32(d, start + 0x14)
        for g in range(_u32(d, ctrl_group + 8)):
            scene = start + _u32(d, ctrl_group + 0xC + 4 * g)
            ctrl = start + _u32(d, scene + 0xC)
            archive = start + _u32(d, archive_bank + 8 + 4 * g)
            for j in range(_u32(d, ctrl + 4)):
                ctrl_wave = start + _u32(d, ctrl + 8 + 4 * j)
                if _u32(d, ctrl_wave) & 0xFFFF != wave_id:
                    continue
                w = start + _u32(d, archive + 0x74 + 4 * j)
                return {
                    "file": d[archive:archive + 0x70].split(b"\0")[0].decode("ascii"),
                    "format": d[w + 1], "base_key": d[w + 2], "rate": _f32(d, w + 4),
                    "offset": _u32(d, w + 8), "length": _u32(d, w + 0xC),
                    "loop": _u32(d, w + 0x10) != 0, "loop_start": _u32(d, w + 0x14),
                    "loop_end": _u32(d, w + 0x18), "count": _u32(d, w + 0x1C),
                    "last": _s16(d, w + 0x20), "penult": _s16(d, w + 0x22),
                }
        return None


# ##############################################################################################
# # Instrument banks                                                                           #
# #                                                                                            #
# # IBNK version 1 (JASBNKParser::Ver1::createBasicBank): oscillators (OSCT) whose points are  #
# # in ENVT, and in LIST, by program number, instruments ('Inst': oscillators, effects, key    #
# # regions of velocity regions) and drum sets ('Perc': one entry per key).                    #
# ##############################################################################################
class Bank:
    def __init__(self, d, start, ws):
        self.ws = ws
        self.number = _u32(d, start + 8)
        size = _u32(d, start + 4)
        chunks = {}
        at = start + 0x20
        while at < start + size:
            chunks[d[at:at + 4]] = (at, _u32(d, at + 4))
            at = (at + 0xB + _u32(d, at + 4)) & ~3
        envt = chunks[b"ENVT"][0] + 8
        osct = chunks[b"OSCT"][0] + 8
        oscillators = []
        for i in range(_u32(d, osct)):
            o = osct + 4 + i * 0x1C
            oscillators.append({
                "target": d[o + 4], "rate": _f32(d, o + 8),
                "table": _points(d, envt + _u32(d, o + 0xC)), "release": _points(d, envt + _u32(d, o + 0x10)),
                "scale": _f32(d, o + 0x14), "offset": _f32(d, o + 0x18),
            })
        self.programs = {}
        lst = chunks[b"LIST"][0] + 8
        for i in range(_u32(d, lst)):
            offset = _u32(d, lst + 4 + 4 * i)
            if not offset:
                continue
            p = start + offset
            kind = d[p:p + 4]
            if kind == b"Inst":
                self.programs[i] = _instrument(d, start, p + 4, oscillators)
            elif kind == b"Perc":
                self.programs[i] = _drums(d, start, p + 4)


def _points(d, p):
    """An oscillator table: (mode, time, value) points up to the end marks (0xF, 0xE)."""
    out = []
    for _ in range(64):
        point = struct.unpack_from(">3h", d, p)
        out.append(point)
        p += 6
        if point[0] in (0xE, 0xF):
            break
    return out


def _effects(d, start, p):
    count = _u32(d, p)
    out = []
    for k in range(count):
        e = start + _u32(d, p + 4 + 4 * k)
        if d[e:e + 4] == b"Sens":
            # JASInstSense: type (0 volume, 1 pitch...), what it follows (velocity or key),
            # the key of the 1.0 level, the levels at 0 and 127.
            out.append(("sense", d[e + 4], d[e + 5], d[e + 6], _f32(d, e + 8), _f32(d, e + 12)))
        elif d[e:e + 4] == b"Rand":
            # JASInstRand: type, then base and range; played at its base value.
            out.append(("rand", d[e + 4], _f32(d, e + 8), _f32(d, e + 12)))
    return out, p + 4 + 4 * count


def _velocity_regions(d, p, count):
    regions = [(d[p + 16 * k], _u32(d, p + 16 * k + 4) & 0xFFFF, _f32(d, p + 16 * k + 8), _f32(d, p + 16 * k + 12))
               for k in range(count)]
    return regions, p + 16 * count


def _instrument(d, start, p, oscillators):
    count = _u32(d, p)
    oscs = [oscillators[_u32(d, p + 4 + 4 * k)] for k in range(count)]
    effects, p = _effects(d, start, p + 4 + 4 * count)
    keys = []
    for _ in range(_u32(d, p)):
        high = d[p + 4]
        regions, next_p = _velocity_regions(d, p + 12, _u32(d, p + 8))
        keys.append((high, regions))
        p = next_p - 4
    p += 4
    return {"kind": "inst", "oscillators": oscs, "effects": effects, "keys": keys,
            "volume": _f32(d, p), "pitch": _f32(d, p + 4)}


def _drums(d, start, p):
    percs = {}
    for key in range(_u32(d, p)):
        offset = _u32(d, p + 4 + 4 * key)
        if not offset:
            continue
        q = start + offset + 4
        volume, pitch, pan, release = _f32(d, q), _f32(d, q + 4), d[q + 8], _u16(d, q + 0xA)
        effects, q = _effects(d, start, start + offset + 0x10)
        regions, _ = _velocity_regions(d, q + 4, _u32(d, q))
        percs[key] = {"volume": volume, "pitch": pitch, "pan": pan / 127.0, "release": release,
                      "effects": effects, "regions": regions}
    return {"kind": "perc", "percs": percs}


def _apply_effects(effects, key, velocity, params):
    for effect in effects:
        if effect[0] == "rand":
            _, kind, base, _range = effect
            value = base
        else:
            _, kind, follows, max_point, start_level, end_level = effect
            v = velocity if follows == 1 else key if follows == 2 else 0
            if max_point in (0, 0x7F):
                value = start_level + v * (end_level - start_level) / 0x7F
            elif v < max_point:
                value = start_level + (1.0 - start_level) * (v / max_point)
            else:
                value = (end_level - 1.0) * ((v - max_point) / (0x7F - max_point)) + 1.0
        if kind == 0:
            params["volume"] *= value
        elif kind == 1:
            params["pitch"] *= value


def instrument_params(program, key, velocity):
    """JASBasicInst::getParam and JASDrumSet::getParam: volume, pitch, wave, oscillators."""
    if program is None:
        return None
    if program["kind"] == "inst":
        params = {"volume": program["volume"], "pitch": program["pitch"], "oscillators": program["oscillators"],
                  "fixed_key": False, "release": 0}
        _apply_effects(program["effects"], key, velocity, params)
        for high, regions in program["keys"]:
            if key <= high:
                for limit, wave_id, volume, pitch in regions:
                    if velocity <= limit:
                        params["volume"] *= volume
                        params["pitch"] *= pitch
                        params["wave"] = wave_id
                        return params
                return None
        return None
    perc = program["percs"].get(key)
    if perc is None:
        return None
    # A drum's single oscillator has no tables: release is the drum's own (direct release).
    params = {"volume": perc["volume"], "pitch": perc["pitch"], "fixed_key": True, "release": perc["release"],
              "oscillators": [{"target": 0, "rate": 1.0, "table": None, "release": None, "scale": 1.0, "offset": 0.0}]}
    _apply_effects(perc["effects"], key, velocity, params)
    for limit, wave_id, volume, pitch in perc["regions"]:
        if velocity <= limit:
            params["volume"] *= volume
            params["pitch"] *= pitch
            params["wave"] = wave_id
            return params
    return None


# ##############################################################################################
# # Waves                                                                                      #
# #                                                                                            #
# # Formats (JASDsp::TChannel::setWaveInfo): 0 AFC ADPCM in 9-byte blocks of 16 samples,       #
# # 1 AFC in 5-byte blocks, 2 PCM8, 3 PCM16. AFC: a header byte (scale exponent, filter) and   #
# # 4- or 2-bit deltas, predicted from the last two samples (JASDsp::DSPADPCM_FILTER).          #
# ##############################################################################################
AFC_COEFS = ((0, 0), (2048, 0), (0, 2048), (1024, 1024), (4096, -2048), (3584, -1536), (3072, -1024), (4608, -2560),
             (4200, -2248), (4800, -2300), (5120, -3072), (2048, -2048), (1024, -1024), (-1024, 1024), (-1024, 0),
             (-2048, 0))


def decode_wave(aw, info):
    data = aw[info["offset"]:info["offset"] + info["length"]]
    count = info["count"]
    fmt = info["format"]
    out = []
    if fmt in (0, 1):
        block = 9 if fmt == 0 else 5
        h1 = h2 = 0
        for b in range(0, len(data) - block + 1, block):
            header = data[b]
            scale = 1 << (header >> 4)
            c0, c1 = AFC_COEFS[header & 15]
            for i in range(16):
                if fmt == 0:
                    byte = data[b + 1 + i // 2]
                    delta = (byte >> 4) if i % 2 == 0 else byte & 15
                    delta = delta - 16 if delta >= 8 else delta
                    delta = (delta * scale) << 11
                else:
                    byte = data[b + 1 + i // 4]
                    delta = (byte >> (6 - 2 * (i % 4))) & 3
                    delta = delta - 4 if delta >= 2 else delta
                    delta = (delta * scale) << 13
                sample = (delta + c0 * h1 + c1 * h2) >> 11
                sample = max(-32768, min(32767, sample))
                h2, h1 = h1, sample
                out.append(sample)
                if len(out) >= count:
                    return out
    elif fmt == 2:
        out = [(v - 256 if v >= 128 else v) << 8 for v in data[:count]]
    elif fmt == 3:
        out = list(struct.unpack_from(">%dh" % min(count, len(data) // 2), data, 0))
    return out


# ##############################################################################################
# # The envelope (JASOscillator)                                                               #
# ##############################################################################################
CURVES = (
    (1.0, 0.9375, 0.875, 0.8125, 0.75, 0.6875, 0.625, 0.5625, 0.5, 0.4375, 0.375, 0.3125, 0.25, 0.1875, 0.125, 0.0625, 0.0),
    (1.0, 0.968246, 0.935414, 0.901388, 0.866025, 0.829156, 0.790569, 0.75, 0.707107, 0.661438, 0.612372, 0.559017, 0.5,
     0.433013, 0.353553, 0.25, 0.0),
    (1.0, 0.878906, 0.765625, 0.660156, 0.5625, 0.472656, 0.390625, 0.316406, 0.25, 0.191406, 0.140625, 0.097656,
     0.0625, 0.035156, 0.015625, 0.003906, 0.0),
    (1.0, 0.970489, 0.781274, 0.546281, 0.399792, 0.289315, 0.212104, 0.157476, 0.112613, 0.08179, 0.057985, 0.043642,
     0.030824, 0.023713, 0.015259, 0.009156, 0.0),
)
STOP, RUN, SUSTAIN, RELEASE, DIRECT = 0, 1, 2, 3, 4


class Oscillator:
    def __init__(self, data, direct_release=0):
        self.data = data
        self.direct = direct_release
        self.time = self.value = self.start = 0.0
        self.index = 0
        self.target = 0.0
        self.curve = 0
        if data["table"] is None:
            self.state = SUSTAIN
            self.value = 1.0
            return
        first = data["table"][0]
        self.target = first[2] / 32768.0
        self.curve = first[0]
        self.state = RUN

    def get(self):
        return self.value * self.data["scale"] + self.data["offset"]

    def advance(self, amount):
        if self.state in (STOP, SUSTAIN):
            return
        self.time += amount * self.data["rate"]
        self.update()

    def release(self):
        d = self.data
        if d["release"] is None and not self.direct:
            self.state = STOP
            return
        if self.direct:
            self.time, self.start, self.target, self.index = 0.0, self.value, 0.0, 0
            self.curve = (self.direct >> 14) & 3
            self.state = DIRECT
            self.update()
            return
        if d["table"] is not d["release"]:
            self.time, self.start, self.index = 0.0, self.value, 0
            self.target = d["release"][0][2] / 32768.0
            self.curve = d["release"][0][0]
        self.state = RELEASE
        self.update()

    def update(self):
        if self.state == DIRECT:
            length = self.direct & 0x3FFF
            if self.time < length:
                self.current(length)
            else:
                self.value = self.target
                self.state = STOP
            return
        if self.state == SUSTAIN:
            return
        table = self.data["table"] if self.state == RUN else self.data["release"]
        if table is None:
            self.state = SUSTAIN
            return
        while self.index < len(table) and self.time >= table[self.index][1]:
            self.time -= table[self.index][1]
            self.value = self.target
            self.index += 1
            self.start = self.value
            if self.index >= len(table):
                self.state = STOP
                return
            mode, _time, value = table[self.index]
            if mode == 0xF:
                self.state = STOP
                return
            if mode == 0xE:
                self.state = SUSTAIN
                return
            if mode == 0xD:
                self.index = value
            else:
                self.curve = mode
                self.target = value / 32768.0
        if self.index < len(table):
            self.current(table[self.index][1])

    def current(self, length):
        table = CURVES[self.curve & 3]
        f = 16.0 * (self.time / length) if length else 16.0
        i = min(15, int(f))
        frac = f - i
        k = (1.0 - frac) * table[i] + frac * table[i + 1]
        self.value = self.start * k + self.target * (1.0 - k)


# ##############################################################################################
# # Channels (JASChannel)                                                                      #
# ##############################################################################################
class Channel:
    def __init__(self, mgr, info, samples, params, key, velocity, update_timer, direct_release):
        self.mgr = mgr
        self.info, self.samples = info, samples
        self.init_volume = params["volume"]
        self.init_pitch = params["pitch"] * (info["rate"] / RATE)
        self.key = 0 if params["fixed_key"] else key - info["base_key"]
        self.velocity = velocity
        self.sweep = self.sweep_target = 0.0
        self.sweep_count = 0
        self.timer = update_timer
        release = params["release"] or direct_release
        self.oscillators = [Oscillator(o, release if i == 0 else 0) for i, o in enumerate(params["oscillators"][:2])]
        for osc in self.oscillators:
            osc.update()
        self.position = 0.0
        self.alive = True
        self.released = False

    def release(self):
        if not self.released:
            self.released = True
            for osc in self.oscillators:
                osc.release()

    def set_sweep(self, target, count):
        if count == 0:
            self.sweep = float(target)
        else:
            self.sweep, self.sweep_target = 0.0, float(target)
        self.sweep_count = count

    def update(self, first):
        """updateDSPChannel: (volume, pitch) for the next sub-frame, or None when it stops."""
        if not first:
            if self.timer:
                self.timer -= 1
                if self.timer == 0:
                    self.release()  # JASTrack::channelUpdateCallback, CB_TIMER
                    self.mgr.forget(self)
            for i, osc in enumerate(self.oscillators):
                osc.advance(48000.0 / RATE)
                if i == 0 and osc.state == STOP:
                    self.alive = False
                    return None
        volume_fx, pitch_fx = 1.0, 1.0
        for osc in self.oscillators:
            if osc.data["target"] == 0:
                volume_fx *= osc.get()
            elif osc.data["target"] == 1:
                pitch_fx *= osc.get()
        p = self.mgr.params
        volume = (self.velocity / 127.0) ** 2 * self.init_volume * volume_fx * p["volume"]
        pitch = 2.0 ** (p["bend"] + (self.key + self.sweep) / 12.0) * self.init_pitch * pitch_fx * p["pitch"]
        if self.sweep_count:
            self.sweep += (self.sweep_target - self.sweep) / self.sweep_count
            self.sweep_count -= 1
        return max(0.0, volume), max(0.0, pitch)

    def mix(self, out, start, volume, pitch):
        samples, info = self.samples, self.info
        end = info["loop_end"] if info["loop"] else len(samples)
        end = min(end, len(samples))
        pos = self.position
        for n in range(start, start + SUBFRAME):
            i = int(pos)
            if i >= end - 1:
                if not info["loop"]:
                    self.alive = False
                    break
                pos -= end - info["loop_start"]
                i = int(pos)
            frac = pos - i
            s = samples[i] * (1.0 - frac) + samples[min(i + 1, len(samples) - 1)] * frac
            out[n] += s * volume
            pos += pitch
        self.position = pos


# ##############################################################################################
# # Tracks (JASTrack, JASSeqCtrl, JASSeqParser)                                                #
# ##############################################################################################
# (argument count, argument widths: 2 bits each, 0 u8, 1 u16, 2 u24, 3 register) of commands
# 0xA0..0xFF (JASSeqParser::sCmdInfo).
COMMANDS = {
    0xB1: ("noteon", 3, 0), 0xB2: ("noteoff", 1, 0), 0xB3: ("note", 4, 0x40), 0xB4: ("lastnote", 1, 0),
    0xB8: ("parame", 2, 0), 0xB9: ("parami", 2, 4), 0xBA: ("paramei", 3, 0x10), 0xBB: ("paramii", 3, 0x14),
    0xC1: ("opentrack", 2, 8), 0xC2: ("closetrack", 1, 0), 0xC3: ("call", 1, 2), 0xC4: ("callf", 2, 8),
    0xC5: ("ret", 0, 0), 0xC6: ("retf", 1, 0), 0xC7: ("jmp", 1, 2), 0xC8: ("jmpf", 2, 8),
    0xC9: ("jmptable", 2, 0xB), 0xCA: ("calltable", 2, 0xB), 0xCB: ("loops", 1, 1), 0xCC: ("loope", 0, 0),
    0xD0: ("readport", 2, 0), 0xD1: ("writeport", 2, 0xC), 0xD2: ("checkin", 1, 0), 0xD3: ("checkout", 1, 0),
    0xD4: ("parentwrite", 2, 0xC), 0xD5: ("childwrite", 2, 0xC), 0xD6: ("parentread", 2, 0),
    0xD7: ("childread", 2, 0), 0xD8: ("regload", 2, 4), 0xD9: ("reg", 3, 0x30), 0xDA: ("reg", 3, 0x10),
    0xDB: ("reguni", 2, 0), 0xDC: ("regtbl", 4, 0xE0), 0xE0: ("tempo", 1, 1), 0xE1: ("bankprg", 1, 1),
    0xE2: ("bank", 1, 0), 0xE3: ("prg", 1, 0), 0xE7: ("envscale", 2, 4), 0xE8: ("envset", 2, 8),
    0xE9: ("adsr", 5, 0x155), 0xEA: ("bus", 2, 4), 0xEB: ("iircut", 1, 0), 0xEC: ("iirset", 4, 0x55),
    0xED: ("firset", 1, 2), 0xF0: ("wait", 0, 0), 0xF1: ("waitbyte", 1, 0), 0xF3: ("setinttable", 1, 2),
    0xF4: ("setint", 1, 1), 0xF5: ("disint", 1, 1), 0xF6: ("reti", 0, 0), 0xF7: ("clri", 0, 0),
    0xF8: ("inttimer", 2, 4), 0xF9: ("synccpu", 1, 1), 0xFD: ("printf", 0, 0), 0xFE: ("nop", 0, 0),
    0xFF: ("finish", 0, 0),
}
# The other commands with their argument counts and widths, read and ignored.
for _code in range(0xA0, 0x100):
    COMMANDS.setdefault(_code, ("none", 0, 0))


class ChannelMgr:
    def __init__(self, track):
        self.track = track
        self.channels = [None] * 8
        self.params = {"volume": 1.0, "pitch": 1.0, "bend": 0.0}

    def forget(self, channel):
        for i, c in enumerate(self.channels):
            if c is channel:
                self.channels[i] = None

    def note_off(self, index):
        channel = self.channels[index]
        if channel is None:
            return False
        channel.release()
        self.channels[index] = None
        return True


class Track:
    def __init__(self, player, data, base, start, parent=None):
        self.player, self.data, self.base = player, data, base
        self.cursor = start
        self.parent = parent
        self.children = [None] * 16
        self.running = True
        self.timer = 0
        self.wait_note = False
        self.stack = []
        self.loops = []
        self.regs = [0] * 0x40
        self.ports = [0] * 16
        self.mgr = ChannelMgr(self)
        self.playtime = 1.0
        self.ready_to_play = False
        self.last_pitch = 60
        if parent is None:
            self.tempo, self.timebase, self.transpose = 120, 48, 0
            self.bank, self.program, self.bend_sense, self.gate_rate = 0, 0xF0, 12, 100
            self.direct_release = 0
        else:
            # JASTrack::inherit
            self.tempo, self.timebase, self.transpose = parent.tempo, parent.timebase, 0
            self.bank, self.program, self.bend_sense, self.gate_rate = parent.bank, parent.program, parent.bend_sense, parent.gate_rate
            self.direct_release = parent.direct_release
        self.move = [[1.0, 1.0, 0], [0.0, 0.0, 0], [0.0, 0.0, 0], [0.5, 0.5, 0], [0.0, 0.0, 0], [0.0, 0.0, 0]]

    # Timing.
    def interval(self):
        root = self
        while root.parent:
            root = root.parent
        return 1.33333337307 * (root.timebase * root.tempo) / RATE

    def dsp_time(self, ticks):
        return int(ticks / self.interval())

    def transpose_total(self):
        return self.transpose + (self.parent.transpose_total() if self.parent else 0)

    # Reading the bytecode.
    def byte(self):
        v = self.data[self.cursor]
        self.cursor += 1
        return v

    def read16(self):
        v = _u16(self.data, self.cursor)
        self.cursor += 2
        return v

    def read24(self):
        v = _u24(self.data, self.cursor)
        self.cursor += 3
        return v

    def midi(self):
        v = 0
        while True:
            b = self.byte()
            v = (v << 7) | (b & 0x7F)
            if not b & 0x80:
                return v

    def read_reg(self, reg):
        if reg < 0x40:
            return self.regs[reg] & 0xFFFF
        reg -= 0x40
        if reg < 0x10:
            return self.ports[reg]
        if reg == 0x20:
            return sum(1 << i for i, c in enumerate(self.children) if c is not None and c.running)
        return {0x22: self.timebase, 0x23: self.transpose, 0x24: self.bend_sense, 0x25: self.gate_rate,
                0x27: self.bank, 0x28: self.program, 0x2D: self.direct_release}.get(reg, 0)

    def write_reg(self, reg, value):
        value &= 0xFFFF
        self.regs[3] = value  # REG_F
        if reg < 0x40:
            self.regs[reg] = value
            return
        reg -= 0x40
        if reg < 0x10:
            self.ports[reg] = value
        elif reg == 0x22:
            self.timebase = value
        elif reg == 0x23:
            self.transpose = value - 0x10000 if value >= 0x8000 else value
        elif reg == 0x24:
            self.bend_sense = value
        elif reg == 0x25:
            self.gate_rate = value
        elif reg == 0x27:
            self.bank = value & 0xFF
        elif reg == 0x28:
            self.program = value & 0xFF
        elif reg == 0x2D:
            self.direct_release = value

    def condition(self, cond):
        f = self.regs[3] & 0xFFFF
        return {0: True, 1: f == 0, 2: f != 0, 3: f == 1, 4: f >= 0x8000, 5: f < 0x8000}.get(cond, False)

    # One tick (JASTrack::tickProc and JASSeqCtrl::tickProc).
    def tick(self):
        if self.running:
            if self.wait_note:
                if self.mgr.channels[0] is not None:
                    self._children_tick()
                    self._timed()
                    return
                self.wait_note = False
            if self.timer > 0:
                self.timer -= 1
            while self.running and self.timer == 0 and not self.wait_note:
                if self.parse() < 0:
                    self.close()
                    return
            self._timed()
        self._children_tick()

    def _timed(self):
        for m in self.move:
            if m[2]:
                m[0] += (m[1] - m[0]) / m[2]
                m[2] -= 1

    def _children_tick(self):
        for i, child in enumerate(self.children):
            if child is not None and child.running:
                child.tick()

    def close(self):
        self.running = False
        for child in self.children:
            if child is not None:
                child.close()
        for i in range(8):
            self.mgr.note_off(i)

    def update_params(self):
        """JASTrack::updateTrack: the channel parameters, the parent's times this track's."""
        volume = self.move[0][0] ** 2
        bend = self.move[1][0] * self.bend_sense * 0.333333343267
        if self.parent:
            pp = self.parent.mgr.params
            self.mgr.params = {"volume": pp["volume"] * volume, "pitch": pp["pitch"], "bend": pp["bend"] + bend}
        else:
            self.mgr.params = {"volume": volume * self.player.volume, "pitch": 1.0, "bend": bend}
        for child in self.children:
            if child is not None and child.running:
                child.update_params()

    # Notes.
    def channel_start(self, key, velocity, update_timer):
        bank = self.player.archive.banks.get(self.bank)
        if bank is None:
            return None
        params = instrument_params(bank.programs.get(self.program), key, velocity)
        if params is None or "wave" not in params:
            return None
        found = self.player.archive.wave(bank, params["wave"])
        if not found:
            return None
        info, samples = found
        if not samples:
            return None
        channel = Channel(self.mgr, info, samples, params, key, velocity, update_timer, self.direct_release)
        self.player.channels.append(channel)
        return channel

    def gate_on(self, key, velocity, ticks, flags):
        key += self.transpose_total()
        if self.gate_rate != 100:
            ticks *= self.gate_rate / 100.0
        dsp_time = self.dsp_time(ticks)
        sweep = flags & 1
        update_timer = 0 if flags & 6 else dsp_time
        pitch = self.last_pitch if sweep else key
        delta = key - pitch
        mgr = self.mgr
        if not self.ready_to_play:
            mgr.note_off(0)
            mgr.channels[0] = self.channel_start(pitch, velocity, update_timer)
        else:
            channel = mgr.channels[0]
            if channel is not None:
                channel.key = pitch - channel.info["base_key"]
                channel.velocity = velocity
                channel.timer = update_timer
        if sweep and mgr.channels[0] is not None:
            mgr.channels[0].set_sweep(delta, dsp_time)
        self.last_pitch = key
        self.ready_to_play = bool(flags & 2)

    def note_on_gate(self, key, velocity, ticks, ctrl):
        flags = (2 if ctrl >> 6 & 1 else 0) | (1 if ctrl >> 7 & 1 else 0) | (4 if ticks == 0 else 0)
        self.gate_on(key, velocity, ticks, flags)
        if ticks:
            self.timer = ticks
        else:
            self.wait_note = True

    def note_on_midi(self, index, key, velocity):
        key += self.transpose_total()
        self.mgr.note_off(index)
        self.mgr.channels[index] = self.channel_start(key, velocity, 0)

    # The parser (JASSeqParser::parse).
    def parse(self):
        b = self.byte()
        if not b & 0x80:
            ctrl = self.byte()
            velocity = self.byte()
            if ctrl & 7 == 0:
                self.note_on_gate(b, velocity, self.midi(), ctrl)
            else:
                self.note_on_midi(ctrl & 7, b, velocity)
            return 0
        high = b & 0xF0
        if high == 0x80:
            if b & 7:
                self.mgr.note_off(b & 7)
            return 0
        if high == 0x90:
            count = (b & 7) + 1
            data = self.byte()
            width, bit = 0, 3
            for _ in range(count):
                if data & 0x80:
                    width |= bit
                data = (data << 1) & 0xFF
                bit <<= 2
            return self.command(self.byte(), width)
        return self.command(b, 0)

    def command(self, code, extra):
        if code == 0xB0:
            self.byte()  # extended commands: only the dump command exists
            return 0
        name, count, widths = COMMANDS[code]
        widths |= extra
        args = []
        for _ in range(count):
            kind = widths & 3
            if kind == 0:
                args.append(self.byte())
            elif kind == 1:
                args.append(self.read16())
            elif kind == 2:
                args.append(self.read24())
            else:
                args.append(self.read_reg(self.byte()))
            widths >>= 2
        return self.run(name, args)

    def run(self, name, a):
        if name == "finish":
            return -1
        if name == "note":
            self.note_on_gate(a[1], a[2], a[3], a[0])
        elif name == "noteon":
            self.note_on_midi(a[1], a[0], a[2])
        elif name == "noteoff":
            self.mgr.note_off(a[0] & 7)
        elif name == "lastnote":
            if a[0] < 0x100:
                self.last_pitch = a[0] + self.transpose_total()
        elif name in ("parame", "paramei"):
            value = (a[1] << 8) & 0xFFFF
            if not a[1] & 0x80:
                value |= (a[1] << 1) & 0xFFFF
            value = value - 0x10000 if value >= 0x8000 else value
            self.set_param(a[0], value / 32767.0, a[2] if name == "paramei" else 0)
        elif name in ("parami", "paramii"):
            value = a[1] - 0x10000 if a[1] >= 0x8000 else a[1]
            self.set_param(a[0], value / 32767.0, a[2] if name == "paramii" else 0)
        elif name == "opentrack":
            child = Track(self.player, self.data, self.base, self.base + a[1], self)
            if self.children[a[0] & 15] is not None:
                self.children[a[0] & 15].close()
            self.children[a[0] & 15] = child
        elif name == "closetrack":
            child = self.children[a[0] & 15]
            if child is not None:
                child.close()
        elif name == "call" or (name == "callf" and self.condition(a[0])):
            self.stack.append(self.cursor)
            self.cursor = self.base + a[-1]
        elif name == "ret" or (name == "retf" and self.condition(a[0])):
            if self.stack:
                self.cursor = self.stack.pop()
        elif name == "jmp" or (name == "jmpf" and self.condition(a[0])):
            self.cursor = self.base + a[-1]
        elif name in ("jmptable", "calltable"):
            target = self.base + _u24(self.data, self.base + a[1] + 3 * a[0])
            if name == "calltable":
                self.stack.append(self.cursor)
            self.cursor = target
        elif name == "loops":
            self.loops.append([self.cursor, a[0]])
        elif name == "loope":
            if self.loops:
                loop = self.loops[-1]
                loop[1] -= 1
                if loop[1] > 0:
                    self.cursor = loop[0]
                else:
                    self.loops.pop()
        elif name == "readport":
            self.write_reg(a[1], self.ports[a[0] & 15])
        elif name == "writeport":
            self.ports[a[0] & 15] = a[1]
        elif name == "parentwrite":
            if self.parent:
                self.parent.ports[a[0] & 15] = a[1]
        elif name == "childwrite":
            child = self.children[a[0] >> 4 & 15]
            if child is not None:
                child.ports[a[0] & 15] = a[1]
        elif name == "parentread":
            if self.parent:
                self.write_reg(a[1], self.parent.ports[a[0] & 15])
        elif name == "childread":
            child = self.children[a[0] >> 4 & 15]
            if child is not None:
                self.write_reg(a[1], child.ports[a[0] & 15])
        elif name == "regload":
            self.write_reg(a[0], a[1])
        elif name == "reg":
            self._reg(a[0], a[1], a[2])
        elif name == "reguni":
            if a[0] == 0xB:
                self.write_reg(a[1], -self.read_reg(a[1]))
        elif name == "regtbl":
            op, reg, table, index = a
            if op == 0xC:
                self.write_reg(reg, self.data[self.base + table + index])
            elif op == 0xD:
                self.write_reg(reg, _u16(self.data, self.base + table + 2 * index))
            elif op == 0xE:
                self.write_reg(reg, _u24(self.data, self.base + table + 3 * index))
            elif op in (0xF, 0x10):
                self.write_reg(reg, _u32(self.data, self.base + table + (4 * index if op == 0xF else index)))
        elif name == "tempo":
            self.tempo = a[0]
        elif name == "bankprg":
            self.bank, self.program = (a[0] >> 8) & 0xFF, a[0] & 0xFF
        elif name == "bank":
            self.bank = a[0] & 0xFF
        elif name == "prg":
            self.program = a[0] & 0xFF
        elif name == "wait":
            self.timer = self.midi()
        elif name == "waitbyte":
            self.timer = a[0]
        elif name == "synccpu":
            self.regs[3] = 0xFFFF
        return 0

    def _reg(self, op, reg, val):
        if op == 1:
            val += self.read_reg(reg)
        elif op == 2:
            val = self.read_reg(reg) - val
        elif op == 3:
            val, reg = self.read_reg(reg) - val, 3
        elif op == 4:
            val, reg = val * self.read_reg(reg), 0x21
        elif op == 5:
            val &= self.read_reg(reg)
        elif op == 6:
            val |= self.read_reg(reg)
        elif op == 7:
            val ^= self.read_reg(reg)
        elif op == 8:
            val = 0
        elif op == 9:
            val = self.read_reg(reg) << val
        elif op == 10:
            val = self.read_reg(reg) >> val
        elif op != 0:
            return
        self.write_reg(reg, val)

    def set_param(self, index, value, count):
        if index >= len(self.move):
            return
        m = self.move[index]
        m[1] = value
        if not count:
            m[0] = value
        m[2] = count


class Player:
    def __init__(self, archive, volume):
        self.archive = archive
        self.volume = volume
        self.channels = []


def render(archive, name, limit_seconds=4.0):
    """The samples (floats, 32 kHz, mono) of one sound effect."""
    s, g, i = archive.names()[name]
    base, start = archive.sequence(g, i)
    player = Player(archive, archive.volume(s, g, i))
    root = Track(player, archive.data, base, start)
    out = []
    subframes = int(limit_seconds * RATE / SUBFRAME)
    started = set()
    for _ in range(subframes):
        # JASTrack::seqMain: whole ticks, then the parameters for the channels.
        while root.playtime >= 1.0 and root.running:
            root.playtime -= 1.0
            root.tick()
        root.playtime += root.interval()
        root.update_params()
        block = [0.0] * SUBFRAME
        for channel in list(player.channels):
            first = id(channel) not in started
            started.add(id(channel))
            state = channel.update(first) if channel.alive else None
            if state is not None:
                channel.mix(block, 0, *state)
            if state is None or not channel.alive:
                player.channels.remove(channel)
                channel.mgr.forget(channel)  # JASTrack::channelUpdateCallback, CB_STOP
        out.extend(block)
        if not root.running and not player.channels:
            break
    # Trim the silence at the end.
    end = len(out)
    while end > 0 and abs(out[end - 1]) < 1.0:
        end -= 1
    return out[:end]


def write_wav(path, samples):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(RATE))
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(round(v * GAIN))))) for v in samples))


def make(audio_res, out, report=None):
    """Writes every sound of SOUNDS as <key>.wav into out; {key: file} of those written."""
    archive = Archive(audio_res)
    names = archive.names()
    written = {}
    for done, (key, name) in enumerate(SOUNDS.items()):
        if report:
            report(key, done, len(SOUNDS))
        if name not in names:
            continue
        samples = render(archive, name)
        if not samples:
            continue
        write_wav(Path(out) / f"{key}.wav", samples)
        written[key] = f"{key}.wav"
    return written


if __name__ == "__main__" and len(sys.argv) > 2:
    target = Path(sys.argv[2])
    target.mkdir(parents=True, exist_ok=True)
    for key, file in make(sys.argv[1], target, lambda k, d, t: print(k, flush=True)).items():
        print("wrote", file)
