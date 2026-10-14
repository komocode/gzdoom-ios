#!/usr/bin/env python3
"""Build and run the Apple ports. Downloads are pinned and SHA-256 checked."""
import argparse
import hashlib
import os
import pathlib
import platform
import plistlib
import datetime
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[1]
DEPS = ROOT / 'build-ios/deps'
DOWNLOADS = [
    ('openal-soft-1.25.2.tar.gz', 'https://github.com/kcat/openal-soft/archive/refs/tags/1.25.2.tar.gz', 'fb27e5839aa11f0e5b9d33756965291fad5d6909ab928ea1f796f4a1a6877894', 'openal-soft-1.25.2'),
    ('SDL2-2.32.10.tar.gz', 'https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz', '5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165', 'SDL2-2.32.10'),
    ('MoltenVK-all.tar', 'https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-all.tar', '562a15a29bc358446a56a4091c5f7e08f604184187c1d34f712148b61ef17276', 'MoltenVK'),
    ('libvpx-1.15.2.tar.gz', 'https://github.com/webmproject/libvpx/archive/refs/tags/v1.15.2.tar.gz', '26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa', 'libvpx-1.15.2'),
]

def run(args, cwd=ROOT, env=None):
    print('+', ' '.join(str(a) for a in args), flush=True)
    subprocess.run([str(a) for a in args], cwd=cwd, env=env, check=True)

def fetch():
    DEPS.mkdir(parents=True, exist_ok=True)
    for name, url, digest, folder in DOWNLOADS:
        archive = DEPS / name
        if not archive.exists():
            tmp = archive.with_suffix('.download')
            print('Downloading', name, flush=True)
            urllib.request.urlretrieve(url, tmp)
            tmp.replace(archive)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != digest:
            raise RuntimeError('Checksum mismatch: ' + str(archive))
        if not (DEPS / folder).exists():
            with tarfile.open(archive) as tar:
                if hasattr(tarfile, 'data_filter'):
                    tar.extractall(DEPS, filter='data')
                else:
                    # Python supplied by older Xcode releases lacks data_filter.
                    for member in tar.getmembers():
                        dest = (DEPS / member.name).resolve()
                        if DEPS.resolve() not in dest.parents and dest != DEPS.resolve():
                            raise RuntimeError('Unsafe archive path: ' + member.name)
                        if member.issym() or member.islnk():
                            link = ((dest.parent if member.issym() else DEPS) / member.linkname).resolve()
                            if DEPS.resolve() not in link.parents:
                                raise RuntimeError('Unsafe archive link: ' + member.name)
                        elif not (member.isfile() or member.isdir()):
                            raise RuntimeError('Unexpected archive entry: ' + member.name)
                    tar.extractall(DEPS)
    allocator = DEPS / 'openal-soft-1.25.2/common/almalloc.h'
    if 'Darwin\'s aligned allocation' not in allocator.read_text():
        run(['patch', '--forward', '-p1', '-i', ROOT / 'ios/patches/openal-soft-apple-alignment.patch'], cwd=allocator.parents[1])

def configure(source, build, options):
    run(['cmake', '-S', source, '-B', build, '-G', 'Ninja', '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', '-DCMAKE_BUILD_TYPE=Release', *options])

