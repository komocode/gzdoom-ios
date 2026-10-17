#!/usr/bin/env python3
"""Compile sprite shader permutations and run offscreen Metal material checks.

Run from any directory:
    python3 scripts/tests/validate_sprite_shaders.py

Requires CMake, a C++17 compiler, and (for Metal) Xcode with its Metal compiler.
Uses the repository's bundled glslang and the SPIRV-Cross dependency downloaded
by scripts/apple.py. Override that location with --spirv-cross-source. No game,
IWAD, window, network access, or installed shader-validation package is needed.
The GPU check requires a local Metal device; use --skip-gpu on headless hosts.
--preview-dir exports 512px synthetic material-fixture PNGs. These exercise
spatial maps through the actual shader; they are not game sprites or generated
production assets. Flat-normal PBR previews are not the original renderer.
--spirv-only runs the portable compiler checks without Apple dependencies.
--gpu-only reuses an existing --output-dir after verifying its assembled GLSL
still matches the current source. --sprite-path and --materials-dir render
actual authored PNG assets instead of synthetic fixtures. Supply --preview-dir
and optional --region name,x0,y0,x1,y1 for explicit material-region measurements.
Build products and evidence stay in a temporary directory, printed on exit.
"""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def run(command: list[str], log: Path) -> str:
    result = subprocess.run(command, capture_output=True, text=True)
    output = result.stdout + result.stderr
    log.write_text(output)
    if result.returncode:
        raise RuntimeError(f"{' '.join(command[:3])} failed; see {log}\n{output[-5000:]}")
    return output


def cmake_path(path: Path) -> str:
    return '"' + str(path).replace('\\', '/').replace('"', '\\"') + '"'


def validate_uniform_layouts(root: Path) -> dict[str, int]:
    """Compare ordered declarations, including new weather and reflection data.

    The GPU fixture follows translated GLSL layouts. This independent check
    ensures those declarations still agree with the engine's CPU structs.
    """
    cpu_types = {'VSMatrix': 'mat4', 'FVector4': 'vec4',
                 'FVector4PalEntry': 'vec4', 'float': 'float', 'int': 'int'}
    source = (root / 'src/common/rendering/hwrenderer/data/hw_viewpointuniforms.h').read_text()
    cpu = re.findall(r'^\s*(VSMatrix|FVector4|float|int)\s+m(\w+)\s*(?:[;=])', source, re.M)
    expected = [(cpu_types[kind], name if kind == 'VSMatrix' else 'u' + name) for kind, name in cpu]
    if not expected:
        raise RuntimeError('No CPU viewpoint fields discovered')
    for backend, path in [('GL', 'gl/gl_shader.cpp'), ('Vulkan/Metal', 'vulkan/shaders/vk_shader.cpp')]:
        shader = (root / 'src/common/rendering' / path).read_text()
        block = re.search(r'uniform(?: readonly)? ViewpointUBO\s*\{(.*?)\};', shader, re.S)
        actual = re.findall(r'^\s*(mat4|vec4|float|int)\s+(\w+)\s*;', block[1], re.M) if block else []
        if actual != expected:
            raise RuntimeError(f'{backend} viewpoint ABI mismatch\nCPU: {expected}\nGLSL: {actual}')
    source = (root / 'src/common/rendering/hwrenderer/data/hw_renderstate.h').read_text()
    block = re.search(r'struct StreamData\s*\{(.*?)\};', source, re.S)
    cpu = re.findall(r'^\s*(FVector4PalEntry|FVector4|float|int)\s+(\w+)\s*;', block[1], re.M) if block else []
    expected_stream = [(cpu_types[kind], name) for kind, name in cpu]
    shader = (root / 'src/common/rendering/vulkan/shaders/vk_shader.cpp').read_text()
    block = re.search(r'struct StreamData\s*\{(.*?)\};', shader, re.S)
    actual = re.findall(r'^\s*(vec4|float|int)\s+(\w+)\s*;', block[1], re.M) if block else []
    if not expected_stream or actual != expected_stream:
        raise RuntimeError(f'Stream ABI mismatch\nCPU: {expected_stream}\nGLSL: {actual}')
    print(f'Uniform ABI matches: {len(expected)} viewpoint and {len(actual)} stream fields')
    return {'viewpoint_fields': len(expected), 'stream_fields': len(actual)}


