"""
Pre-renders the player of the ESP preview in the menu: Number K for T & the default SAS for CT, each
holding their rifle, with their textures & a smooth light. Drawn from FRAMES angles around, the menu only
shows the image of the angle it is turned to, as cheap as an icon.

Needs the command line version of Source2Viewer (https://github.com/ValveResourceFormat/ValveResourceFormat/releases, cli-windows-x64.zip)

    pip install numpy pillow opencv-python
    python scripts/render_player_previews.py path/to/Source2Viewer-CLI.exe

Writes src/assets/models/PlayerPreviews.h & .cpp: per team a JPEG with every angle in a grid & a PNG
with how much of every pixel is covered (the JPEG has no transparency), plus the layout of the grid.
"""
import argparse
import json
import os
import subprocess
import tempfile

import cv2
import numpy as np
from PIL import Image

from export_player_models import Gltf, find_game, quat_to_mat, trs

TEAMS = {
    't': ('agents/models/tm_professional/tm_professional_vari.vmdl_c', 'idle_ak', 'weapons/models/ak47/weapon_rif_ak47.vmdl_c'),  # Number K
    'ct': ('agents/models/ctm_sas/ctm_sas.vmdl_c', 'idle_m4a4', 'weapons/models/m4a4/weapon_rif_m4a4.vmdl_c'),                     # Default CT
}
BASE_ANIMATION = 'idle_rifle'   # Full pose, the weapon idles are additive on top
MESHES = ['thirdperson_body', 'thirdperson_default_gloves']
WEAPON_MESHES = ['body_hd']

FRAMES = 12             # Angles around, 30 degrees apart
COLUMNS = 4             # Of the grid
FRAME_SIZE = (256, 320) # Pixels of an angle
PIXELS_PER_METER = 160.
FEET = 312              # Row of the feet in a frame
SUPERSAMPLE = 3         # Drawn bigger, then scaled down for smooth edges
BRIGHTNESS = 1.3        # The textures are dark for the lighting of the game
LIGHT = np.array([-0.35, 0.5, 0.8])  # Towards the light: the front, a bit from above & the left
JPEG_QUALITY = 90


def export(cli, game, model, out, animations=None):
    command = [cli, '-i', os.path.join(game, 'pak01_dir.vpk'), '-f', model, '-o', out, '-d',
               '--gltf_export_format', 'gltf', '--gltf_export_materials', '--game', os.path.join(game, 'gameinfo.gi')]
    if animations:
        command += ['--gltf_export_animations', '--gltf_animation_list', animations]
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def quat_mul(a, b):
    x1, y1, z1, w1 = a; x2, y2, z2, w2 = b
    return np.array([w1*x2 + x1*w2 + y1*z2 - z1*y2, w1*y2 - x1*z2 + y1*w2 + z1*x2,
                     w1*z2 + x1*y2 - y1*x2 + z1*w2, w1*w2 - x1*x2 - y1*y2 - z1*z2])


def node_worlds(g, base_animation=None, additive=None):
    """World matrix of every node: the first frame of base_animation, additive added on top of it"""
    nodes = g.js['nodes']
    local = [[np.array(n.get('translation', [0, 0, 0]), float), np.array(n.get('rotation', [0, 0, 0, 1]), float),
              np.array(n.get('scale', [1, 1, 1]), float)] for n in nodes]

    def first_frame(name):
        anim = next(a for a in g.js.get('animations', []) if a['name'].endswith('/' + name) and '/world/' in a['name'])
        return {(ch['target']['node'], ch['target']['path']): g.accessor(anim['samplers'][ch['sampler']]['output'])[0].astype(float)
                for ch in anim['channels']}

    if base_animation:
        for (node, path), value in first_frame(base_animation).items():
            local[node][{'translation': 0, 'rotation': 1, 'scale': 2}[path]] = value

    if additive:
        for (node, path), value in first_frame(additive).items():
            # The root of an additive animation is absolute, the base places the player. The weapon bones
            # too, the base holds the weapon where the hands are
            if nodes[node].get('name') in ('root_motion', 'pelvis', 'wpnPivot', 'wpn'):
                continue
            if path == 'translation':
                local[node][0] = local[node][0] + value
            elif path == 'rotation':
                local[node][1] = quat_mul(local[node][1], value)

    local_m = [np.array(n['matrix']).reshape(4, 4).T if 'matrix' in n else trs(*l) for n, l in zip(nodes, local)]
    parent = {c: i for i, n in enumerate(nodes) for c in n.get('children', [])}

    world = [None] * len(nodes)
    def get(i):
        if world[i] is None:
            world[i] = local_m[i] if i not in parent else get(parent[i]) @ local_m[i]
        return world[i]
    return [get(i) for i in range(len(nodes))]