def build(target, args):
    if target in ('watchos', 'watchos-simulator'):
        return build_watch(target, args)
    fetch()
    arch = args.arch or ('arm64' if target in ('ios', 'tvos') else platform.machine())
    sdk = {'ios': 'iphoneos', 'simulator': 'iphonesimulator', 'macos': 'macosx',
           'tvos': 'appletvos', 'tvos-simulator': 'appletvsimulator'}[target]
    base = ROOT / ('build-' + target + '-' + arch)
    prefix = base / 'deps-install'
    mobile = target != 'macos'
    deploy = '18.0' if mobile else '12.0'
    common = [f'-DCMAKE_OSX_SYSROOT={sdk}', f'-DCMAKE_OSX_ARCHITECTURES={arch}', f'-DCMAKE_OSX_DEPLOYMENT_TARGET={deploy}', f'-DCMAKE_INSTALL_PREFIX={prefix}']
    if mobile:
        system = 'tvOS' if target.startswith('tvos') else 'iOS'
        common += [f'-DCMAKE_SYSTEM_NAME={system}', '-DCMAKE_TRY_COMPILE_TARGET_TYPE=EXECUTABLE']
    sdlbuild = base / 'sdl'
    if not (prefix / 'lib/libSDL2.a').exists():
        configure(DEPS / 'SDL2-2.32.10', sdlbuild, common + ['-DSDL_SHARED=OFF', '-DSDL_STATIC=ON', '-DSDL_TEST=OFF', '-DSDL_TESTS=OFF'])
        run(['cmake', '--build', sdlbuild, '-j', args.jobs])
        run(['cmake', '--install', sdlbuild])
    vpxbuild = base / 'vpx'
    vpxbuild.mkdir(parents=True, exist_ok=True)
    if not (prefix / 'lib/libvpx.a').exists():
        sdkpath = subprocess.check_output(['xcrun', '--sdk', sdk, '--show-sdk-path'], text=True).strip()
        triple = arch + '-apple-' + {
            'simulator': 'ios18.0-simulator', 'ios': 'ios18.0', 'macos': 'macos12.0',
            'tvos': 'tvos18.0', 'tvos-simulator': 'tvos18.0-simulator'}[target]
        # Portable decoding avoids instructions unavailable on A10/A12 and Intel simulators.
        env = dict(os.environ, CC='xcrun clang', CXX='xcrun clang++', AR='ar', AS='xcrun clang', LD='xcrun clang')
        flags = f'-target {triple} -isysroot {sdkpath}'
        env['LDFLAGS'] = flags
        run([DEPS / 'libvpx-1.15.2/configure', '--target=generic-gnu', f'--prefix={prefix}', '--disable-examples', '--disable-tools', '--disable-docs', '--disable-unit-tests', '--disable-vp8-encoder', '--disable-vp9-encoder', '--disable-shared', '--enable-static', f'--extra-cflags={flags}', f'--extra-cxxflags={flags}'], cwd=vpxbuild, env=env)
        run(['make', '-j', args.jobs], cwd=vpxbuild, env=env)
        run(['make', 'install'], cwd=vpxbuild, env=env)
    engine = base / 'engine'
    options = common + ['-DGZDOOM_BUNDLE_TEST_WADS=' + ('ON' if args.test_wads else 'OFF'), f'-DCMAKE_PREFIX_PATH={prefix}', f'-DSDL2_DIR={prefix}/lib/cmake/SDL2', f'-DSDL2_INCLUDE_DIR={prefix}/include;{prefix}/include/SDL2', f'-DGZDOOM_OPENAL_SOURCE={DEPS}/openal-soft-1.25.2', '-DDYN_OPENAL=OFF', f'-DVPX_INCLUDE_DIR={prefix}/include', f'-DVPX_LIBRARIES={prefix}/lib/libvpx.a', f'-DGZDOOM_MOLTENVK_ROOT={DEPS}/MoltenVK', '-DPK3_QUIET_ZIPDIR=ON', '-DBUILD_SHARED_LIBS=OFF', '-DFORCE_INTERNAL_ZMUSIC=ON', '-DZMUSIC_WITH_FLUIDSYNTH=OFF', '-DFORCE_INTERNAL_BZIP2=ON', '-DVULKAN_USE_XLIB=OFF']
    if mobile:
        host = ROOT / 'build-ios/host-tools'
        configure(ROOT / 'ios/host-tools', host, [])
        run(['cmake', '--build', host, '-j', args.jobs])
        options += [f'-DIMPORT_EXECUTABLES={host}/ImportExecutables.cmake', '-DHAVE_GLES2=OFF', '-DHAVE_VM_JIT=OFF', '-DDYN_OPENAL=OFF', '-DGZDOOM_BUNDLE_TEST_WADS=' + ('ON' if args.test_wads else 'OFF')]
        if args.team:
            options += [f'-DGZDOOM_DEVELOPMENT_TEAM={args.team}']
    configure(ROOT, engine, options)
    run(['cmake', '--build', engine, '-j', args.jobs])
    app = engine / 'gzdoom.app'
    print('\nBuilt:', app, flush=True)
    return app