def build_compilers(root: Path, source: Path, output: Path, cross: Path, jobs: int, spirv_only: bool) -> Path:
    zvulkan = root / 'libraries/ZVulkan'
    paths = []
    for path in re.findall(r'\b(src/glslang/[^\s)]+\.cpp)\b', (zvulkan / 'CMakeLists.txt').read_text()):
        if '/Windows/' not in path and path not in paths:
            paths.append(path)
    resources = (zvulkan / 'src/vulkanbuilders.cpp').read_text()
    resources = '#include <glslang/glslang/Public/ShaderLang.h>\nstatic const TBuiltInResource' + resources.split('static const TBuiltInResource', 1)[1].split('\nvoid ShaderBuilder::Init', 1)[0]
    (output / 'sprite_shader_resources.h').write_text(resources)
    cmake = 'cmake_minimum_required(VERSION 3.24)\nproject(sprite_shader_validation LANGUAGES CXX)\nset(CMAKE_CXX_STANDARD 17)\n'
    cmake += 'add_executable(sprite_shader_check ' + cmake_path(source / 'sprite_shader_check.cpp') + '\n'
    cmake += ''.join('  ' + cmake_path(zvulkan / path) + '\n' for path in paths) + ')\n'
    cmake += 'target_include_directories(sprite_shader_check PRIVATE ' + cmake_path(zvulkan / 'src') + ' ' + cmake_path(output) + ')\n'
    if not spirv_only:
        if not (cross / 'spirv_msl.cpp').is_file():
            raise RuntimeError(f'SPIRV-Cross missing at {cross}; use --spirv-cross-source or fetch Apple dependencies with scripts/apple.py')
        cmake += 'add_executable(sprite_msl_check ' + cmake_path(source / 'sprite_msl_check.cpp') + '\n'
        for name in ['spirv_cross.cpp', 'spirv_parser.cpp', 'spirv_cross_parsed_ir.cpp', 'spirv_cfg.cpp', 'spirv_glsl.cpp', 'spirv_msl.cpp']:
            cmake += '  ' + cmake_path(cross / name) + '\n'
        cmake += ')\ntarget_include_directories(sprite_msl_check PRIVATE ' + cmake_path(cross) + ')\n'
    (output / 'CMakeLists.txt').write_text(cmake)
    build = output / 'build'
    run(['cmake', '-S', str(output), '-B', str(build), '-DCMAKE_BUILD_TYPE=Release'], output / 'configure.log')
    run(['cmake', '--build', str(build), '-j', str(jobs)], output / 'build.log')
    return build


