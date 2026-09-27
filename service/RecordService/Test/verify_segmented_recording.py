"""Verify standalone fMP4 structure and fully decode each indexed segment."""
import pathlib
import struct
import subprocess
import sys
import tempfile


def main():
    executable, fixture, ffmpeg, output_root = sys.argv[1:]
    root = pathlib.Path(output_root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    output = pathlib.Path(tempfile.mkdtemp(prefix="segments-", dir=root))
    print(f"Artifacts: {output}", flush=True)
    subprocess.run([executable, fixture, str(output)], check=True, timeout=45)
    paths = (output / "completed.txt").read_text().splitlines()
    if len(paths) != 12 or len(set(paths)) != 12:
        raise RuntimeError("Expected 12 distinct completed segments")
    for name in paths:
        data = pathlib.Path(name).read_bytes()
        boxes, offset = [], 0
        while offset < len(data):
            size, kind = struct.unpack_from(">I4s", data, offset)
            header = 8
            if size == 1:
                size = struct.unpack_from(">Q", data, offset + 8)[0]
                header = 16
            elif size == 0:
                size = len(data) - offset
            if size < header or offset + size > len(data):
                raise RuntimeError(f"Invalid MP4 box in {name}")
            boxes.append(kind)
            offset += size
        if not {b"ftyp", b"moov", b"moof", b"mdat"}.issubset(boxes):
            raise RuntimeError(f"Not a standalone fragmented MP4: {name}")
        subprocess.run([ffmpeg, "-hide_banner", "-v", "error", "-xerror", "-i", name,
                        "-map", "0:v:0", "-map", "0:a:0", "-f", "null", "-"],
                       check=True, timeout=15)
    print("All 12 indexed fMP4 segments decoded successfully.")


if __name__ == "__main__":
    main()