def build_watch(target, args):
    fetch()
    arch = args.arch or ('arm64' if target == 'watchos' else platform.machine())
    sdk = 'watchos' if target == 'watchos' else 'watchsimulator'
    deploy = '26.0' if target == 'watchos' and arch == 'arm64' else '11.0'
    base = ROOT / ('build-' + target + '-' + arch)
    prefix = base / 'deps-install'
    vpxbuild = base / 'vpx'
    vpxbuild.mkdir(parents=True, exist_ok=True)
    triple = arch + '-apple-watchos' + deploy + ('-simulator' if target.endswith('-simulator') else '')
    if not (prefix / 'lib/libvpx.a').exists():
        sdkpath = subprocess.check_output(['xcrun', '--sdk', sdk, '--show-sdk-path'], text=True).strip()
        env = dict(os.environ, CC='xcrun clang', CXX='xcrun clang++', AR='ar', AS='xcrun clang', LD='xcrun clang')
        flags = f'-target {triple} -isysroot {sdkpath}'
        env['LDFLAGS'] = flags
        run([DEPS / 'libvpx-1.15.2/configure', '--target=generic-gnu', f'--prefix={prefix}', '--disable-examples', '--disable-tools', '--disable-docs', '--disable-unit-tests', '--disable-vp8-encoder', '--disable-vp9-encoder', '--disable-shared', '--enable-static', f'--extra-cflags={flags}', f'--extra-cxxflags={flags}'], cwd=vpxbuild, env=env)
        run(['make', '-j', args.jobs], cwd=vpxbuild, env=env)
        run(['make', 'install'], cwd=vpxbuild, env=env)
    host = ROOT / 'build-ios/host-tools'
    configure(ROOT / 'ios/host-tools', host, [])
    run(['cmake', '--build', host, '-j', args.jobs])
    configure(ROOT, base / 'engine', [
        '-DCMAKE_SYSTEM_NAME=watchOS', f'-DCMAKE_OSX_SYSROOT={sdk}',
        f'-DCMAKE_OSX_ARCHITECTURES={arch}', f'-DCMAKE_OSX_DEPLOYMENT_TARGET={deploy}',
        f'-DCMAKE_Swift_FLAGS=-target {triple} -module-cache-path {base}/swift-module-cache',
        f'-DVPX_INCLUDE_DIR={prefix}/include', f'-DVPX_LIBRARIES={prefix}/lib/libvpx.a',
        f'-DIMPORT_EXECUTABLES={host}/ImportExecutables.cmake',
        '-DPK3_QUIET_ZIPDIR=ON', '-DZMUSIC_WITH_FLUIDSYNTH=OFF',
        '-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local',
        '-DGZDOOM_BUNDLE_TEST_WADS=' + ('ON' if args.test_wads else 'OFF')])
    run(['cmake', '--build', base / 'engine', '-j', args.jobs])
    app = base / 'engine/gzdoom-watch.app'
    print('\nBuilt:', app, flush=True)
    return app

def universal_macos(args):
    apps = []
    for arch in ('arm64', 'x86_64'):
        per_arch = argparse.Namespace(**vars(args))
        per_arch.arch = arch
        apps.append(build('macos', per_arch))
    app = ROOT / 'build-macos-universal/gzdoom.app'
    if app.exists():
        shutil.rmtree(app)
    app.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(apps[0], app, symlinks=True)
    exe = pathlib.Path('Contents/MacOS/gzdoom')
    run(['xcrun', 'lipo', '-create', apps[0] / exe, apps[1] / exe, '-output', app / exe])
    # macOS treats directories inside MacOS as nested code during signing.
    # Keep the executable and loader there, with game data in Resources.
    resources = app / 'Contents/Resources'
    for item in (app / 'Contents/MacOS').iterdir():
        if item.name == 'gzdoom' or item.suffix == '.dylib':
            continue
        destination = resources / item.name
        if destination.exists():
            if destination.is_dir():
                shutil.rmtree(destination)
            else:
                destination.unlink()
        shutil.move(item, destination)
    run(['codesign', '--force', '--sign', '-', app / 'Contents/MacOS/libMoltenVK.dylib'])
    run(['codesign', '--force', '--sign', '-', app])
    run(['codesign', '--verify', '--deep', '--strict', app])
    print('Universal Mac app:', app, flush=True)
    return app

