"""Reuses the Geargrafx output (gg_trace, gg_cdinfo) between test runs.

The Geargrafx answer only changes if the executable itself, the arguments or the files read (game,
BIOS, button script, save) change. Next to the outputs there is a <first output>.key file with the
fingerprint of all that; if it matches and the outputs exist, Geargrafx does not run again.

Files up to 16 MB are keyed by content; larger ones (disc images, large ROMs), by size and date.
For a .cue, the track files it references (FILE "...") are included too.
PCE_GG_CACHE=0 disables it (always runs Geargrafx).

Usage:
    ref = gg_cache.start(cmd, [output1, output2])  # runs Geargrafx in the background, if needed
    ...                                            # (run our side meanwhile)
    if ref.wait() != 0: ...                        # 0 = outputs ready (from this run or cached)
"""
import hashlib
import os
import re
import subprocess

SMALL = 16 << 20


def _file_key(h, path):
    st = os.stat(path)
    h.update(f"file {os.path.abspath(path).lower()} {st.st_size}\n".encode())
    if st.st_size <= SMALL:
        with open(path, "rb") as f:
            h.update(hashlib.sha256(f.read()).digest())
    else:
        h.update(f"{st.st_mtime_ns}\n".encode())
    if path.lower().endswith(".cue") and st.st_size <= SMALL:
        with open(path, encoding="utf-8", errors="replace") as f:
            for name in re.findall(r'^\s*FILE\s+"([^"]+)"', f.read(), re.M):
                track = os.path.join(os.path.dirname(path), name)
                if os.path.exists(track):
                    _file_key(h, track)


def key(cmd, outputs, cwd=""):
    """Fingerprint of the command (the outputs are left out: only their names change from test to test)."""
    h = hashlib.sha256()
    for i, arg in enumerate(cmd):
        path = os.path.join(cwd, arg)
        if arg in outputs:
            h.update(b"<output>\n")
        elif os.path.isfile(path):
            _file_key(h, path)
        elif i == 0:
            raise FileNotFoundError(arg)
        else:
            h.update(f"arg {arg}\n".encode())
    return h.hexdigest()


class _Ref:
    def __init__(self, proc, key_path, digest):
        self.proc, self.key_path, self.digest = proc, key_path, digest
        self.reused = proc is None

    def wait(self):
        if self.proc is None:
            return 0
        rc = self.proc.wait()
        if rc == 0:
            with open(self.key_path, "w") as f:
                f.write(self.digest + "\n")
        return rc


def start(cmd, outputs, **popen_args):
    cmd = [str(c) for c in cmd]
    outputs = [str(o) for o in outputs]
    base = popen_args.get("cwd") or ""
    digest = key(cmd, outputs, base)
    key_path = os.path.join(base, outputs[0]) + ".key"
    ready = all(os.path.exists(os.path.join(base, o)) for o in outputs) and os.path.exists(key_path)
    if os.environ.get("PCE_GG_CACHE", "1") != "0" and ready:
        with open(key_path) as f:
            if f.read().strip() == digest:
                return _Ref(None, key_path, digest)
    if os.path.exists(key_path):
        os.remove(key_path)  # an interrupted run does not leave half-written outputs counting as valid
    popen_args.setdefault("stdout", subprocess.DEVNULL)
    return _Ref(subprocess.Popen(cmd, **popen_args), key_path, digest)


def run(cmd, outputs, **popen_args):
    return start(cmd, outputs, **popen_args).wait()