class Textures:
    """Base color textures of the materials, float rgb"""
    def __init__(self):
        self.images = []
        self.keys = []

    def add(self, key, image):
        if key not in self.keys:
            self.keys.append(key)
            self.images.append(image)
        return self.keys.index(key)


def mesh(g, worlds, mesh_filter, textures):
    """Positions, texture coordinates, texture of every triangle & triangles of the matching meshes"""
    positions, uvs, triangles, materials, count = [], [], [], [], 0
    base = os.path.dirname(g.path)

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

            material = g.js['materials'][primitive['material']] if 'material' in primitive else {}
            pbr = material.get('pbrMetallicRoughness', {})
            factor = np.array(pbr.get('baseColorFactor', [1, 1, 1, 1])[:3])
            texture = pbr.get('baseColorTexture', {}).get('index')

            if texture is not None and 'TEXCOORD_0' in attributes:
                uri = g.js['images'][g.js['textures'][texture]['source']]['uri']
                image = np.asarray(Image.open(os.path.join(base, uri)).convert('RGB'), np.float32) / 255. * factor
                index = textures.add((g.path, uri), image.astype(np.float32))
                uv = g.accessor(attributes['TEXCOORD_0']).astype(np.float64)
            else:
                index = textures.add(tuple(factor), np.tile(factor, (2, 2, 1)).astype(np.float32))
                uv = np.full((len(pos), 2), 0.5)

            idx = g.accessor(primitive['indices']).reshape(-1, 3).astype(np.int64)
            positions.append(out); uvs.append(uv); triangles.append(idx + count)
            materials.append(np.full(len(idx), index))
            count += len(pos)

    return np.vstack(positions), np.vstack(uvs), np.vstack(triangles), np.concatenate(materials)


WEAPON_HAND_R = np.array([-2.6, -1.4, 0.]) * 0.0254  # Attachment "weapon_hand_r" of the agents on hand_R, meters


def place_weapon(pos, gun_worlds, gun_names, worlds, names):
    """Like the game: the bone "weapon" of the weapon onto the bone "wpn" of the player. The game then bends
    the right hand onto the grip, here the weapon moves the few centimeters onto the hand instead"""
    weapon = gun_worlds[gun_names.index('weapon')]
    m = worlds[names.index('wpn')] @ np.linalg.inv(weapon)

    grip = (m @ gun_worlds[gun_names.index('ag1_hand_r')])[:3, 3]
    hand = (worlds[names.index('hand_R')] @ np.r_[WEAPON_HAND_R, 1.])[:3]

    return pos @ m[:3, :3].T + m[:3, 3] + (hand - grip)