def assemble_shaders(root: Path, output: Path) -> list[Path]:
    shader_dir = output / 'shaders'
    shader_dir.mkdir(exist_ok=True)
    vk = (root / 'src/common/rendering/vulkan/shaders/vk_shader.cpp').read_text()
    bindings = vk.split('static const char *shaderBindings = R"(', 1)[1].split(')";', 1)[0]
    patcher = (root / 'src/common/rendering/hwrenderer/data/hw_shaderpatcher.cpp').read_text()
    table = patcher.split('const FDefaultShader defaultshaders[] =', 1)[1].split('const FEffectShader', 1)[0]
    assets = root / 'wadsrc/static'
    shader_path = assets / 'shaders/glsl'
    fragment = (shader_path / 'main.fp').read_text()
    vertex = (shader_path / 'main.vp').read_text()
    files = []
    for name, func, light, definitions in re.findall(r'\{\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"\s*,\s*"([^"]*)"\s*\}', table):
        material = (assets / func).read_text()
        extra = ''
        # Match BuildFragmentSource's legacy material adapter.
        if 'SetupMaterial' not in material and 'ProcessMaterial' not in material:
            adapter = 'func_defaultmat2.fp' if 'GetTexCoord' in material else 'func_defaultmat.fp'
            extra = (shader_path / adapter).read_text()
            if 'ProcessTexel' not in material and 'GetTexCoord' not in material:
                extra = extra.replace('material.Base = ProcessTexel();', 'material.Base = Process(vec4(1.0));')
        definitions = definitions.replace('\\n', '\n')
        stem = re.sub('[^a-z0-9]+', '_', name.lower())
        for gbuffer in [False, True]:
            for rt in [False, True]:
                for alpha in [False, True]:
                    prefix = '#version 460\n#extension GL_EXT_ray_query : enable\n#define SUPPORTS_RAYTRACING\n' if rt else '#version 450 core\n'
                    prefix += '#extension GL_GOOGLE_include_directive : enable\n#define MAX_STREAM_DATA 1024\n#define NO_CLIPDISTANCE_SUPPORT\n' + definitions
                    prefix += '#define GBUFFER_PASS\n' if gbuffer else ''
                    prefix += '' if alpha else '#define NO_ALPHATEST\n'
                    code = prefix + bindings + '\n#line 1\n' + fragment + '\n#line 1\n' + extra + '\n' + material
                    code += '\n' + (shader_path / 'func_defaultlight.fp').read_text() + '\n' + (assets / light).read_text()
                    target = shader_dir / f'{stem}_gb{int(gbuffer)}_rt{int(rt)}_alpha{int(alpha)}.frag'
                    target.write_text(code)
                    files.append(target)
        for rt in [False, True]:
            prefix = '#version 460\n#extension GL_EXT_ray_query : enable\n' if rt else '#version 450 core\n'
            prefix += '#define MAX_STREAM_DATA 1024\n#define NO_CLIPDISTANCE_SUPPORT\n' + definitions
            target = shader_dir / f'{stem}_rt{int(rt)}.vert'
            target.write_text(prefix + bindings + '\n' + vertex)
            files.append(target)
    if not files:
        raise RuntimeError('No default shaders discovered; check shader table parser')
    return files


def validate_metal(build: Path, files: list[Path], output: Path, jobs: int) -> int:
    for platform in ['macos', 'ios']:
        text = run([str(build / 'sprite_msl_check'), platform, *[str(file) + '.spv' for file in files]], output / f'msl-{platform}.log')
        print(platform + ': ' + text.strip())
    # Native compile covers both Apple targets, all three material families,
    # both RT modes, both G-buffer modes, and the corresponding vertex stages.
    metal = [path for path in (output / 'shaders').glob('*.metal')
             if path.name.startswith(('pbr_', 'specular_', 'default_'))
             and ('alpha1' in path.name or '.vert' in path.name)]

    def compile_one(path: Path) -> None:
        ios = path.name.endswith('.ios.metal')
        sdk, standard = ('iphoneos', 'ios-metal2.4') if ios else ('macosx', 'macos-metal2.4')
        run(['xcrun', '-sdk', sdk, 'metal', '-std=' + standard,
             '-fmodules-cache-path=' + str(output / 'module-cache'), '-c', str(path), '-o', str(path) + '.air'], Path(str(path) + '.log'))

    with ThreadPoolExecutor(max_workers=min(jobs, 4)) as pool:
        list(pool.map(compile_one, metal))
    print(f'{len(metal)} native macOS/iOS Metal permutations compiled')
    return len(metal)