def sign_device(app, device, profile_path=None):
    identities = subprocess.check_output(['security', 'find-identity', '-v', '-p', 'codesigning'], text=True)
    candidates = [pathlib.Path(profile_path)] if profile_path else list((pathlib.Path.home() / 'Library/Developer/Xcode/UserData/Provisioning Profiles').glob('*.mobileprovision'))
    now = datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
    bundle = plistlib.loads((app / 'Info.plist').read_bytes())['CFBundleIdentifier']
    for profile in candidates:
        decoded = subprocess.run(['security', 'cms', '-D', '-i', profile], capture_output=True)
        try:
            data = plistlib.loads(decoded.stdout)
        except Exception:
            continue
        e = data.get('Entitlements', {})
        identifier = e.get('application-identifier', '')
        if not identifier.endswith('.*') or not e.get('get-task-allow') or data['ExpirationDate'] < now:
            continue
        if device not in data.get('ProvisionedDevices', []):
            continue
        identity = next((hashlib.sha1(c).hexdigest().upper() for c in data['DeveloperCertificates'] if hashlib.sha1(c).hexdigest().upper() in identities), None)
        if not identity:
            continue
        team = data['TeamIdentifier'][0]
        entitlements = {'application-identifier': team + '.' + bundle, 'com.apple.developer.team-identifier': team, 'get-task-allow': True, 'keychain-access-groups': [team + '.' + bundle]}
        path = app.parent / 'device-entitlements.plist'
        path.write_bytes(plistlib.dumps(entitlements))
        shutil.copyfile(profile, app / 'embedded.mobileprovision')
        framework = app / 'Frameworks/MoltenVK.framework'
        if framework.exists():
            run(['codesign', '--force', '--sign', identity, '--timestamp=none', framework])
        run(['codesign', '--force', '--sign', identity, '--timestamp=none', '--entitlements', path, app])
        run(['codesign', '--verify', '--deep', '--strict', app])
        return
    raise RuntimeError('No valid wildcard development profile and certificate covers this device. Use Xcode automatic signing or provide --profile; see ios/README.md.')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('target', choices=['ios', 'simulator', 'macos', 'tvos', 'tvos-simulator', 'watchos', 'watchos-simulator'])
    p.add_argument('--universal', action='store_true', help='Build a single Mac app for Apple Silicon and Intel')
    p.add_argument('--arch', choices=['arm64', 'x86_64'])
    p.add_argument('--jobs', type=int, default=min(os.cpu_count() or 4, 10))
    p.add_argument('--test-wads', action='store_true', help='Opt in to bundling local vendor WADs (development only)')
    p.add_argument('--run', action='store_true')
    p.add_argument('--device', help='Simulator or physical-device UDID')
    p.add_argument('--team', help='Optional development team recorded in CMake for Xcode signing')
    p.add_argument('--profile', help='Optional wildcard iOS development profile (otherwise detected automatically)')
    args = p.parse_args()
    if args.universal and args.target != 'macos':
        p.error('--universal is only supported for macos')
    if args.target in ('ios', 'tvos') and args.arch == 'x86_64':
        p.error('Physical iOS/tvOS devices require arm64')
    if args.target == 'watchos' and args.arch == 'x86_64':
        p.error('GZDoom requires the arm64 Watch ABI available on newer Watches running watchOS 26 or later')
    app = universal_macos(args) if args.universal else build(args.target, args)
    bundle_id = plistlib.loads((app / ('Contents/Info.plist' if args.target == 'macos' else 'Info.plist')).read_bytes())['CFBundleIdentifier']
    if args.run:
        if args.target == 'macos':
            # open reuses a running app instance.
            wad = next(iter(sorted((ROOT / 'vendor').glob('*.WAD'))), None)
            cmd = ['open', app, '--args', '+vid_preferbackend', '1']
            if wad and args.test_wads:
                cmd += ['-iwad', wad]
            run(cmd)
        elif args.target in ('simulator', 'tvos-simulator', 'watchos-simulator'):
            device = args.device or 'booted'
            framework = app / 'Frameworks/MoltenVK.framework'
            if framework.exists():
                run(['codesign', '--force', '--sign', '-', framework])
            run(['codesign', '--force', '--sign', '-', app])
            run(['xcrun', 'simctl', 'install', device, app])
            run(['xcrun', 'simctl', 'launch', device, bundle_id])
        else:
            if not args.device:
                raise RuntimeError('Provide the physical device UDID using --device (xcrun devicectl list devices).')
            sign_device(app, args.device, args.profile)
            run(['xcrun', 'devicectl', 'device', 'install', 'app', '--device', args.device, app])
            run(['xcrun', 'devicectl', 'device', 'process', 'launch', '--device', args.device, bundle_id])

if __name__ == '__main__':
    try:
        main()
    except (subprocess.CalledProcessError, RuntimeError) as err:
        sys.exit(str(err))