def smooth_normals(pos, tris):
    """Normals averaged over the faces around every place, vertices split at texture seams count as one"""
    _, place = np.unique(np.round(pos * 2000.).astype(np.int64), axis=0, return_inverse=True)
    place = place.reshape(-1)
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    face = np.cross(b - a, c - a)  # Weighted by the area

    sums = np.zeros((place.max() + 1, 3))
    for k in range(3):
        np.add.at(sums, place[tris[:, k]], face)

    # The winding is not the same everywhere, faces facing the other way would cancel out: a second pass
    # adds every face turned towards the first sum
    reference = sums.copy(); sums[:] = 0
    for k in range(3):
        sign = np.sign(np.einsum('ij,ij->i', reference[place[tris[:, k]]], face))
        sign[sign == 0] = 1
        np.add.at(sums, place[tris[:, k]], face * sign[:, None])

    normals = sums[place]
    return normals / np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-9)


def render(pos, uv, tris, materials, normals, textures, yaw):
    """One angle: rgb & coverage, straight alpha"""
    s = SUPERSAMPLE
    width, height = FRAME_SIZE[0] * s, FRAME_SIZE[1] * s
    ppm = PIXELS_PER_METER * s

    c, si = np.cos(yaw), np.sin(yaw)
    x = pos[:, 0] * c - pos[:, 2] * si
    depth = pos[:, 0] * si + pos[:, 2] * c   # Towards the camera
    screen = np.c_[width / 2. + x * ppm, FEET * s - pos[:, 1] * ppm]

    # Triangle ids, far ones first so the near ones cover them. Fine enough at this size, the triangles are small
    order = np.argsort(depth[tris].mean(1))
    ids = np.zeros((height, width, 3), np.uint8)
    shift = 4
    points = np.round(screen * (1 << shift)).astype(np.int32)

    for t in order:
        value = int(t) + 1
        cv2.fillConvexPoly(ids, points[tris[t]], (value & 255, (value >> 8) & 255, (value >> 16) & 255), lineType=cv2.LINE_8, shift=shift)

    id_map = ids[..., 0].astype(np.int64) | (ids[..., 1].astype(np.int64) << 8) | (ids[..., 2].astype(np.int64) << 16)
    covered = id_map > 0
    ys, xs = np.nonzero(covered)
    t = id_map[covered] - 1

    # Barycentric coordinates of every pixel in its triangle
    p = np.c_[xs + 0.5, ys + 0.5]
    a, b, cc = screen[tris[t, 0]], screen[tris[t, 1]], screen[tris[t, 2]]
    v0, v1, v2 = b - a, cc - a, p - a
    d00 = (v0 * v0).sum(1); d01 = (v0 * v1).sum(1); d11 = (v1 * v1).sum(1); d20 = (v2 * v0).sum(1); d21 = (v2 * v1).sum(1)
    denom = d00 * d11 - d01 * d01
    denom[np.abs(denom) < 1e-12] = 1e-12
    w1 = np.clip((d11 * d20 - d01 * d21) / denom, 0, 1)
    w2 = np.clip((d00 * d21 - d01 * d20) / denom, 0, 1)
    w0 = np.clip(1 - w1 - w2, 0, 1)

    def interpolate(values):
        return values[tris[t, 0]] * w0[:, None] + values[tris[t, 1]] * w1[:, None] + values[tris[t, 2]] * w2[:, None]

    # Light per pixel, from both sides
    n = interpolate(normals)
    n = np.c_[n[:, 0] * c - n[:, 2] * si, n[:, 1], n[:, 0] * si + n[:, 2] * c]
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
    light = LIGHT / np.linalg.norm(LIGHT)
    lit = 0.42 + 0.58 * np.abs(n @ light)

    # Texture, bilinear
    texcoords = np.clip(interpolate(uv), 0, 1)
    color = np.zeros((len(t), 3), np.float32)
    material = materials[t]
    for m in np.unique(material):
        sel = material == m
        image = textures.images[m]
        h, w, _ = image.shape
        u = texcoords[sel, 0] * (w - 1); v = texcoords[sel, 1] * (h - 1)
        u0 = np.floor(u).astype(int); v0_ = np.floor(v).astype(int)
        u1 = np.minimum(u0 + 1, w - 1); v1_ = np.minimum(v0_ + 1, h - 1)
        fu = (u - u0)[:, None]; fv = (v - v0_)[:, None]
        color[sel] = (image[v0_, u0] * (1 - fu) * (1 - fv) + image[v0_, u1] * fu * (1 - fv)
                      + image[v1_, u0] * (1 - fu) * fv + image[v1_, u1] * fu * fv)

    rgb = np.zeros((height, width, 3), np.float32)
    rgb[ys, xs] = np.clip(color * lit[:, None] * BRIGHTNESS, 0, 1)
    alpha = covered.astype(np.float32)

    # Scaled down with the color weighted by the coverage
    small = (FRAME_SIZE[0], FRAME_SIZE[1])
    premultiplied = cv2.resize(rgb * alpha[..., None], small, interpolation=cv2.INTER_AREA)
    alpha = cv2.resize(alpha, small, interpolation=cv2.INTER_AREA)
    rgb = premultiplied / np.maximum(alpha[..., None], 1e-6)
    return np.clip(rgb, 0, 1), alpha


