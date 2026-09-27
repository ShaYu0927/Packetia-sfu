#!/usr/bin/env python3
"""Compose two recordings into one side-by-side H264/AAC MP4 preview."""

import argparse
import json
import math
from pathlib import Path
import shutil
import subprocess
import sys


def positive_seconds(value):
    seconds = float(value)
    if not math.isfinite(seconds) or seconds <= 0:
        raise argparse.ArgumentTypeError("duration must be a positive finite number")
    return seconds


def recording_streams(ffprobe, path):
    if not path.is_file():
        raise ValueError(f"Input file does not exist: {path}")
    result = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "stream=codec_type",
         "-of", "json", str(path.resolve())],
        check=True, capture_output=True, text=True,
    )
    types = {stream.get("codec_type") for stream in json.loads(result.stdout)["streams"]}
    if "video" not in types:
        raise ValueError(f"Input must contain a video track: {path}")
    return "audio" in types


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--demo", action="store_true", help="use generated pictures and tones")
    parser.add_argument("--left", type=Path, help="recording to display on the left")
    parser.add_argument("--right", type=Path, help="recording to display on the right")
    parser.add_argument("--duration", type=positive_seconds, default=10,
                        help="maximum preview duration in seconds (default: 10)")
    parser.add_argument("--output", type=Path, required=True, help="new .mp4 file; never overwritten")
    args = parser.parse_args()
    if args.demo:
        if args.left or args.right:
            parser.error("--demo cannot be combined with --left or --right")
    elif not args.left or not args.right:
        parser.error("specify --demo or both --left and --right")
    if args.output.suffix.lower() != ".mp4":
        parser.error("--output must end in .mp4")
    if args.output.exists():
        parser.error(f"Output already exists: {args.output}")
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        parser.error("ffmpeg is required on PATH (Ubuntu: sudo apt install ffmpeg)")

    if args.demo:
        inputs = [
            "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
            "-f", "lavfi", "-i", "smptebars=size=640x360:rate=25",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
            "-f", "lavfi", "-i", "sine=frequency=880:sample_rate=48000",
        ]
        audio_sources = ["2:a:0", "3:a:0"]
    else:
        ffprobe = shutil.which("ffprobe")
        if not ffprobe:
            parser.error("ffprobe is required on PATH to inspect input tracks")
        has_audio = [recording_streams(ffprobe, path) for path in (args.left, args.right)]
        inputs = ["-i", str(args.left.resolve()), "-i", str(args.right.resolve())]
        audio_sources = [f"{index}:a:0" if present else None
                         for index, present in enumerate(has_audio)]

    filters = []
    for index in range(2):
        # Normalize geometry, frame rate and time base before the horizontal stack.
        filters.append(
            f"[{index}:v:0]setpts=PTS-STARTPTS,"
            "scale=640:360:force_original_aspect_ratio=decrease:force_divisible_by=2,"
            "pad=640:360:(ow-iw)/2:(oh-ih)/2:black,setsar=1,"
            f"fps=25,settb=AVTB,format=yuv420p[v{index}]"
        )
    filters.append("[v0][v1]hstack=inputs=2:shortest=1[video]")
    for index, source in enumerate(audio_sources):
        start = (f"[{source}]asetpts=PTS-STARTPTS,aresample=48000,"
                 "aformat=sample_fmts=fltp:channel_layouts=stereo,"
                 f"apad=whole_dur={args.duration}"
                 if source else "anullsrc=r=48000:cl=stereo")
        filters.append(f"{start},atrim=duration={args.duration}[a{index}]")
    filters.append("[a0][a1]amix=inputs=2:duration=longest:dropout_transition=0:normalize=1[audio]")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    command = [
        ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostdin", "-n",
        *inputs, "-filter_complex", ";".join(filters),
        "-map", "[video]", "-map", "[audio]",
        "-c:v", "libx264", "-preset", "veryfast", "-crf", "23",
        "-pix_fmt", "yuv420p", "-bf", "0", "-g", "50",
        "-c:a", "aac", "-b:a", "128k", "-ar", "48000", "-ac", "2",
        "-t", str(args.duration), "-shortest", "-movflags", "+faststart",
        str(args.output.resolve()),
    ]
    subprocess.run(command, check=True)
    print(f"Created: {args.output.resolve()}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Composition failed: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr, file=sys.stderr)
        sys.exit(1)
