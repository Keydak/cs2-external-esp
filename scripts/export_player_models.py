"""
Exports the models of the previews in the menu: Number K for T & the default SAS for CT, standing with
the knife idle, and the planted bomb. Plain meshes, the menu shades them in one color.

Needs the command line version of Source2Viewer (https://github.com/ValveResourceFormat/ValveResourceFormat/releases, cli-windows-x64.zip)

    pip install numpy
    python scripts/export_player_models.py path/to/Source2Viewer-CLI.exe --out x64/Release/models

The meshes are simplified (few triangles, the menu draws them on the CPU) and written as models/<name>.mesh,
embedded into src/assets/models/PlayerModels.cpp too:
"CS2M", version 1, vertex count, triangle count, float32 positions (y up), uint16 (or uint32) indices.
"""
import argparse
import json
import os
import struct
import subprocess
import tempfile

import numpy as np

MODELS = {
    't': 'agents/models/tm_professional/tm_professional_vari.vmdl_c',   # Number K
    'ct': 'agents/models/ctm_sas/ctm_sas.vmdl_c',                       # Default CT
}
ANIMATION = 'idle_knife'
MESHES = ['thirdperson_body', 'thirdperson_default_gloves']
CELL = 0.024 # Simplification grid, meters

# Models without a pose, the planted bomb of the bomb preview
STATIC_MODELS = {
    'c4': ('weapons/models/c4/weapon_c4.vmdl_c', ['body_hd'], 0.0035),
}


def find_game():
    for root in [r'C:\Program Files (x86)\Steam\steamapps', r'D:\Games\steamapps', r'D:\SteamLibrary\steamapps', r'E:\SteamLibrary\steamapps']:
        path = os.path.join(root, 'common', 'Counter-Strike Global Offensive', 'game', 'csgo')
        if os.path.exists(os.path.join(path, 'pak01_dir.vpk')):
            return path
    return None


def export(cli, game, model, out, animation=None):
    """glTF, its buffers next to it"""
    command = [cli, '-i', os.path.join(game, 'pak01_dir.vpk'), '-f', model, '-o', out, '-d',
               '--gltf_export_format', 'gltf', '--game', os.path.join(game, 'gameinfo.gi')]
    if animation:
        command += ['--gltf_export_animations', '--gltf_animation_list', animation]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# glTF reading

COMP = {5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}
NC = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


class Gltf:
    def __init__(self, path):
        self.js = json.load(open(path))
        base = os.path.dirname(path)
        self.buffers = [open(os.path.join(base, b['uri']), 'rb').read() for b in self.js['buffers']]

    def accessor(self, i):
        a = self.js['accessors'][i]; bv = self.js['bufferViews'][a['bufferView']]
        data = self.buffers[bv['buffer']]
        offset = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        n = NC[a['type']]; dt = COMP[a['componentType']]
        stride = bv.get('byteStride', 0); itemsize = np.dtype(dt).itemsize * n
        if stride and stride != itemsize:
            raw = np.frombuffer(data, np.uint8, count=stride * a['count'], offset=offset).reshape(a['count'], stride)[:, :itemsize]
            out = np.frombuffer(raw.tobytes(), dt).reshape(a['count'], n)
        else:
            out = np.frombuffer(data, dt, count=a['count'] * n, offset=offset).reshape(a['count'], n)
        if a.get('normalized') and dt != np.float32:
            out = out.astype(np.float64) / np.iinfo(dt).max
        return out


def quat_to_mat(q):
    x, y, z, w = q
    return np.array([
        [1 - 2*(y*y + z*z), 2*(x*y - z*w), 2*(x*z + y*w)],
        [2*(x*y + z*w), 1 - 2*(x*x + z*z), 2*(y*z - x*w)],
        [2*(x*z - y*w), 2*(y*z + x*w), 1 - 2*(x*x + y*y)]])

def trs(t, r, s):
    m = np.eye(4)
    m[:3, :3] = quat_to_mat(r) * np.array(s)
    m[:3, 3] = t
    return m


def node_worlds(g, animation=None):
    """World matrix of every node, posed with the first frame of the animation"""
    nodes = g.js['nodes']
    local = [[np.array(n.get('translation', [0, 0, 0]), float), np.array(n.get('rotation', [0, 0, 0, 1]), float),
              np.array(n.get('scale', [1, 1, 1]), float)] for n in nodes]

    if animation:
        anim = next(a for a in g.js.get('animations', []) if a['name'].endswith('/' + animation) and '/world/' in a['name'])
        for ch in anim['channels']:
            path = {'translation': 0, 'rotation': 1, 'scale': 2}.get(ch['target']['path'])
            if path is not None:
                local[ch['target']['node']][path] = g.accessor(anim['samplers'][ch['sampler']]['output'])[0].astype(float)

    local_m = [np.array(n['matrix']).reshape(4, 4).T if 'matrix' in n else trs(*l) for n, l in zip(nodes, local)]
    parent = {c: i for i, n in enumerate(nodes) for c in n.get('children', [])}

    world = [None] * len(nodes)
    def get(i):
        if world[i] is None:
            world[i] = local_m[i] if i not in parent else get(parent[i]) @ local_m[i]
        return world[i]
    return [get(i) for i in range(len(nodes))]


