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
        ('src/gallium/drivers/zink/zink_nir_algebraic.c', ['src/gallium/drivers/zink/nir_to_spirv/'
                                                           'zink_nir_algebraic.py', '-p', nir], True),
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


VK_XML = 'src/vulkan/registry/vk.xml'
# Classes of src/nouveau/headers/meson.build (nv_classes), in the same order: a class parsed after
# another one of the same engine (same last two digits) is given that one as --prev-in-h.
NV_CLASSES = '''cl85b5 cl902d cl9039 cl906f cl9097 cl90b5 cl90c0 cl91c0 cl9297 cla040 cla0b5 cla06f cla097 cla140
cla16f cla1c0 cla197 cla26f cla297 clb06f clb097 clb0b5 clb0c0 clb197 clb1c0 clc06f clc097 clc197 clc1b5 clc1c0
cla0c0 clc0b5 clc0c0 clc36f clc397 clc3b5 clc3c0 clc46f clc56f clc597 clc5b0 clc5b5 clc5c0 clc697 clc6b5 clc6c0
clc797 clc7b0 clc7b5 clc7c0 clc86f clc8b5 clc96f clc997 clc9b0 clc9b5 clc9c0 clca6f clcab5 clcb97 clcbc0 clcd97
clcdc0 clcec0 clce97'''.split()


NV_HWREF_GENS = ('gm107', 'gp100', 'gh100', 'tu102')
NAK_LATENCIES = {'sm100': ('pred_raw', 'pred_war', 'pred_waw', 'reg_raw', 'reg_war', 'reg_waw', 'upred_raw',
                           'upred_war', 'upred_waw', 'ureg_raw', 'ureg_war', 'ureg_waw')}


