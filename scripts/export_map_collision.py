"""
Exports the static collision of CS2 maps into small triangle files used by the grenade prediction.

    pip install zstandard
    python scripts/export_map_collision.py                  # every de_ map, written to ./maps
    python scripts/export_map_collision.py de_dust2 de_mirage --out x64/Release/maps

The program looks for maps/<map name>.tri next to its working directory.
Run it again after Valve updates a map.
"""
import argparse
import os
import re
import struct
import sys

from kv3 import KV3

VPK_SIGNATURE = 0x55AA1234
TRI_MAGIC = b'CS2T'
TRI_VERSION = 1

# Collision layers grenades fly through
IGNORED_LAYERS = {'playerclip', 'npcclip', 'sky'}


def find_maps_dir():
    candidates = []

    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as key:
            steam = winreg.QueryValueEx(key, 'SteamPath')[0]
        candidates.append(steam)

        library = os.path.join(steam, 'steamapps', 'libraryfolders.vdf')
        if os.path.exists(library):
            for path in re.findall(r'"path"\s+"([^"]+)"', open(library, encoding='utf-8').read()):
                candidates.append(path.replace('\\\\', '\\'))
    except OSError:
        pass

    for root in candidates:
        maps = os.path.join(root, 'steamapps', 'common', 'Counter-Strike Global Offensive', 'game', 'csgo', 'maps')
        if os.path.isdir(maps):
            return maps

    return None


def read_vpk_entry(path, wanted):
    """Returns the bytes of a single file stored inside a single-file vpk (maps are not split)."""
    with open(path, 'rb') as f:
        signature, version, tree_size = struct.unpack('<III', f.read(12))
        if signature != VPK_SIGNATURE:
            raise ValueError(f'{path} is not a vpk')

        header_size = 12
        if version == 2:
            f.read(16)
            header_size += 16

        def read_string():
            out = bytearray()
            while (c := f.read(1)) != b'\x00':
                out += c
            return out.decode('utf-8', 'replace')

        found = None
        while (extension := read_string()):
            while (directory := read_string()):
                while (name := read_string()):
                    _, preload, _, offset, length, _ = struct.unpack('<IHHIIH', f.read(18))
                    f.read(preload)
                    if f'{directory}/{name}.{extension}' == wanted:
                        found = (offset, length)

        if not found:
            return None

        f.seek(header_size + tree_size + found[0])
        return f.read(found[1])


def physics_block(resource):
    """Finds the PHYS block of a compiled Source 2 resource."""
    _, _, _, block_offset, block_count = struct.unpack_from('<IHHII', resource, 0)
    base = 8 + block_offset

    for i in range(block_count):
        entry = base + i * 12
        name = resource[entry:entry + 4]
        offset, size = struct.unpack_from('<II', resource, entry + 4)
        if name == b'PHYS':
            start = entry + 4 + offset
            return resource[start:start + size]

    return None


def hull_triangles(hull):
    """Fan-triangulates every face of a half-edge convex hull."""
    positions = hull['m_VertexPositions']
    vertices = [struct.unpack_from('<3f', positions, i) for i in range(0, len(positions), 12)]

    # Half edge: next, twin, origin, face (one byte each)
    edges = hull['m_Edges']
    faces = hull['m_Faces']

    triangles = []
    for start in faces:
        polygon = []
        edge = start
        for _ in range(len(edges) // 4):
            polygon.append(edges[edge * 4 + 2])
            edge = edges[edge * 4]
            if edge == start:
                break

        for i in range(1, len(polygon) - 1):
            triangles.append((vertices[polygon[0]], vertices[polygon[i]], vertices[polygon[i + 1]]))

    return triangles


def mesh_triangles(mesh):
    raw_vertices = mesh['m_Vertices']
    vertices = [struct.unpack_from('<3f', raw_vertices, i) for i in range(0, len(raw_vertices), 12)]

    raw_triangles = mesh['m_Triangles']
    return [
        tuple(vertices[index] for index in struct.unpack_from('<3i', raw_triangles, i))
        for i in range(0, len(raw_triangles), 12)
    ]


def blocks_grenades(attribute):
    layers = {layer.lower() for layer in attribute.get('m_InteractAsStrings', [])}
    return not (layers & IGNORED_LAYERS)


def export_map(maps_dir, map_name, out_dir):
    vpk = os.path.join(maps_dir, f'{map_name}.vpk')
    resource = read_vpk_entry(vpk, f'maps/{map_name}/world_physics.vmdl_c')
    if resource is None:
        print(f'  {map_name}: no world physics found, skipped')
        return False

    phys = physics_block(resource)
    if phys is None:
        print(f'  {map_name}: no PHYS block found, skipped')
        return False

    root = KV3(phys).root
    attributes = root['m_collisionAttributes']

    triangles = []
    skipped = 0

    for part in root['m_parts']:
        shape = part['m_rnShape']

        for hull in shape['m_hulls']:
            if blocks_grenades(attributes[hull['m_nCollisionAttributeIndex']]):
                triangles += hull_triangles(hull['m_Hull'])
            else:
                skipped += 1

        for mesh in shape['m_meshes']:
            if blocks_grenades(attributes[mesh['m_nCollisionAttributeIndex']]):
                triangles += mesh_triangles(mesh['m_Mesh'])
            else:
                skipped += 1

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f'{map_name}.tri')

    with open(path, 'wb') as f:
        f.write(TRI_MAGIC)
        f.write(struct.pack('<II', TRI_VERSION, len(triangles)))
        for triangle in triangles:
            f.write(struct.pack('<9f', *triangle[0], *triangle[1], *triangle[2]))

    print(f'  {map_name}: {len(triangles)} triangles ({skipped} non solid shapes skipped) -> {path}')
    return True


def main():
    parser = argparse.ArgumentParser(description='Exports CS2 map collision for the grenade prediction')
    parser.add_argument('maps', nargs='*', help='map names, e.g. de_dust2 (default: every de_ map)')
    parser.add_argument('--maps-dir', help='game/csgo/maps folder (default: found through steam)')
    parser.add_argument('--out', default='maps', help='output folder (default: ./maps)')
    args = parser.parse_args()

    maps_dir = args.maps_dir or find_maps_dir()
    if not maps_dir:
        print('Could not find the CS2 maps folder, pass it with --maps-dir')
        return 1

    names = args.maps or sorted(
        name[:-4] for name in os.listdir(maps_dir)
        if name.startswith('de_') and name.endswith('.vpk') and not name.endswith('_vanity.vpk')
    )

    print(f'Exporting {len(names)} map(s) from {maps_dir}')
    failed = [name for name in names if not export_map(maps_dir, name, args.out)]

    if failed:
        print(f'Failed: {", ".join(failed)}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
