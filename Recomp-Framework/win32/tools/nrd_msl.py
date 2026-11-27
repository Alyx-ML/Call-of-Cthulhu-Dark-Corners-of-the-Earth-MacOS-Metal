#!/usr/bin/env python3
"""Make the Metal shaders of the NRD denoiser (runtime/d3d9_nrd.mm) at build time (win32/nrd.mk).

1. tools/nrd_dump (linked with the NRD library) writes the shader identifier of each REBLUR_DIFFUSE pipeline, in the
   order of the NRD instance ("fileName|macro1=value1|...").
2. Slang compiles each NRD shader to SPIR-V with the arguments of the NRD build (NRD CMakeLists.txt and ShaderMake:
   compute shader model 6.0, -O3, column-major matrices, the entry point names of the source, the macros of the
   identifier and NRD_INTERNAL, and the SPIR-V register shifts s 0, b 2, u 3, t 20).
3. SPIRV-Cross changes each SPIR-V module to Metal Shading Language. The SPIR-V binding numbers become the Metal
   argument indices (--msl-decoration-binding): the samplers are at sampler 0 and 1, the constant buffer at buffer 2,
   the storage textures at texture 3 and up, and the textures at texture 20 and up.
4. Slang and SPIRV-Cross compile the two kernels of runtime/nrd_glue.hlsl (nrd_pack, nrd_unpack) the same way, with
   no register shifts.
5. The output is a C header with one string of Metal source for each different shader identifier (the NRD instance
   can have more than one pipeline with the same identifier) and the shader of each pipeline.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile


def fail(msg):
    sys.exit('nrd_msl.py: ' + msg)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        fail('%s failed (exit %d):\n%s%s' % (os.path.basename(cmd[0]), r.returncode, r.stdout, r.stderr))
    return r


def local_size(path):
    """The LocalSize execution mode (OpExecutionMode, mode 17) of a SPIR-V module."""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 20 or len(data) % 4:
        fail(path + ' is not a SPIR-V module')
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    if words[0] != 0x07230203:
        fail(path + ' is not a little-endian SPIR-V module')
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xffff
        if count == 0 or i + count > len(words):
            break
        if op == 16 and count >= 6 and words[i + 2] == 17:
            return words[i + 3], words[i + 4], words[i + 5]
        i += count
    fail(path + ' has no LocalSize execution mode')


# the Slang arguments of an NRD shader (see the module documentation)
NRD_SHIFTS = ['-fvk-s-shift', '0', 'all', '-fvk-b-shift', '2', 'all', '-fvk-u-shift', '3', 'all',
              '-fvk-t-shift', '20', 'all']
MACRO = re.compile(r'^[A-Za-z_][A-Za-z_0-9]*=[A-Za-z_0-9]+$')


def slang(slangc, src, entry, includes, defines, shifts, spv):
    cmd = [slangc, src, '-lang', 'hlsl', '-entry', entry, '-stage', 'compute', '-profile', 'cs_6_0', '-target', 'spirv',
           '-O3', '-matrix-layout-column-major', '-fvk-use-entrypoint-name']
    for inc in includes:
        cmd += ['-I', inc]
    for d in defines:
        cmd += ['-D', d]
    run(cmd + shifts + ['-o', spv])


def to_msl(spirv_cross, spv, out, entry):
    # SPIRV-Cross names the kernel of the entry point main main0 (main is not a valid Metal function name)
    run([spirv_cross, spv, '--msl', '--msl-version', '30000', '--msl-decoration-binding', '--output', out])
    with open(out) as f:
        src = f.read()
    kernel = 'main0' if entry == 'main' else entry
    if ('kernel void %s(' % kernel) not in src:
        fail('%s has no kernel %s' % (out, kernel))
    return src


def c_string(src):
    lines = []
    for line in src.splitlines():
        esc = line.replace('\\', '\\\\').replace('"', '\\"')
        lines.append('    "%s\\n"' % esc)
    return '\n'.join(lines) if lines else '    ""'


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--dump', required=True)
    ap.add_argument('--slang', required=True, help='the Slang compiler (slangc)')
    ap.add_argument('--spirv-cross', required=True)
    ap.add_argument('--nrd', required=True, help='the NRD source folder')
    ap.add_argument('--mathlib', required=True, help='the MathLib source folder of the NRD build')
    ap.add_argument('--glue', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    for tool in (a.slang, a.spirv_cross):
        if not shutil.which(tool):
            fail("the command '%s' was not found" % tool)

    out_dir = os.path.dirname(os.path.abspath(a.out))
    with tempfile.TemporaryDirectory(dir=out_dir) as tmp:
        shaders = os.path.join(a.nrd, 'Shaders')
        includes = [shaders, a.mathlib]
        listing = run([a.dump]).stdout
        entries = []        # one entry for each different shader identifier
        shader_of = {}      # shader identifier -> index in entries
        pipes = []          # pipeline index -> index in entries
        for n, line in enumerate(listing.splitlines()):
            index, ident = line.split(' ', 1)
            if int(index) != n:
                fail('nrd_dump does not list the pipelines in order')
            if ident in shader_of:
                pipes.append(shader_of[ident])
                continue
            parts = ident.split('|')
            name, defines = parts[0], parts[1:]
            if os.path.basename(name) != name or not name.endswith('.hlsl'):
                fail('pipeline %s has the shader file %r' % (index, name))
            for d in defines:
                if not MACRO.match(d):
                    fail('pipeline %s has the macro %r' % (index, d))
            spv = os.path.join(tmp, index + '.spv')
            slang(a.slang, os.path.join(shaders, name), 'main', includes, ['NRD_INTERNAL'] + defines, NRD_SHIFTS, spv)
            tx, ty, tz = local_size(spv)
            src = to_msl(a.spirv_cross, spv, os.path.join(tmp, index + '.metal'), 'main')
            shader_of[ident] = len(entries)
            pipes.append(len(entries))
            entries.append((ident, src, tx, ty, tz))
        if not entries:
            fail('the NRD instance has no pipelines')

        glue = []
        for entry in ('nrd_pack', 'nrd_unpack'):
            spv = os.path.join(tmp, entry + '.spv')
            slang(a.slang, a.glue, entry, includes, [], [], spv)
            tx, ty, tz = local_size(spv)
            src = to_msl(a.spirv_cross, spv, os.path.join(tmp, entry + '.metal'), entry)
            glue.append((entry, src, tx, ty, tz))

    h = ['// Generated by Recomp-Framework/win32/tools/nrd_msl.py from the NRD sources; do not edit.',
         '// The Metal shaders of the NRD REBLUR_DIFFUSE pipelines and of runtime/nrd_glue.hlsl. nrd_msl has one shader for',
         '// each different NRD shader identifier; nrd_pipe gives the nrd_msl index of each pipeline, in the order of the',
         '// NRD instance. The kernel of an NRD shader is main0; a glue kernel has the name of its entry.',
         '#pragma once',
         '#include <stdint.h>',
         'typedef struct { const char *id; const char *msl; uint16_t tx, ty, tz; } NrdMsl;',
         'static const NrdMsl nrd_msl[] = {']
    for ident, src, tx, ty, tz in entries:
        h.append('  {"%s",\n%s,\n   %d, %d, %d},' % (ident, c_string(src), tx, ty, tz))
    h.append('};')
    h.append('#define NRD_MSL_COUNT %d' % len(entries))
    h.append('static const uint16_t nrd_pipe[] = {%s};' % ', '.join(str(p) for p in pipes))
    h.append('#define NRD_PIPE_COUNT %d' % len(pipes))
    for entry, src, tx, ty, tz in glue:
        h.append('static const NrdMsl %s_msl = {"%s",\n%s,\n   %d, %d, %d};' % (entry, entry, c_string(src), tx, ty, tz))
    tmp_out = a.out + '.tmp'
    with open(tmp_out, 'w') as f:
        f.write('\n'.join(h) + '\n')
    os.replace(tmp_out, a.out)
    print('nrd_msl.py: %d NRD pipelines (%d shaders) and 2 glue kernels -> %s' % (len(pipes), len(entries), a.out))


if __name__ == '__main__':
    main()
