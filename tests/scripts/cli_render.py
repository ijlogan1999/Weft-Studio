"""weft-cli render: a new project with the metronome on renders clicks to a WAV file."""

import pathlib
import shutil
import struct
import subprocess
import sys

cli, work = sys.argv[1], pathlib.Path(sys.argv[2])
shutil.rmtree(work, ignore_errors=True)
work.mkdir(parents=True)
subprocess.run([cli, "new", str(work), "Render"], check=True)
project = next(work.glob("*.weft"))
out = work / "out.wav"
result = subprocess.run([cli, "render", str(project), str(out), "--seconds", "2", "--metronome"],
                        check=True, capture_output=True, text=True)
print(result.stdout.strip())
assert "frames=96000" in result.stdout, result.stdout
data = out.read_bytes()
assert data[:4] == b"RIFF" and data[8:12] == b"WAVE"
pos, frames = 12, None
while pos < len(data):
    cid, size = data[pos:pos + 4], struct.unpack("<I", data[pos + 4:pos + 8])[0]
    if cid == b"data":
        samples = struct.unpack(f"<{size // 4}f", data[pos + 8:pos + 8 + size])
        frames = len(samples) // 2
        # Clicks at 0, 0.5, 1.0 and 1.5 s (120 BPM): each starts after silence.
        onsets = [i for i in range(1, frames) if samples[2 * i] != 0 and samples[2 * i - 2] == 0 and i % 24000 == 0]
        assert samples[0] != 0, "no click at 0"
        assert onsets == [24000, 48000, 72000], onsets
    pos += 8 + size + (size & 1)
assert frames == 96000, frames
print("ok")
