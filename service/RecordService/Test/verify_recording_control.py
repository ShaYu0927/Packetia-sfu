"""Run the real recording service and decode every completed MP4 with FFmpeg."""
import pathlib
import subprocess
import sys
import tempfile


def main():
    executable, fixture, ffmpeg, output_root = sys.argv[1:]
    root = pathlib.Path(output_root).resolve()
    root.mkdir(parents=True, exist_ok=True)
    output = pathlib.Path(tempfile.mkdtemp(prefix="stream-control-", dir=root))
    print(f"Recording artifacts: {output}", flush=True)
    subprocess.run([executable, fixture, str(output)], check=True, timeout=30)
    files = sorted(output.glob("*.mp4"))
    if len(files) != 3:
        raise RuntimeError(f"Expected three completed recordings, found {len(files)}")
    for path in files:
        subprocess.run([ffmpeg, "-hide_banner", "-v", "error", "-xerror", "-i", str(path),
                        "-map", "0:v:0", "-map", "0:a:0", "-f", "null", "-"],
                       check=True, timeout=30)
    print("All three recordings decoded successfully; artifacts retained.")


if __name__ == "__main__":
    main()