def multi_jobs():
    """Generators that write several files or name their output themselves: (directory, command,
    outputs). Every output name in `outputs` is a token ('@C', '@H', ...) in the command; the files
    are written to the directory Meson would use. The Vulkan runtime, Zink and NVK use the same XML
    and beta setting as Mesa's Vulkan drivers (--beta false: no provisional extensions)."""
    util, runtime, nv = 'src/vulkan/util', 'src/vulkan/runtime', 'src/nouveau'
    spirv, aux, zink = 'src/compiler/spirv', 'src/gallium/auxiliary', 'src/gallium/drivers/zink'
    beta = ['--beta', 'false']
    xml = ['--xml', VK_XML]
    items = [
        (spirv, [spirv + '/spirv_info_gen.py', '--json', spirv + '/spirv.core.grammar.json',
                 '--out-h', '@H', '--out-c', '@C'],
         {'@C': 'spirv_info.c', '@H': 'spirv_info.h'}),
        (aux, [aux + '/driver_trace/enums2names.py', 'src/gallium/include/pipe/p_defines.h',
               'src/gallium/include/pipe/p_video_enums.h', 'src/util/blend.h', '-C', '@C', '-H', '@H'],
         {'@C': 'tr_util.c', '@H': 'tr_util.h'}),
        ('src/vulkan/wsi', [util + '/vk_entrypoints_gen.py', *xml, '--proto', '--weak', '--out-h', '@H',
                            '--out-c', '@C', '--prefix', 'wsi', *beta],
         {'@C': 'wsi_common_entrypoints.c', '@H': 'wsi_common_entrypoints.h'}),
        (zink, [zink + '/zink_device_info.py', '@H', '@C', VK_XML],
         {'@C': 'zink_device_info.c', '@H': 'zink_device_info.h'}),
        (zink, [zink + '/zink_instance.py', '@H', '@C', VK_XML],
         {'@C': 'zink_instance.c', '@H': 'zink_instance.h'}),
        (util, [util + '/vk_dispatch_table_gen.py', *xml, '--out-c', '@C', '--out-h', '@H', *beta],
         {'@C': 'vk_dispatch_table.c', '@H': 'vk_dispatch_table.h'}),
        (util, [util + '/gen_enum_to_str.py', *xml, '--out-c', '@C', '--out-h', '@H', '--out-d', '@D', *beta],
         {'@C': 'vk_enum_to_str.c', '@H': 'vk_enum_to_str.h', '@D': 'vk_enum_defines.h'}),
        (util, [util + '/vk_struct_type_cast_gen.py', *xml, '--out', '@H', *beta],
         {'@H': 'vk_struct_type_cast.h'}),
        (util, [util + '/vk_extensions_gen.py', *xml, '--out-c', '@C', '--out-h', '@H'],
         {'@C': 'vk_extensions.c', '@H': 'vk_extensions.h'}),
        (runtime, [util + '/vk_entrypoints_gen.py', *xml, '--proto', '--weak', '--out-h', '@H', '--out-c', '@C',
                   '--prefix', 'vk_common', *beta],
         {'@C': 'vk_common_entrypoints.c', '@H': 'vk_common_entrypoints.h'}),
        (runtime, [util + '/vk_cmd_queue_gen.py', *xml, '--out-c', '@C', '--out-h', '@H', *beta],
         {'@C': 'vk_cmd_queue.c', '@H': 'vk_cmd_queue.h'}),
        (runtime, [util + '/vk_entrypoints_gen.py', *xml, '--proto', '--weak', '--out-h', '@H', '--out-c', '@C',
                   '--prefix', 'vk_cmd_enqueue', '--prefix', 'vk_cmd_enqueue_unless_primary', *beta],
         {'@C': 'vk_cmd_enqueue_entrypoints.c', '@H': 'vk_cmd_enqueue_entrypoints.h'}),
        (runtime, [util + '/vk_dispatch_trampolines_gen.py', *xml, '--out-c', '@C', '--out-h', '@H', *beta],
         {'@C': 'vk_dispatch_trampolines.c', '@H': 'vk_dispatch_trampolines.h'}),
        (runtime, [util + '/vk_physical_device_features_gen.py', *xml, '--out-c', '@C', '--out-h', '@H', *beta],
         {'@C': 'vk_physical_device_features.c', '@H': 'vk_physical_device_features.h'}),
        (runtime, [util + '/vk_physical_device_properties_gen.py', *xml, '--out-c', '@C', '--out-h', '@H', *beta],
         {'@C': 'vk_physical_device_properties.c', '@H': 'vk_physical_device_properties.h'}),
        (runtime, [util + '/vk_physical_device_spirv_caps_gen.py', *xml, '--out-c', '@C', *beta],
         {'@C': 'vk_physical_device_spirv_caps.c'}),
        (runtime, [util + '/vk_synchronization_helpers_gen.py', *xml, '--out-c', '@C', *beta],
         {'@C': 'vk_synchronization_helpers.c'}),
        (runtime, [runtime + '/vk_format_info_gen.py', *xml, '--out-c', '@C', '--out-h', '@H'],
         {'@C': 'vk_format_info.c', '@H': 'vk_format_info.h'}),
        (nv + '/vulkan', [util + '/vk_entrypoints_gen.py', *xml, '--proto', '--weak', '--out-h', '@H',
                          '--out-c', '@C', '--prefix', 'nvk', *beta],
         {'@C': 'nvk_entrypoints.c', '@H': 'nvk_entrypoints.h'}),
        (nv + '/vulkan', [nv + '/vulkan/nvk_drirc_gen.py', '--import-path', 'src/util', '--drirc-src', '@C',
                          '--drirc-hdr', '@H', '--validate', nv + '/vulkan/00-nvk-defaults.conf'],
         {'@C': 'nvk_drirc.c', '@H': 'nvk_drirc.h'}),
        (nv + '/nil', [nv + '/nil/nil_format_table_gen.py', '--csv', nv + '/nil/nil_formats.csv', '--out-h', '@H',
                       '--out-c', '@C'],
         {'@C': 'nil_format_table.c', '@H': 'nil_format_table.h'}),
        (nv + '/compiler', [nv + '/compiler/nak_nir_algebraic.py', '-p', 'src/compiler/nir', '--out', '@C'],
         {'@C': 'nak_nir_algebraic.c'}),
        (nv + '/headers', [nv + '/headers/nv_push_class_dump_h.py', '--classes', ' '.join(NV_CLASSES) + ' ',
                           '--out-h', '@H'],
         {'@H': 'nv_push_class_dump.h'}),
    ]
    # nvidia_headers: the C push helpers and, for NAK, nil and NVK's Rust, the same classes as Rust
    # modules. lib.rs also names the dev_mmu.h modules that nvk.mk makes with bindgen.
    headers, classes = nv + '/headers', nv + '/headers/nvidia/classes'
    modules = [f'nvh_hwref_{gen}_mmu.rs' for gen in NV_HWREF_GENS]
    for index, cl in enumerate(NV_CLASSES):
        previous = [c for c in NV_CLASSES[:index] if c[-2:] == cl[-2:]]
        parse = [headers + '/class_parser.py', '--in-h', f'{classes}/{cl}.h']
        if previous:
            parse += ['--prev-in-h', f'{classes}/{previous[-1]}.h']
        items.append((headers, parse + ['--out-h', '@H', '--out-c', '@C'],
                      {'@C': f'nv_push_{cl}.c', '@H': f'nv_push_{cl}.h'}))
        items.append((headers, [headers + '/class_parser.py', '--in-h', f'{classes}/{cl}.h', '--out-rs', '@R'],
                      {'@R': f'nvh_classes_{cl}.rs'}))
        items.append((headers, parse + ['--out-rs-mthd', '@R'], {'@R': f'nvh_classes_{cl}_mthd.rs'}))
        modules += [f'nvh_classes_{cl}.rs', f'nvh_classes_{cl}_mthd.rs']
        for kind, wanted in (('qmd', cl.endswith('c0')), ('tex', cl.endswith('97')), ('sph', True)):
            if wanted and (SRC.parent / classes / f'{cl}{kind}.h').is_file():
                items.append((headers, [headers + '/struct_parser.py', '--in-h', f'{classes}/{cl}{kind}.h',
                                        '--out-rs', '@R'], {'@R': f'nvh_classes_{cl}_{kind}.rs'}))
                modules.append(f'nvh_classes_{cl}_{kind}.rs')
    items.append((headers, [headers + '/lib_rs_gen.py', '--out-rs', '@R', *modules], {'@R': 'lib.rs'}))
    # NAK's instruction latency tables.
    latencies = nv + '/compiler/latencies'
    for sm, tables in NAK_LATENCIES.items():
        items.append((latencies, [latencies + '/lat_rs_gen.py', *(f'{latencies}/{sm}/{t}.csv' for t in tables),
                                  '-p', headers, '--out-rs', '@R', '--sm', sm], {'@R': f'{sm}.rs'}))
    items.append((latencies, [latencies + '/lib_rs_gen.py', '-p', headers, '--out-rs', '@R', *NAK_LATENCIES],
                  {'@R': 'lib.rs'}))
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
    # The Rust generators pipe their output through rustfmt as text: UTF-8, not the host's code page.
    env['PYTHONUTF8'] = '1'
    store(OUT / 'src' / 'git_sha1.h', f'#define MESA_GIT_SHA1 " (git-{SHA1})"\n'.encode())
    scratch = OUT / '.scratch'
    scratch.mkdir(parents=True, exist_ok=True)
    for directory, command, outputs in multi_jobs():
        paths = {token: scratch / name for token, name in outputs.items()}
        args = [str(paths[arg]) if arg in paths else arg for arg in command]
        if subprocess.run([sys.executable, *args], cwd=MESA, env=env).returncode:
            raise SystemExit(f'Mesa generator failed: {directory}/{list(outputs.values())[0]}')
        for token, name in outputs.items():
            store(OUT / directory / name, paths[token].read_bytes())
            paths[token].unlink()
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
    if sys.argv[1:] == ['nv-classes']:
        # nvk.mk compiles one nv_push_<class>.c per class
        print(' '.join(NV_CLASSES))
    else:
        main()
