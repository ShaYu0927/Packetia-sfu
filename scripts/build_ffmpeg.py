#!/usr/bin/env python3
"""Build the vendored FFmpeg in an isolated directory, without a system install."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
CODEC_LIBRARIES = {
    'x264': ('x264', ['--enable-libx264', '--enable-encoder=libx264']),
    'x265': ('x265', ['--enable-libx265', '--enable-encoder=libx265']),
    'vpx': ('vpx', ['--enable-libvpx', '--enable-encoder=libvpx_vp8,libvpx_vp9',
                    '--enable-decoder=libvpx_vp8,libvpx_vp9']),
    'dav1d': ('dav1d', ['--enable-libdav1d', '--enable-decoder=libdav1d']),
    'aom': ('aom', ['--enable-libaom', '--enable-encoder=libaom_av1', '--enable-decoder=libaom_av1']),
    'svtav1': ('SvtAv1Enc', ['--enable-libsvtav1', '--enable-encoder=libsvtav1']),
    'opus': ('opus', ['--enable-libopus', '--enable-encoder=libopus', '--enable-decoder=libopus']),
}


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--source-dir', type=Path, default=ROOT / 'third/ffmpeg/ffmpeg-9.0.1')
    result.add_argument('--output-dir', type=Path, default=ROOT / 'build/ffmpeg')
    result.add_argument('--jobs', type=int, default=min(os.cpu_count() or 1, 8))
    result.add_argument('--codec-library', action='append', choices=CODEC_LIBRARIES, default=[],
                        help='Enable an installed codec library; may be repeated')
    result.add_argument('--enable-gpl', action='store_true', help='Required when enabling x264 or x265')
    result.add_argument('--programs', action='store_true', help='Also build ffmpeg/ffprobe and test fixture filters')
    result.add_argument('--disable-asm', action='store_true', help='Build without assembly optimizations')
    result.add_argument('--dry-run', action='store_true', help='Print commands without checking tools or writing files')
    return result


def plan(args):
    source = args.source_dir.resolve()
    output = args.output_dir.resolve()
    if not all((source / name).is_file() for name in ('configure', 'RELEASE', 'Makefile', 'libavcodec/Makefile', 'doc/Makefile')):
        raise ValueError('source directory must contain FFmpeg configure, RELEASE and upstream Makefiles')
    if output == source or source in output.parents:
        raise ValueError('output directory must be outside the FFmpeg source tree')
    if args.jobs < 1:
        raise ValueError('--jobs must be positive')
    libraries = sorted(set(args.codec_library))
    if set(libraries) & {'x264', 'x265'} and not args.enable_gpl:
        raise ValueError('x264/x265 require --enable-gpl; this changes the FFmpeg build license')
    work, prefix = output / 'work', output / 'install'
    flags = [str(source / 'configure'), f'--prefix={prefix}', f'--libdir={prefix / "lib"}',
        '--enable-shared', '--disable-static', '--enable-pic', '--disable-autodetect',
        '--disable-doc', '--disable-debug', '--disable-everything', '--disable-network',
        '--disable-avdevice', '--disable-avfilter', '--disable-programs',
        '--enable-avcodec', '--enable-avformat', '--enable-avutil', '--enable-swscale', '--enable-swresample',
        '--enable-protocol=file,pipe', '--enable-demuxer=mov,h264,hevc,aac,ogg,matroska,ivf',
        '--enable-muxer=mp4,mov,adts,ogg,matroska,webm,ivf',
        '--enable-parser=h264,hevc,vp8,vp9,av1,aac,opus',
        '--enable-decoder=h264,hevc,vp8,vp9,aac,opus,pcm_s16le',
        '--enable-encoder=aac,pcm_s16le,rawvideo',
        '--enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc,extract_extradata']
    if args.enable_gpl:
        flags.append('--enable-gpl')
    if args.disable_asm:
        flags.append('--disable-asm')
    for name in libraries:
        flags.extend(CODEC_LIBRARIES[name][1])
    if args.programs:
        flags += ['--enable-ffmpeg', '--enable-ffprobe', '--enable-avfilter', '--enable-avdevice',
            '--enable-indev=lavfi', '--enable-muxer=null',
            '--enable-decoder=rawvideo,pcm_f64le,wrapped_avframe', '--enable-encoder=wrapped_avframe',
            '--enable-filter=abuffer,buffer,abuffersink,buffersink,testsrc2,sine,aresample,aformat,format,scale,null,anull,trim,atrim,setpts,asetpts']
    # The local CLI and shared libraries must also work outside the build directory.
    if platform.system() == 'Linux':
        flags.append(f'--extra-ldflags=-Wl,-rpath,{prefix / "lib"}')
    return {'source': str(source), 'version': (source / 'RELEASE').read_text().strip(),
            'work': str(work), 'prefix': str(prefix), 'libraries': libraries, 'configure': flags}


def run(command, work, env, log):
    print('+ ' + shlex.join(command), flush=True)
    with log.open('ab') as stream:
        stream.write(('\n+ ' + shlex.join(command) + '\n').encode())
        stream.flush()
        result = subprocess.run(command, cwd=work, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        tail = log.read_text(errors='replace').splitlines()[-50:]
        raise RuntimeError(f'command failed ({result.returncode}); log: {log}\n' + '\n'.join(tail))


def build(args):
    configuration = plan(args)
    work, prefix = Path(configuration['work']), Path(configuration['prefix'])
    install_targets = ['install-libs', 'install-headers']
    if args.programs:
        install_targets += ['install-progs', 'install-data']
    if args.dry_run:
        print(json.dumps(configuration, indent=2))
        print(shlex.join(['make', f'-j{args.jobs}']))
        print(shlex.join(['make', *install_targets]))
        return
    if platform.system() not in ('Darwin', 'Linux'):
        raise ValueError('this script currently supports native macOS and Linux builds')
    if (Path(configuration['source']) / 'ffbuild/config.mak').exists():
        raise ValueError('FFmpeg source is configured in-tree; use a clean source tree for an out-of-tree build')
    env = dict(os.environ)
    make = shutil.which('gmake') or shutil.which('make')
    if not make or not shutil.which('cc'):
        raise ValueError('a C compiler and GNU make are required')
    pkg_config = env.get('PKG_CONFIG', 'pkg-config')
    if configuration['libraries']:
        if not shutil.which(pkg_config):
            raise ValueError('pkg-config is required for external codec libraries')
        for name in configuration['libraries']:
            package = CODEC_LIBRARIES[name][0]
            if subprocess.run([pkg_config, '--exists', package], env=env).returncode:
                raise ValueError(f'missing {name} development package ({package}.pc); install it or omit --codec-library {name}')
    work.mkdir(parents=True, exist_ok=True)
    stamp, log = work / 'packetia-config.json', work / 'build.log'
    # Flags and toolchain changes require a clean generated tree. Source edits are
    # still picked up by make on every invocation, including when the stamp matches.
    signature = dict(configuration, system=platform.system(), machine=platform.machine(),
        configure_sha256=hashlib.sha256((Path(configuration['source']) / 'configure').read_bytes()).hexdigest(),
        environment={key: env.get(key, '') for key in
                     ('CC', 'CXX', 'CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'PKG_CONFIG', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR')})
    previous = json.loads(stamp.read_text()) if stamp.exists() else None
    if previous != signature or not (work / 'ffbuild/config.mak').is_file():
        if (work / 'Makefile').exists():
            run([make, 'distclean'], work, env, log)
        stamp.unlink(missing_ok=True)
        run(configuration['configure'], work, env, log)
        stamp.write_text(json.dumps(signature, indent=2) + '\n')
    run([make, f'-j{args.jobs}'], work, env, log)
    # Install the SDK and optional tools, without upstream documentation/examples
    # that are not part of this vendored source subset.
    run([make, *install_targets], work, env, log)
    (prefix / 'packetia-build.json').write_text(json.dumps(signature, indent=2) + '\n')
    print(f'FFmpeg {configuration["version"]} installed to {prefix}\n'
          f'Configure Packetia with -DPACKETIA_FFMPEG_ROOT={shlex.quote(str(prefix))}', flush=True)


if __name__ == '__main__':
    arguments = parser().parse_args()
    try:
        build(arguments)
    except (ValueError, RuntimeError, OSError) as error:
        print(f'FFmpeg build failed: {error}', file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print('FFmpeg build interrupted; rerun the same command to continue.', file=sys.stderr)
        sys.exit(130)
