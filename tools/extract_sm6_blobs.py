#!/usr/bin/env python3
# Extract the SM 6.0 / SM 5.0 shader twins used by tests/src/dx12_dxil.c from vkd3d-proton's tests (data only).
# Usage: python3 -I tools/extract_sm6_blobs.py third_party/vkd3d-proton-src/*/tests > tests/shaders/dx12_sm6_blobs.h
import re, sys
tests = sys.argv[1]
def arr(text, start_pat):
    i = text.index(start_pat); j = text.index('{', i); k = text.index('};', j)
    body = re.sub(r'#if 0.*?#endif', '', text[j+1:k], flags=re.S)
    return body.strip().rstrip(',')
out = ['// SM 6.0 (DXIL) and SM 5.0 (DXBC) twins of the same HLSL, extracted verbatim from vkd3d-proton v2.14.1 tests',
       '// (https://github.com/HansKristian-Work/vkd3d-proton, LGPL-2.1; blobs compiled by its authors with Microsoft DXC / fxc).',
       '// Regenerate: python3 -I tools/extract_sm6_blobs.py third_party/vkd3d-proton-src/*/tests', '#include <stdint.h>', '']
u = open(f'{tests}/d3d12_test_utils.h').read()
a = u.index('static inline void init_pipeline_state_desc(D3D12_GRAPHICS'); b = u.index('static inline void init_pipeline_state_desc_dxil(')
out.append('// void main(uint id : SV_VertexID, out float4 position : SV_Position) { float2 coords = float2((id << 1) & 2, id & 2);')
out.append('//     position = float4(coords * float2(2, -2) + float2(-1, 1), 0, 1); }   (full-screen triangle)')
out.append('static const uint32_t sm6_vs_fullscreen_dxbc[] = {' + arr(u[a:b], 'vs_code[]') + '};')
out.append('static const uint8_t sm6_vs_fullscreen_dxil[] = {' + arr(u[b:], 'vs_code[]') + '};')
for name, path, src in [('ps_render_a8', 'shaders/render_target/headers/ps_render_a8.h', 'shaders/render_target/ps_render_a8.ps_5_0.ps_6_0.hlsl'),
                        ('vs_passthrough', 'shaders/command/headers/conservative_rasterization_vs.h', 'shaders/command/conservative_rasterization_vs.vs_5_0.vs_6_0.hlsl'),
                        ('ps_atoc', 'shaders/pso/headers/ps_atoc.h', 'shaders/pso/ps_atoc.ps_5_0.ps_6_0.hlsl')]:
    t = open(f'{tests}/{path}').read()
    out.append('')
    out += ['// ' + l for l in open(f'{tests}/{src}').read().strip().splitlines()]
    out.append(f'static const uint32_t sm6_{name}_dxbc[] = {{' + arr(t, '_code_dxbc[]') + '};')
    out.append(f'static const uint8_t sm6_{name}_dxil[] = {{' + arr(t, '_code_dxil[]') + '};')
print('\n'.join(out))
