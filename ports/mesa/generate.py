"""Run Mesa's own code generators into build/mesa/gen.

Each output lands in the directory Meson would use for it (the build
directory of the meson.build that declares it), so Mesa's quoted includes
resolve the same way. Outputs are rewritten only when their content changes,
so an unchanged generator run does not trigger recompilation.
"""
from pathlib import Path
import os
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
MESA = ROOT / 'third_party' / 'mesa'
SRC = MESA / 'src'
OUT = ROOT / 'build' / 'mesa' / 'gen'
SHA1 = '745709a000'

GL_ES_API = 'src/mesa/glapi/glapi/gen/gl_and_es_API.xml'
GL_API = 'src/mesa/glapi/glapi/gen/gl_API.xml'
GL_SYMBOLS = 'src/glx/libgl-symbols.txt'


def jobs():
    """(output, command, captured) with paths relative to the Mesa root.
    '@OUT' in a command stands for the output path."""
    glsl = 'src/compiler/glsl'
    nir = 'src/compiler/nir'
    spirv = 'src/compiler/spirv'
    gen = 'src/mesa/glapi/glapi/gen'
    items = [
        ('src/util/format_srgb.c', ['src/util/format_srgb.py'], True),
        ('src/util/shader_stats.h', ['src/util/process_shader_stats.py', 'src/util/shader_stats.rnc',
                                     'src/util/shader_stats.xml'], True),
        ('src/util/driconf_static.h', ['src/util/driconf_static.py', 'src/util/00-mesa-defaults.conf', '@OUT'], False),
        ('src/util/format/u_format_gen.h', ['src/util/format/u_format_table.py', 'src/util/format/u_format.yaml',
                                            '--enums'], True),
        ('src/util/format/u_format_pack.h', ['src/util/format/u_format_table.py', 'src/util/format/u_format.yaml',
                                             '--header'], True),
        ('src/util/format/u_format_table.c', ['src/util/format/u_format_table.py',
                                              'src/util/format/u_format.yaml'], True),
        ('src/compiler/builtin_types.h', ['src/compiler/builtin_types_h.py', '@OUT'], False),
        ('src/compiler/builtin_types.c', ['src/compiler/builtin_types_c.py', '@OUT'], False),
        ('src/compiler/ir_expression_operation.h', [glsl + '/ir_expression_operation.py', 'enum'], True),
        (glsl + '/ir_expression_operation_constant.h', [glsl + '/ir_expression_operation.py', 'constant'], True),
        (glsl + '/ir_expression_operation_strings.h', [glsl + '/ir_expression_operation.py', 'strings'], True),
        (nir + '/nir_builder_opcodes.h', [nir + '/nir_builder_opcodes_h.py'], True),
        (nir + '/nir_constant_expressions.c', [nir + '/nir_constant_expressions.py'], True),
        (nir + '/nir_opcodes.h', [nir + '/nir_opcodes_h.py'], True),
        (nir + '/nir_opcodes.c', [nir + '/nir_opcodes_c.py'], True),
        (nir + '/nir_opt_algebraic.c', [nir + '/nir_opt_algebraic.py', '--out', '@OUT'], False),
        (nir + '/nir_intrinsics.h', [nir + '/nir_intrinsics_h.py', '--out', '@OUT'], False),
        (nir + '/nir_intrinsics_indices.h', [nir + '/nir_intrinsics_indices_h.py', '--out', '@OUT'], False),
        (nir + '/nir_intrinsics.c', [nir + '/nir_intrinsics_c.py', '--out', '@OUT'], False),
        (spirv + '/vtn_gather_types.c', [spirv + '/vtn_gather_types_c.py', spirv + '/spirv.core.grammar.json',
                                         '@OUT'], False),
        (spirv + '/vtn_generator_ids.h', [spirv + '/vtn_generator_ids_h.py', spirv + '/spir-v.xml', '@OUT'], False),
        ('src/gallium/auxiliary/draw_nir_lower_opcodes.c', ['src/gallium/auxiliary/draw/draw_nir_lower_opcodes.py',
                                                            '-p', nir], True),
        ('src/gallium/auxiliary/u_tracepoints.c', ['src/gallium/auxiliary/util/u_tracepoints.py',
                                                   '-p', 'src/util/perf/', '-C', '@OUT'], False),
        ('src/gallium/auxiliary/u_tracepoints.h', ['src/gallium/auxiliary/util/u_tracepoints.py',
                                                   '-p', 'src/util/perf/', '-H', '@OUT'], False),
        ('src/gallium/auxiliary/u_indices_gen.c',['src/gallium/auxiliary/indices/u_indices_gen.py', '@OUT'], False),
        ('src/gallium/auxiliary/u_unfilled_gen.c', ['src/gallium/auxiliary/indices/u_unfilled_gen.py', '@OUT'], False),
        ('src/mesa/format_fallback.c', ['src/mesa/main/format_fallback.py', 'src/mesa/main/formats.csv', '@OUT'], False),
        ('src/mesa/get_hash.h', ['src/mesa/main/get_hash_generator.py', '-f', GL_ES_API], True),
        ('src/mesa/format_info.h', ['src/mesa/main/format_info.py', 'src/mesa/main/formats.csv'], True),
        # WebGL calls the OpenGL ES 3.x entry points libGLESv2 exports.
        ('src/mesa/glapi/es2api/es2_glapi_mapi_tmp.h', ['src/mesa/glapi/new/gen_gldispatch_mapi.py', 'glesv2',
                                                        'src/mesa/glapi/glapi/registry/gl.xml'], True),
        (gen + '/enums.c', [gen + '/gl_enums.py', '-f', 'src/mesa/glapi/glapi/registry/gl.xml'], True),
        (gen + '/api_exec_init.c', [gen + '/api_exec_init.py', '-f', GL_ES_API], True),
        (gen + '/api_exec_decl.h', [gen + '/api_exec_decl_h.py', '-f', GL_ES_API], True),
        (gen + '/api_save_init.h', [gen + '/api_save_init_h.py', '-f', GL_ES_API], True),
        (gen + '/api_save.h', [gen + '/api_save_h.py', '-f', GL_ES_API], True),
        (gen + '/api_beginend_init.h', [gen + '/api_beginend_init_h.py', '-f', GL_ES_API], True),
        (gen + '/api_hw_select_init.h', [gen + '/api_hw_select_init_h.py', '-f', GL_API], True),
        (gen + '/dispatch.h', [gen + '/gl_table.py', '-f', GL_ES_API, '-m', 'dispatch'], True),
        (gen + '/marshal_generated.h', [gen + '/marshal_generated_h.py', GL_ES_API, '8'], True),
        (gen + '/unmarshal_table.c', [gen + '/unmarshal_table_c.py', GL_ES_API, '8'], True),
        (gen + '/api_trace.c', [gen + '/api_trace_c.py', '-f', GL_ES_API], True),
        ('src/mesa/glapi/shared-glapi/shared_glapi_mapi_tmp.h', ['src/mesa/glapi/mapi_abi.py', '--printer',
                                                                 'shared-glapi', '--gl_symbols', GL_SYMBOLS,
                                                                 GL_ES_API], True),
    ]
    for part in range(8):
        items.append((f'{gen}/marshal_generated{part}.c',
                      [gen + '/marshal_generated_c.py', GL_ES_API, str(part), '8', '8'], True))
    for source, name, symbol in (('float64.glsl', 'float64_glsl.h', 'float64_source'),
                                 ('CrossPlatformSettings_piece_all.glsl', 'cross_platform_settings_piece_all.h',
                                  'cross_platform_settings_piece_all_header'),
                                 ('bc1.glsl', 'bc1_glsl.h', 'bc1_source'),
                                 ('bc4.glsl', 'bc4_glsl.h', 'bc4_source'),
                                 ('etc2_rgba_stitch.glsl', 'etc2_rgba_stitch_glsl.h', 'etc2_rgba_stitch_source'),
                                 ('astc_decoder.glsl', 'astc_glsl.h', 'astc_source')):
        items.append((f'{glsl}/{name}', ['src/util/xxd.py', f'{glsl}/{source}', '@OUT', '-n', symbol], False))
    return items