def validate_gpu(source: Path, output: Path, preview: Path | None = None,
                 sprite: Path | None = None, materials: Path | None = None,
                 regions: list[str] | None = None) -> str:
    metal = output / 'shaders/pbr_gb0_rt0_alpha1.frag.spv.macos.metal'
    code = metal.read_text()
    structs = code[code.index('struct ViewpointUBO'):code.index('struct LightBufferSSO')]
    # CPU fixture layouts come from the actual translator, including padding.
    structs = re.sub(r'\b(float4x4|float4|float3|float2)\b', r'simd_\1', structs)
    (output / 'sprite_gpu_structs.h').write_text('#include <simd/simd.h>\n' + structs)
    program = output / 'sprite_gpu_check'
    run(['xcrun', 'clang++', '-std=c++17', '-fobjc-arc', '-I' + str(output), str(source / 'sprite_gpu_check.mm'),
         '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'ImageIO', '-framework', 'CoreGraphics',
         '-o', str(program)], output / 'gpu-build.log')
    command = [str(program), str(metal),
               '--default-shader', str(output / 'shaders/default_gb0_rt0_alpha1.frag.spv.macos.metal'),
               '--gbuffer-shader', str(output / 'shaders/pbr_gb1_rt0_alpha1.frag.spv.macos.metal')]
    if preview:
        command += ['--preview-dir', str(preview)]
    if sprite:
        if not materials or not preview:
            raise RuntimeError('--sprite-path requires --materials-dir and --preview-dir')
        command += ['--sprite-path', str(sprite), '--materials-dir', str(materials)]
        for region in regions or []:
            command += ['--region', region]
    result = run(command, output / 'gpu-results.txt')
    print(result.strip())
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--spirv-cross-source', type=Path)
    parser.add_argument('--jobs', type=int, default=min(os.cpu_count() or 2, 8))
    parser.add_argument('--spirv-only', action='store_true')
    parser.add_argument('--skip-gpu', action='store_true')
    parser.add_argument('--preview-dir', type=Path)
    parser.add_argument('--gpu-only', action='store_true')
    parser.add_argument('--sprite-path', type=Path)
    parser.add_argument('--materials-dir', type=Path)
    parser.add_argument('--region', action='append', default=[])
    args = parser.parse_args()
    root = args.source_root.resolve()
    source = Path(__file__).resolve().parent
    output = args.output_dir.resolve() if args.output_dir else Path(tempfile.mkdtemp(prefix='gzdoom-sprite-shaders-'))
    output.mkdir(parents=True, exist_ok=True)
    cross = args.spirv_cross_source or root / 'build-ios/deps/SPIRV-Cross-vulkan-sdk-1.4.309.0'
    print('Validation evidence: ' + str(output), flush=True)
    try:
        layouts = validate_uniform_layouts(root)
        if args.gpu_only:
            if not args.output_dir or args.spirv_only or args.skip_gpu:
                raise RuntimeError('--gpu-only requires --output-dir and cannot be combined with --spirv-only/--skip-gpu')
            # Reuse only known-current source assembly; never silently render
            # an old shader after its source or uniform declarations changed.
            with tempfile.TemporaryDirectory(prefix='gzdoom-sprite-source-check-') as fresh:
                files = assemble_shaders(root, Path(fresh))
                for file in files:
                    previous = output / 'shaders' / file.name
                    if not previous.is_file() or previous.read_bytes() != file.read_bytes():
                        raise RuntimeError(f'Cached shader differs from current source: {file.name}; run full validation first')
            result = validate_gpu(source, output, args.preview_dir, args.sprite_path,
                                  args.materials_dir, args.region)
            report = {'source_root': str(root), 'uniform_layouts': layouts,
                      'reused_current_shader_permutations': len(files), 'gpu': result}
            filename = (args.sprite_path.stem + '-gpu-results.json') if args.sprite_path else 'gpu-recheck-results.json'
            (output / filename).write_text(json.dumps(report, indent=2) + '\n')
            return 0
        build = build_compilers(root, source, output, cross, max(args.jobs, 1), args.spirv_only)
        files = assemble_shaders(root, output)
        print(run([str(build / 'sprite_shader_check'), *map(str, files)], output / 'spirv-results.txt').strip())
        results = {'spirv_permutations': len(files), 'source_root': str(root), 'uniform_layouts': layouts, 'gpu': 'skipped'}
        if not args.spirv_only:
            results['native_metal_permutations'] = validate_metal(build, files, output, max(args.jobs, 1))
            if not args.skip_gpu:
                results['gpu'] = validate_gpu(source, output, args.preview_dir.resolve() if args.preview_dir else None,
                                              args.sprite_path, args.materials_dir, args.region)
                if args.preview_dir:
                    results['synthetic_preview_directory'] = str(args.preview_dir.resolve())
        (output / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    except (RuntimeError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
