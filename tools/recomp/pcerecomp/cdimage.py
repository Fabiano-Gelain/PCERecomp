"""CUE/BIN CD image reading for the recompiler: only what is needed to read bytes of the data
tracks by disc position (sector * 2048 + byte), which is how coverage v2 identifies the code
loaded from the CD (runtime/pce/coverage.h).

Track positions follow the same rule as the runtime (runtime/cd/cdimage.c, same as Geargrafx);
the recomp_python test checks them against cd_info.
"""
import os
import re
import zlib

RAW = 2352
DATA = 2048


def _msf(text):
    m, s, f = (int(x) for x in text.split(":"))
    return (m * 60 + s) * 75 + f


class Track:
    def __init__(self, number, kind, file_index):
        self.number = number
        self.kind = kind  # "AUDIO", "MODE1/2048", "MODE1/2352"
        self.file_index = file_index
        self.index0 = None
        self.index1 = 0
        self.pregap = None
        self.sector_size = DATA if kind == "MODE1/2048" else RAW
        self.start_lba = self.end_lba = 0
        self.lead_in_lba = None
        self.file_offset = 0

    @property
    def is_data(self):
        return self.kind != "AUDIO"


class CdImage:
    def __init__(self, cue_path):
        self.path = cue_path
        self.files = []  # paths of the .bin files
        self.tracks = []
        directory = os.path.dirname(cue_path)
        with open(cue_path, encoding="latin-1") as f:
            for line in f:
                words = line.strip()
                low = words.lower()
                if low.startswith("file"):
                    m = re.search(r'"(.*)"', words)
                    name = m.group(1) if m else words.split()[1]
                    self.files.append(name if os.path.isabs(name) else os.path.join(directory, name))
                elif low.startswith("track"):
                    parts = words.split()
                    self.tracks.append(Track(int(parts[1]), parts[2].upper(), len(self.files) - 1))
                elif low.startswith("pregap"):
                    self.tracks[-1].pregap = _msf(words.split()[1])
                elif low.startswith("index"):
                    parts = words.split()
                    if int(parts[1]) == 0:
                        self.tracks[-1].index0 = _msf(parts[2])
                    elif int(parts[1]) == 1:
                        self.tracks[-1].index1 = _msf(parts[2])
        self._layout()
        self._cache = {}

    def _layout(self):
        sizes = [os.path.getsize(p) for p in self.files]
        i = 0
        while i < len(self.tracks):
            file_index = self.tracks[i].file_index
            start_sector = self.tracks[i - 1].end_lba + 1 if i else 0
            total_pregap = 0
            first = True
            while i < len(self.tracks) and self.tracks[i].file_index == file_index:
                t = self.tracks[i]
                if t.pregap is not None:
                    total_pregap += t.pregap
                t.start_lba = t.index1 + total_pregap + start_sector
                if t.pregap is not None:
                    t.lead_in_lba = t.index1 + total_pregap - t.pregap + start_sector
                elif t.index0 is not None:
                    t.lead_in_lba = t.index0 + total_pregap + start_sector
                offset = 0
                if not first:
                    prev = self.tracks[i - 1]
                    prev.end_lba = (t.lead_in_lba if t.lead_in_lba is not None else t.start_lba) - 1
                    offset = prev.file_offset + (prev.end_lba - prev.start_lba + 1) * prev.sector_size
                t.file_offset = offset
                if t.lead_in_lba is not None and t.pregap is None:
                    t.file_offset += (t.start_lba - t.lead_in_lba) * t.sector_size
                i += 1
                first = False
            last = self.tracks[i - 1]
            rest = sizes[file_index] - last.file_offset
            last.end_lba = last.start_lba + (rest + last.sector_size - 1) // last.sector_size - 1

    def track_of(self, lba):
        for t in self.tracks:
            if t.start_lba <= lba <= t.end_lba:
                return t
        return None

    def read_sector(self, lba):
        """The 2048 bytes of a data sector (ValueError if it is not a data track)."""
        if lba in self._cache:
            return self._cache[lba]
        t = self.track_of(lba)
        if t is None or not t.is_data:
            raise ValueError(f"sector {lba} is not in a data track")
        pos = t.file_offset + (lba - t.start_lba) * t.sector_size + (16 if t.sector_size == RAW else 0)
        with open(self.files[t.file_index], "rb") as f:
            f.seek(pos)
            data = f.read(DATA)
        if len(data) != DATA:
            raise ValueError(f"sector {lba} outside the file")
        if len(self._cache) > 4096:
            self._cache.clear()
        self._cache[lba] = data
        return data

    def read(self, position):
        """One byte by disc position (sector * 2048 + byte)."""
        return self.read_sector(position // DATA)[position % DATA]

    def read_bytes(self, position, length):
        """`length` consecutive bytes starting at the disc position."""
        out = bytearray()
        while len(out) < length:
            sector = self.read_sector((position + len(out)) // DATA)
            start = (position + len(out)) % DATA
            out += sector[start:start + length - len(out)]
        return bytes(out)

    def data_track_crc32(self):
        """CRC32 of the 2048 bytes of every sector of the data tracks (the same as cd_info)."""
        crc = 0
        for t in self.tracks:
            if t.is_data:
                for lba in range(t.start_lba, t.end_lba + 1):
                    crc = zlib.crc32(self.read_sector(lba), crc)
        return crc