def store(path, data):
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + '.tmp')
    temporary.write_bytes(data)
    os.replace(temporary, path)


def main():
    env = dict(os.environ)
    modules = str(ROOT / 'third_party' / 'python-build')
    env['PYTHONPATH'] = modules + (os.pathsep + env['PYTHONPATH'] if env.get('PYTHONPATH') else '')
    env['PYTHONDONTWRITEBYTECODE'] = '1'
    store(OUT / 'src' / 'git_sha1.h', f'#define MESA_GIT_SHA1 " (git-{SHA1})"\n'.encode())
    scratch = OUT / '.scratch'
    scratch.mkdir(parents=True, exist_ok=True)
    # Generators that write a header and a source in one run: @H and @C.
    spirv = 'src/compiler/spirv'
    aux = 'src/gallium/auxiliary'
    pairs = (
        (spirv, 'spirv_info', [spirv + '/spirv_info_gen.py', '--json', spirv + '/spirv.core.grammar.json',
                               '--out-h', '@H', '--out-c', '@C']),
        (aux, 'tr_util', [aux + '/driver_trace/enums2names.py', 'src/gallium/include/pipe/p_defines.h',
                          'src/gallium/include/pipe/p_video_enums.h', 'src/util/blend.h', '-C', '@C', '-H', '@H']),
    )
    for directory, name, command in pairs:
        pair = {'@H': scratch / (name + '.h'), '@C': scratch / (name + '.c')}
        args = [str(pair[arg]) if arg in pair else arg for arg in command]
        if subprocess.run([sys.executable, *args], cwd=MESA, env=env).returncode:
            raise SystemExit(f'Mesa generator failed: {name}')
        for written in pair.values():
            store(OUT / directory / written.name, written.read_bytes())
            written.unlink()
    for output, command, captured in jobs():
        target = OUT / output
        written = scratch / Path(output).name
        args = [str(written) if arg == '@OUT' else arg for arg in command]
        result = subprocess.run([sys.executable, *args], cwd=MESA, env=env,
                                stdout=subprocess.PIPE if captured else None)
        if result.returncode:
            raise SystemExit(f'Mesa generator failed: {output}')
        store(target, result.stdout if captured else written.read_bytes())
        if not captured:
            written.unlink()


if __name__ == '__main__':
    main()