def bleed(rgb, alpha):
    """Edge colors spread into the empty pixels, the JPEG then has no dark seam around the model"""
    rgb = rgb.copy()
    known = alpha > 0.01
    for _ in range(12):
        # Average of the known neighbours, a box blur over the known pixels
        total = cv2.blur(rgb * known[..., None], (3, 3))
        weight = cv2.blur(known.astype(np.float32), (3, 3))
        fill = (~known) & (weight > 0)
        rgb[fill] = total[fill] / weight[fill][:, None]
        known = known | fill
    return rgb


def write_sources(path, teams):
    """PlayerPreviews.h & .cpp: the images & the layout"""
    header = ['#pragma once', '', '// Made by scripts/render_player_previews.py, the pre-rendered player of the ESP preview', '',
              'namespace player_preview {',
              f'    constexpr int FRAMES = {FRAMES};          // Angles around, frame k is turned k * 360 / FRAMES degrees',
              f'    constexpr int COLUMNS = {COLUMNS};         // Of the grid',
              f'    constexpr int FRAME_WIDTH = {FRAME_SIZE[0]};',
              f'    constexpr int FRAME_HEIGHT = {FRAME_SIZE[1]};',
              f'    constexpr int FEET = {FEET};          // Row of the feet in a frame',
              '}', '']
    for name, info in teams.items():
        header.append(f'extern const unsigned char player_preview_{name}[];        // JPEG')
        header.append(f'extern const size_t player_preview_{name}_size;')
        header.append(f'extern const unsigned char player_preview_{name}_alpha[];  // PNG, gray')
        header.append(f'extern const size_t player_preview_{name}_alpha_size;')
        header.append(f'constexpr int player_preview_{name}_height = {info["height"]};  // Pixels from the feet to the top of the head')
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(header) + '\n')

    source = ['// Made by scripts/render_player_previews.py', f'#include "{os.path.basename(path)}"', '']
    for name, info in teams.items():
        for suffix, data in (('', info['jpeg']), ('_alpha', info['alpha'])):
            source.append(f'extern const unsigned char player_preview_{name}{suffix}[] = {{')
            for i in range(0, len(data), 24):
                source.append('    ' + ', '.join(f'0x{b:02X}' for b in data[i:i + 24]) + ',')
            source.append('};')
            source.append(f'const size_t player_preview_{name}{suffix}_size = sizeof(player_preview_{name}{suffix});')
            source.append('')
    with open(os.path.splitext(path)[0] + '.cpp', 'w', newline='\n') as f:
        f.write('\n'.join(source) + '\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('cli', help='Source2Viewer-CLI.exe')
    parser.add_argument('--game', default=find_game(), help='game/csgo folder of CS2')
    parser.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', 'src', 'assets', 'models', 'PlayerPreviews.h'))
    parser.add_argument('--images', help='also saves the images into this folder, to look at them')
    args = parser.parse_args()

    if not args.game:
        parser.error('CS2 not found, pass --game path/to/game/csgo')

    teams = {}
    with tempfile.TemporaryDirectory() as temp:
        for name, (model, weapon_idle, weapon) in TEAMS.items():
            agent_path = os.path.join(temp, name, name + '.gltf')
            weapon_path = os.path.join(temp, name + '_weapon', 'weapon.gltf')
            export(args.cli, args.game, model, agent_path, BASE_ANIMATION + ',' + weapon_idle)
            export(args.cli, args.game, weapon, weapon_path, 'dropped')  # With an animation for its bones

            textures = Textures()
            agent = Gltf(agent_path); agent.path = agent_path
            worlds = node_worlds(agent, BASE_ANIMATION, weapon_idle)
            body = mesh(agent, worlds, MESHES, textures)

            names = [n.get('name') for n in agent.js['nodes']]

            gun = Gltf(weapon_path); gun.path = weapon_path
            gun_worlds = node_worlds(gun)
            gun_names = [n.get('name') for n in gun.js['nodes']]
            gun_pos, gun_uv, gun_tris, gun_materials = mesh(gun, gun_worlds, WEAPON_MESHES, textures)
            gun_pos = place_weapon(gun_pos, gun_worlds, gun_names, worlds, names)

            pos = np.vstack([body[0], gun_pos])
            uv = np.vstack([body[1], gun_uv])
            tris = np.vstack([body[2], gun_tris + len(body[0])])
            materials = np.concatenate([body[3], gun_materials])

            # Turned around the middle of the player, feet at 0
            pos[:, 1] -= pos[:, 1].min()
            pos[:, 0] -= (body[0][:, 0].min() + body[0][:, 0].max()) / 2
            pos[:, 2] -= (body[0][:, 2].min() + body[0][:, 2].max()) / 2
            normals = smooth_normals(pos, tris)

            sheet_rgb = np.zeros((FRAME_SIZE[1] * ((FRAMES + COLUMNS - 1) // COLUMNS), FRAME_SIZE[0] * COLUMNS, 3), np.float32)
            sheet_alpha = np.zeros(sheet_rgb.shape[:2], np.float32)

            for frame in range(FRAMES):
                rgb, alpha = render(pos, uv, tris, materials, normals, textures, frame * 2 * np.pi / FRAMES)
                x0, y0 = (frame % COLUMNS) * FRAME_SIZE[0], (frame // COLUMNS) * FRAME_SIZE[1]
                sheet_rgb[y0:y0 + FRAME_SIZE[1], x0:x0 + FRAME_SIZE[0]] = bleed(rgb, alpha)
                sheet_alpha[y0:y0 + FRAME_SIZE[1], x0:x0 + FRAME_SIZE[0]] = alpha
                print(f'{name}: angle {frame + 1}/{FRAMES}')

            ok, jpeg = cv2.imencode('.jpg', cv2.cvtColor((sheet_rgb * 255).round().astype(np.uint8), cv2.COLOR_RGB2BGR), [cv2.IMWRITE_JPEG_QUALITY, JPEG_QUALITY])
            ok2, alpha_png = cv2.imencode('.png', (sheet_alpha * 255).round().astype(np.uint8), [cv2.IMWRITE_PNG_COMPRESSION, 9])
            assert ok and ok2

            height = int(round(pos[:, 1].max() * PIXELS_PER_METER))
            teams[name] = {'jpeg': jpeg.tobytes(), 'alpha': alpha_png.tobytes(), 'height': height}
            print(f'{name}: {len(tris)} triangles, {len(jpeg) // 1024} + {len(alpha_png) // 1024} KB')

            if args.images:
                os.makedirs(args.images, exist_ok=True)
                rgba = np.dstack([sheet_rgb, sheet_alpha])
                Image.fromarray((rgba * 255).round().astype(np.uint8), 'RGBA').save(os.path.join(args.images, name + '.png'))

    write_sources(args.out, teams)


if __name__ == '__main__':
    main()