def mesh(g, worlds, mesh_filter):
    """Positions & triangles of the matching meshes. Skinned meshes follow the bones, the others their node"""
    positions, triangles, count = [], [], 0

    for i, node in enumerate(g.js['nodes']):
        if 'mesh' not in node or not any(f in node.get('name', '') for f in mesh_filter):
            continue

        joints = None
        if 'skin' in node:
            skin = g.js['skins'][node['skin']]
            inverse = g.accessor(skin['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
            joints = np.array([worlds[j] @ inverse[k] for k, j in enumerate(skin['joints'])])

        for primitive in g.js['meshes'][node['mesh']]['primitives']:
            attributes = primitive['attributes']
            pos = g.accessor(attributes['POSITION']).astype(np.float64)
            homo = np.c_[pos, np.ones(len(pos))]

            if joints is not None:
                jw = g.accessor(attributes['JOINTS_0']).astype(np.int64)
                ww = g.accessor(attributes['WEIGHTS_0']).astype(np.float64)
                ww = ww / np.maximum(ww.sum(1, keepdims=True), 1e-9)
                out = np.zeros((len(pos), 3))
                for k in range(4):
                    out += ww[:, k:k+1] * np.einsum('nij,nj->ni', joints[jw[:, k]], homo)[:, :3]
            else:
                out = (homo @ worlds[i].T)[:, :3]

            idx = g.accessor(primitive['indices']).reshape(-1, 3).astype(np.int64)
            positions.append(out); triangles.append(idx + count)
            count += len(pos)

    return np.vstack(positions), np.vstack(triangles)


def decimate(pos, tris, cell):
    # Vertex clustering: vertices in the same cell become one
    keys = np.floor(pos / cell).astype(np.int64)
    _, cluster, counts = np.unique(keys, axis=0, return_inverse=True, return_counts=True)
    cluster = cluster.reshape(-1)
    new_pos = np.zeros((cluster.max() + 1, 3)); np.add.at(new_pos, cluster, pos); new_pos /= counts[:, None]
    t = cluster[tris]
    keep = (t[:, 0] != t[:, 1]) & (t[:, 1] != t[:, 2]) & (t[:, 0] != t[:, 2])
    t = np.unique(np.sort(t[keep], axis=1), axis=0, return_index=True)[1]
    t = cluster[tris][keep][np.sort(t)]
    used = np.unique(t)
    remap = -np.ones(len(new_pos), np.int64); remap[used] = np.arange(len(used))
    return new_pos[used], remap[t]


def write(path, pos, tris):
    with open(path, 'wb') as f:
        f.write(b'CS2M'); f.write(struct.pack('<III', 1, len(pos), len(tris)))
        f.write(pos.astype(np.float32).tobytes())
        f.write(tris.astype(np.uint16 if len(pos) < 65536 else np.uint32).tobytes())

def write_header(path, files):
    """Embeds the meshes into the program, used when models/<name>.mesh is not next to it.
    Declared in the header, the data in a .cpp next to it so only that one file is slow to compile"""
    os.makedirs(os.path.dirname(path), exist_ok=True)

    header = ['#pragma once', '', '// Made by scripts/export_player_models.py, the preview models (CS2M meshes)', '']
    for name in files:
        header.append(f'extern const unsigned char player_model_{name}[];')
        header.append(f'extern const size_t player_model_{name}_size;')
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(header) + '\n')

    source = ['// Made by scripts/export_player_models.py', f'#include "{os.path.basename(path)}"', '']
    for name, data in files.items():
        source.append(f'extern const unsigned char player_model_{name}[] = {{')
        for i in range(0, len(data), 24):
            source.append('    ' + ', '.join(f'0x{b:02X}' for b in data[i:i + 24]) + ',')
        source.append('};')
        source.append(f'const size_t player_model_{name}_size = sizeof(player_model_{name});')
        source.append('')
    with open(os.path.splitext(path)[0] + '.cpp', 'w', newline='\n') as f:
        f.write('\n'.join(source) + '\n')

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('cli', help='Source2Viewer-CLI.exe')
    parser.add_argument('--game', default=find_game(), help='game/csgo folder of CS2')
    parser.add_argument('--out', default='models')
    parser.add_argument('--header', default=os.path.join(os.path.dirname(__file__), '..', 'src', 'assets', 'models', 'PlayerModels.h'),
                        help='C++ header the meshes are embedded with, the data goes into the .cpp next to it')
    parser.add_argument('--header-only', action='store_true', help='only embeds the .mesh files already in --out')
    args = parser.parse_args()

    names = list(MODELS) + list(STATIC_MODELS)
    embedded = lambda: {name: open(os.path.join(args.out, name + '.mesh'), 'rb').read() for name in names}

    if args.header_only:
        write_header(args.header, embedded())
        return

    if not args.game:
        parser.error('CS2 not found, pass --game path/to/game/csgo')

    os.makedirs(args.out, exist_ok=True)

    with tempfile.TemporaryDirectory() as temp:
        for name, model in MODELS.items():
            path = os.path.join(temp, name, name + '.gltf')
            export(args.cli, args.game, model, path, ANIMATION)

            g = Gltf(path)
            pos, tris = decimate(*mesh(g, node_worlds(g, ANIMATION), MESHES), CELL)
            write(os.path.join(args.out, name + '.mesh'), pos, tris)
            print(f'{name}: {len(pos)} vertices, {len(tris)} triangles')

        for name, (model, meshes, cell) in STATIC_MODELS.items():
            path = os.path.join(temp, name, name + '.gltf')
            export(args.cli, args.game, model, path)

            g = Gltf(path)
            pos, tris = mesh(g, node_worlds(g), meshes)
            pos[:, 1] -= pos[:, 1].min()    # Resting on the ground
            pos, tris = decimate(pos, tris, cell)
            write(os.path.join(args.out, name + '.mesh'), pos, tris)
            print(f'{name}: {len(pos)} vertices, {len(tris)} triangles')

    write_header(args.header, embedded())


if __name__ == '__main__':
    main()
