#!/usr/bin/python3
"""Compares platform/port_remastered_cmdl.cpp against retrotool's `cmdl convert`.

    check.py --build /tmp/rmcmdl_tool --models N
    check.py --build /tmp/rmcmdl_tool --model <uuid.CMDL> ...

For each model it runs retrotool (the oracle: it writes materials.tsv and a glTF whose
accessors hold the decoded geometry) and the C++ tool (which writes materials.tsv and
its own vbuf/mesh dumps), then checks:

  * materials.tsv is byte identical,
  * per glTF primitive: material, positions, normals, every UV set, joints, weights and
    indices equal the tool's dump. Integers exactly; floats within 1e-6, and whether they
    were in fact bit identical is reported.

The model list is chosen to cover every vertex format, index width and model kind the
files use; --survey prints that list before running anything, and --parse-all runs the
tool over every model in x/all (retrotool can convert only the ones whose textures were
extracted, so that pass is the parser's own check). Scratch output goes under --work
(default /tmp/rmcmdl-check) and each model's two directories are deleted as soon as it
has been compared, so the run stays small.
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALL = os.path.join(ROOT, "build", "mpr", "x", "all")
RETROTOOL = os.path.join(ROOT, "build", "mpr-tools", "retrotool", "target", "release", "retrotool")

FLOAT_TOLERANCE = 1e-6

FORMATS = {
    0: "R8Unorm", 1: "R8Uint", 2: "R8Snorm", 3: "R8Sint", 4: "R16Unorm", 5: "R16Uint",
    6: "R16Snorm", 7: "R16Sint", 8: "R16Float", 9: "Rg8Unorm", 10: "Rg8Uint", 11: "Rg8Snorm",
    12: "Rg8Sint", 13: "R32Uint", 14: "R32Sint", 15: "R32Float", 16: "Rg16Unorm",
    17: "Rg16Uint", 18: "Rg16Snorm", 19: "Rg16Sint", 20: "Rg16Float", 21: "Rgba8Unorm",
    22: "Rgba8Uint", 23: "Rgba8Snorm", 24: "Rgba8Sint", 25: "Rgb10a2Unorm", 26: "Rgb10a2Uint",
    27: "Rg32Uint", 28: "Rg32Sint", 29: "Rg32Float", 30: "Rgba16Unorm", 31: "Rgba16Uint",
    32: "Rgba16Snorm", 33: "Rgba16Sint", 34: "Rgba16Float", 35: "Rgb32Uint", 36: "Rgb32Sint",
    37: "Rgb32Float", 38: "Rgba32Uint", 39: "Rgba32Sint", 40: "Rgba32Float",
}
COMPONENTS = {
    0: "POSITION", 1: "NORMAL", 2: "TANGENT_0", 3: "TANGENT_1", 4: "TANGENT_2",
    5: "TEXCOORD_0", 6: "TEXCOORD_1", 7: "TEXCOORD_2", 8: "TEXCOORD_3", 9: "COLOR",
    10: "BONE_INDICES", 11: "BONE_WEIGHTS", 12: "BAKED_LIGHTING_COORD",
    13: "BAKED_LIGHTING_TANGENT",
}
INDEX_WIDTH = {5121: 1, 5123: 2, 5125: 4}
INDEX_BITS = {0: 8, 1: 16, 2: 32}
COMPONENT_STRUCT = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
TYPE_COUNT = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4}

# glTF attribute name -> the name the tool dumps the same attribute under.
NAME_MAP = {"POSITION": "POSITION", "NORMAL": "NORMAL", "TANGENT": "TANGENT",
            "TEXCOORD_0": "TEXCOORD_0", "TEXCOORD_1": "TEXCOORD_1", "TEXCOORD_2": "TEXCOORD_2",
            "TEXCOORD_3": "TEXCOORD_3", "JOINTS_0": "JOINTS_0", "WEIGHTS_0": "WEIGHTS_0",
            "TANGENT_1": "TANGENT_1", "TANGENT_2": "TANGENT_2",
            "BAKED_LIGHTING_COORD": "BAKED_LIGHTING_COORD",
            "BAKED_LIGHTING_TANGENT": "BAKED_LIGHTING_TANGENT"}


# ---------------------------------------------------------------- raw file survey

def read_form(data, off):
    """(id, body, end) of the RFRM form at off, or None."""
    if data[off:off + 4] != b"RFRM":
        return None
    size = struct.unpack_from("<Q", data, off + 4)[0]
    return data[off + 20:off + 24], off + 32, off + 32 + size


def read_blocks(data, start, end):
    """Yields (id, body, end) over forms and chunks, nesting included."""
    p = start
    while p < end:
        form = read_form(data, p)
        if form is not None:
            _, s, e = form
            yield from read_blocks(data, s, e)
            p = e
            continue
        cid = data[p:p + 4]
        size = struct.unpack_from("<Q", data, p + 4)[0]
        skip = struct.unpack_from("<Q", data, p + 16)[0]
        yield cid, p + 24 + skip, p + 24 + skip + size
        p = p + 24 + skip + size


def survey(path):
    """What a model file uses: form kind, vertex formats per component, index widths."""
    with open(path, "rb") as fh:
        data = fh.read()
    form, s, e = read_form(data, 0)
    info = {"form": form.decode(), "formats": {}, "index": set(), "skinned": False}
    for cid, bs, be in read_blocks(data, s, e):
        if cid == b"SKHD":
            info["skinned"] = True
        if cid == b"VBUF":
            p = bs
            n = struct.unpack_from("<I", data, p)[0]
            p += 4
            for _ in range(n):
                _vcount, ccount = struct.unpack_from("<II", data, p)
                p += 8
                for _ in range(ccount):
                    bidx, off, stride, fmt, comp = struct.unpack_from("<IIIII", data, p)
                    p += 20
                    name = COMPONENTS.get(comp, "component_%d" % comp)
                    info["formats"].setdefault(name, set()).add(FORMATS.get(fmt, "format_%d" % fmt))
                p += 1  # num_buffers
        elif cid == b"IBUF":
            n = struct.unpack_from("<I", data, bs)[0]
            info["index"] = set(struct.unpack_from("<%dI" % n, data, bs + 4))
    return info


def survey_all():
    """Every model in x/all, keyed by a signature of what it uses."""
    signatures = collections.OrderedDict()
    for name in sorted(os.listdir(ALL)):
        if not (name.endswith(".CMDL") or name.endswith(".SMDL")):
            continue
        path = os.path.join(ALL, name)
        try:
            info = survey(path)
        except Exception:  # noqa: BLE001
            continue
        signature = (info["form"], info["skinned"],
                     tuple(sorted((k, tuple(sorted(v))) for k, v in info["formats"].items())),
                     tuple(sorted(info["index"])))
        signatures.setdefault(signature, []).append(name)
    return signatures


_CONVERTIBLE = {}


def convertible(name, work):
    """Whether retrotool can convert this model (it needs every TXTR it uses).

    Roughly a third of the models reference a TXTR that was never extracted, and for
    those the oracle can only produce materials.tsv, so the geometry of those is not
    comparable and a convertible model of the same kind is preferred."""
    if name not in _CONVERTIBLE:
        directory = os.path.join(work, "probe")
        shutil.rmtree(directory, ignore_errors=True)
        os.makedirs(directory, exist_ok=True)
        result = subprocess.run([RETROTOOL, "cmdl", "convert", os.path.join(ALL, name), directory],
                                capture_output=True, text=True)
        _CONVERTIBLE[name] = result.returncode == 0
        shutil.rmtree(directory, ignore_errors=True)
    return _CONVERTIBLE[name]


_LAYERED = {}


def layered_materials(name, work):
    """Whether a model's materials include the layered (PBR) texture parameters.

    Those take the CPLX branch of the tsv writer, which is a different line shape from
    everything else, so a few models that have them are worth checking."""
    if name not in _LAYERED:
        directory = os.path.join(work, "probe")
        shutil.rmtree(directory, ignore_errors=True)
        os.makedirs(directory, exist_ok=True)
        result = subprocess.run([RETROTOOL, "cmdl", "convert", os.path.join(ALL, name), directory],
                                capture_output=True, text=True,
                                env=dict(os.environ, RETROTOOL_MATERIALS_ONLY="1"))
        tsv = ""
        if result.returncode == 0:
            with open(os.path.join(directory, "materials.tsv"), "r") as fh:
                tsv = fh.read()
        _LAYERED[name] = "\nlayered\t" in tsv or tsv.startswith("layered\t")
        shutil.rmtree(directory, ignore_errors=True)
    return _LAYERED[name]


def choose(signatures, wanted, work):
    """Picks models covering every vertex format, index width and model kind.

    Every signature gets one model, preferring the smallest one retrotool can convert,
    and then the smallest convertible spares are taken until `wanted` is reached. Each
    signature holds a model's full component-to-format map, so covering all of them
    covers every format and index width in the file set."""
    ordered = []
    for signature, names in signatures.items():
        names = sorted(names, key=lambda n: os.path.getsize(os.path.join(ALL, n)))
        chosen = next((n for n in names if convertible(n, work)), None)
        ordered.append((signature, chosen or names[0], len(names)))
    ordered.sort(key=lambda entry: (entry[0][0], not entry[0][1], entry[0][3]))
    chosen = []
    seen = set()
    for _signature, name, _count in ordered:
        if name not in seen:
            seen.add(name)
            chosen.append(name)
    covered_formats = set()
    covered_index = set()
    for name in chosen:
        info = survey(os.path.join(ALL, name))
        for formats in info["formats"].values():
            covered_formats |= formats
        covered_index |= info["index"]
    spare = sorted((n for names in signatures.values() for n in names if n not in seen),
                   key=lambda n: os.path.getsize(os.path.join(ALL, n)))
    for name in spare:
        if len(chosen) >= wanted:
            break
        info = survey(os.path.join(ALL, name))
        new_formats = set()
        for formats in info["formats"].values():
            new_formats |= formats
        new_index = info["index"]
        if new_formats <= covered_formats and new_index <= covered_index:
            continue
        if not convertible(name, work):
            continue
        covered_formats |= new_formats
        covered_index |= new_index
        seen.add(name)
        chosen.append(name)
    for name in spare:
        if len(chosen) >= wanted:
            break
        if name not in seen and convertible(name, work):
            seen.add(name)
            chosen.append(name)
    # A few models whose materials carry layered textures, so that branch of the tsv
    # writer is compared too.
    if sum(1 for name in chosen if layered_materials(name, work)) < 3:
        for name in spare:
            if sum(1 for chosen_name in chosen
                   if layered_materials(chosen_name, work)) >= 3:
                break
            if name in seen or not convertible(name, work) or not layered_materials(name, work):
                continue
            seen.add(name)
            chosen.append(name)
    return chosen


# ---------------------------------------------------------------- dump readers

class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, count):
        if self.pos + count > len(self.data):
            raise ValueError("dump is shorter than its header says")
        out = self.data[self.pos:self.pos + count]
        self.pos += count
        return out

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def text(self, count):
        return self.take(count).decode("utf-8")

    def floats(self, count):
        return list(struct.unpack("<%df" % count, self.take(4 * count)))

    def uints(self, count):
        return list(struct.unpack("<%dI" % count, self.take(4 * count)))


def read_vbuf(path):
    with open(path, "rb") as fh:
        data = fh.read()
    reader = Reader(data)
    if reader.take(4) != b"RMD1":
        raise ValueError("%s: not a vbuf dump" % path)
    vertex_count = reader.u32()
    count = reader.u32()
    attributes = {}
    for _ in range(count):
        name = reader.text(reader.u32())
        components = reader.u32()
        is_integer = reader.u32()
        values = reader.uints(vertex_count * components) if is_integer \
            else reader.floats(vertex_count * components)
        attributes[name] = {"components": components, "integer": is_integer, "values": values}
    if reader.pos != len(data):
        raise ValueError("%s: %d bytes left over" % (path, len(data) - reader.pos))
    return vertex_count, attributes


def read_mesh(path):
    with open(path, "rb") as fh:
        data = fh.read()
    reader = Reader(data)
    if reader.take(4) != b"RSH1":
        raise ValueError("%s: not a mesh dump" % path)
    mesh = {
        "material": reader.u32(), "vertexBuffer": reader.u32(), "indexBuffer": reader.u32(),
        "indexStart": reader.u32(), "indexCount": reader.u32(), "indexWidth": reader.u32(),
        "vertexCount": reader.u32(),
    }
    mesh["indices"] = reader.uints(mesh["indexCount"])
    if reader.pos != len(data):
        raise ValueError("%s: %d bytes left over" % (path, len(data) - reader.pos))
    return mesh


# ---------------------------------------------------------------- glTF reading

class Gltf:
    def __init__(self, directory):
        self.dir = directory
        with open(os.path.join(directory, "out.gltf"), "r") as fh:
            self.json = json.load(fh)
        self.buffers = {}

    def buffer(self, index):
        if index not in self.buffers:
            uri = self.json["buffers"][index]["uri"]
            with open(os.path.join(self.dir, uri), "rb") as fh:
                self.buffers[index] = fh.read()
        return self.buffers[index]

    def read_accessor(self, index):
        """(values, componentsPerElement, isInteger, normalized), with normalization applied."""
        accessor = self.json["accessors"][index]
        count = accessor["count"]
        components = TYPE_COUNT[accessor["type"]]
        component_type = accessor["componentType"]
        normalized = accessor.get("normalized", False)
        data = self.buffer(self.json["bufferViews"][accessor["bufferView"]]["buffer"])
        offset = self.json["bufferViews"][accessor["bufferView"]].get("byteOffset", 0) + \
            accessor.get("byteOffset", 0)
        stride = self.json["bufferViews"][accessor["bufferView"]].get("byteStride")
        if stride is None:
            stride = INDEX_WIDTH[component_type] * components
        code = COMPONENT_STRUCT[component_type]
        if component_type == 5126:  # f32 is normalized onto itself
            normalized = False
        values = []
        for element in range(count):
            at = offset + element * stride
            row = struct.unpack_from("<" + str(components) + code, data, at)
            if normalized:
                if code in ("b", "h"):
                    divisor = 127.0 if code == "b" else 32767.0
                    row = [max(v / divisor, -1.0) for v in row]
                else:
                    divisor = 255.0 if code == "B" else 65535.0
                    row = [v / divisor for v in row]
            values.extend(row)
        return values, components, component_type in (5120, 5121, 5122, 5123, 5125), normalized


def compare_floats(name, mine, theirs, problems, exact):
    if len(mine) != len(theirs):
        problems.append("%s: %d values against %d" % (name, len(mine), len(theirs)))
        return
    worst = 0.0
    bits_equal = True
    for a, b in zip(mine, theirs):
        if a != b:
            bits_equal = False
            worst = max(worst, abs(a - b))
    if worst > FLOAT_TOLERANCE:
        problems.append("%s: differs by up to %g" % (name, worst))
    exact[name] = bits_equal


def compare_ints(name, mine, theirs, problems):
    if mine != theirs:
        first = next(i for i, (a, b) in enumerate(zip(mine, theirs)) if a != b)
        problems.append("%s: value %d is %d against %d" % (name, first, mine[first], theirs[first]))


def compare_tsv(mine_dir, oracle_dir, problems, kinds=None):
    """materials.tsv has to match byte for byte."""
    with open(os.path.join(mine_dir, "materials.tsv"), "rb") as fh:
        mine_tsv = fh.read()
    with open(os.path.join(oracle_dir, "materials.tsv"), "rb") as fh:
        oracle_tsv = fh.read()
    if kinds is not None:
        kinds |= {line.split("\t", 1)[0] for line in oracle_tsv.decode("ascii", "replace").splitlines()}
    if mine_tsv == oracle_tsv:
        return True
    mine_lines = mine_tsv.decode("utf-8", "replace").splitlines()
    oracle_lines = oracle_tsv.decode("utf-8", "replace").splitlines()
    detail = "materials.tsv differs (%d against %d lines)" % (len(mine_lines), len(oracle_lines))
    for i in range(min(len(mine_lines), len(oracle_lines))):
        if mine_lines[i] != oracle_lines[i]:
            detail += "\n    line %d: mine %r" % (i + 1, mine_lines[i])
            detail += "\n    line %d: theirs %r" % (i + 1, oracle_lines[i])
            break
    problems.append(detail)
    return False


def check_geometry(mine_dir, oracle_dir, report):
    """Compares every glTF primitive against the tool's dumps."""
    problems = []
    gltf = Gltf(oracle_dir)
    exact = {}
    unmatched = []
    meshes = gltf.json.get("meshes", [])
    dumps = [name for name in os.listdir(mine_dir) if name.startswith("mesh") and name.endswith(".bin")]
    if len(dumps) != len(meshes):
        problems.append("%d mesh dumps against %d glTF meshes" % (len(dumps), len(meshes)))
    report["meshes"] += len(meshes)
    for mesh_index, mesh in enumerate(meshes):
        primitives = mesh.get("primitives", [])
        if len(primitives) != 1:
            problems.append("mesh %d: %d primitives, expected 1" % (mesh_index, len(primitives)))
            continue
        primitive = primitives[0]
        mine_mesh = read_mesh(os.path.join(mine_dir, "mesh%d.bin" % mesh_index))
        if primitive.get("material") != mine_mesh["material"]:
            problems.append("mesh %d: material %s against %s" %
                            (mesh_index, primitive.get("material"), mine_mesh["material"]))
        vertex_count, attributes = read_vbuf(os.path.join(mine_dir, "vbuf%d.bin" % mine_mesh["vertexBuffer"]))
        if vertex_count != mine_mesh["vertexCount"]:
            problems.append("mesh %d: %d vertices against %d" %
                            (mesh_index, vertex_count, mine_mesh["vertexCount"]))
        seen = set()
        for gltf_name, accessor_index in primitive.get("attributes", {}).items():
            dump_name = NAME_MAP.get(gltf_name)
            if dump_name is None:
                # glTF exposes the remaining components under "_" prefixed custom names.
                dump_name = gltf_name[1:] if gltf_name.startswith("_") else gltf_name
            if dump_name not in attributes:
                unmatched.append("%s (gltf) has no dump as %s" % (gltf_name, dump_name))
                continue
            seen.add(dump_name)
            values, components, is_integer, normalized = gltf.read_accessor(accessor_index)
            # A normalized integer accessor holds floats once decoded, which is what
            # the tool stores, so the dump is only integer for the plain integer ones.
            is_integer = is_integer and not normalized
            attribute = attributes[dump_name]
            if attribute["integer"] != is_integer:
                problems.append("%s: integer flag %s against %s" %
                                (gltf_name, attribute["integer"], is_integer))
            label = "mesh %d %s" % (mesh_index, gltf_name)
            if is_integer:
                compare_ints(label, attribute["values"], values, problems)
                report["int_arrays"] += 1
            else:
                compare_floats(label, attribute["values"], values, problems, exact)
        for dump_name in sorted(set(attributes) - seen):
            unmatched.append("%s (dump) has no glTF attribute" % dump_name)
        index_accessor = primitive.get("indices")
        if index_accessor is None:
            problems.append("mesh %d: glTF primitive has no indices" % mesh_index)
        else:
            values, _components, _is_integer, _normalized = gltf.read_accessor(index_accessor)
            compare_ints("mesh %d indices" % mesh_index, mine_mesh["indices"], values, problems)
            report["int_arrays"] += 1
        report["attributes"] += len(primitive.get("attributes", {}))
    report["exact"] += len(exact)
    report["inexact"] += len(exact) - sum(1 for v in exact.values() if v)
    report["unmatched"] += unmatched
    return problems


# ---------------------------------------------------------------- driver

def parse_all(args):
    """Runs the tool over every model in x/all, which is the parser's robustness check:
    the comparison above only sees the models retrotool can convert."""
    failures = collections.Counter()
    examples = []
    total = 0
    for signature, names in survey_all().items():
        for name in names:
            total += 1
            directory = os.path.join(args.work, "parseall")
            shutil.rmtree(directory, ignore_errors=True)
            result = subprocess.run([args.build, os.path.join(ALL, name), directory],
                                    capture_output=True, text=True)
            shutil.rmtree(directory, ignore_errors=True)
            if result.returncode != 0:
                message = result.stderr.strip()[:120]
                failures[message] += 1
                if len(examples) < 10:
                    examples.append((name, message))
    print("parsed %d models, %d failures" % (total, sum(failures.values())))
    for message, count in failures.most_common(12):
        print("   %4d  %s" % (count, message))
    for name, message in examples:
        print("   e.g. %s: %s" % (name, message))
    return 0 if not failures else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", default="/tmp/rmcmdl_tool", help="the compiled tool")
    parser.add_argument("--models", type=int, default=48, help="how many models to check")
    parser.add_argument("--model", action="append", help="check this file name, repeatable")
    parser.add_argument("--work", default="/tmp/rmcmdl-check", help="scratch directory")
    parser.add_argument("--survey", action="store_true", help="print the model list and stop")
    parser.add_argument("--parse-all", action="store_true",
                        help="parse every model in x/all and report failures, no comparison")
    parser.add_argument("--keep", action="store_true", help="keep the scratch output")
    args = parser.parse_args()

    signatures = survey_all()
    print("models in %s: %d files, %d distinct (form, skinned, formats, index width) sets" %
          (ALL, sum(len(v) for v in signatures.values()), len(signatures)))
    formats = set()
    widths = set()
    for signature in signatures:
        for _name, entries in signature[2]:
            formats |= set(entries)
        widths |= set(signature[3])
    print("vertex formats: %s" % ", ".join(sorted(formats)))
    # EBufferType: 0 is 8 bit, 1 is 16, 2 is 32.
    print("index widths: %s" % ", ".join("%d bit" % INDEX_BITS[w] for w in sorted(widths)))
    print("forms: %s" % ", ".join(sorted({s[0] for s in signatures})))
    print("skinned: %s" % ", ".join(sorted({str(s[1]) for s in signatures})))

    os.makedirs(args.work, exist_ok=True)
    if args.parse_all:
        return parse_all(args)
    if args.model:
        models = args.model
    else:
        models = choose(signatures, args.models, args.work)
    print("checking %d models" % len(models))
    if args.survey:
        for name in models:
            info = survey(os.path.join(ALL, name))
            print("   %-46s %s skinned=%d formats=%s index=%s" %
                  (name, info["form"], info["skinned"],
                   sorted({f for v in info["formats"].values() for f in v}),
                   sorted(info["index"])))
        return 0

    passed = []
    geometry = []
    failed = []
    report = {"meshes": 0, "attributes": 0, "exact": 0, "inexact": 0, "unmatched": [],
              "skipped_geometry": 0, "int_arrays": 0}
    kinds = set()
    for name in models:
        path = os.path.join(ALL, name)
        oracle_dir = os.path.join(args.work, "oracle", name)
        mine_dir = os.path.join(args.work, "mine", name)
        shutil.rmtree(oracle_dir, ignore_errors=True)
        shutil.rmtree(mine_dir, ignore_errors=True)
        os.makedirs(oracle_dir, exist_ok=True)
        os.makedirs(mine_dir, exist_ok=True)
        # retrotool resolves the textures next to the model, so it is run with the
        # model where it is and its output directory somewhere else.
        oracle = subprocess.run([RETROTOOL, "cmdl", "convert", path, oracle_dir],
                                capture_output=True, text=True)
        if oracle.returncode != 0:
            # retrotool also writes the TXTR files as PNGs, and it stops on the first
            # texture that was never extracted, so a model can be checked for
            # materials only. RETROTOOL_MATERIALS_ONLY stops it right after the tsv.
            fallback = subprocess.run([RETROTOOL, "cmdl", "convert", path, oracle_dir],
                                      capture_output=True, text=True,
                                      env=dict(os.environ, RETROTOOL_MATERIALS_ONLY="1"))
            if fallback.returncode != 0:
                failed.append((name, ["retrotool failed: %s" % fallback.stderr.strip()]))
                continue
            oracle = None
        shutil.rmtree(oracle_dir, ignore_errors=True)
        os.makedirs(oracle_dir, exist_ok=True)
        write = subprocess.run([RETROTOOL, "cmdl", "convert", path, oracle_dir],
                               capture_output=True, text=True,
                               env=dict(os.environ) if oracle is not None
                               else dict(os.environ, RETROTOOL_MATERIALS_ONLY="1"))
        if write.returncode != 0:
            failed.append((name, ["retrotool failed: %s" % write.stderr.strip()]))
            continue
        mine = subprocess.run([args.build, path, mine_dir], capture_output=True, text=True)
        if mine.returncode != 0:
            failed.append((name, ["the tool failed: %s" % mine.stderr.strip()]))
            continue
        problems = []
        tsv_equal = compare_tsv(mine_dir, oracle_dir, problems, kinds)
        if oracle is not None:
            problems += check_geometry(mine_dir, oracle_dir, report)
        else:
            report["skipped_geometry"] += 1
            print("note %s: materials only, retrotool cannot convert it (a TXTR it wants "
                  "was never extracted)" % name)
        if problems:
            failed.append((name, problems))
        elif oracle is not None and tsv_equal:
            passed.append(name)
            geometry.append(name)
        if not args.keep:
            shutil.rmtree(oracle_dir, ignore_errors=True)
            shutil.rmtree(mine_dir, ignore_errors=True)

    print()
    print("materials.tsv and geometry: %d models equal, %d different, %d materials only" %
          (len(passed), len(failed), report["skipped_geometry"]))
    print("materials.tsv alone: %d models compared in total" % (len(passed) + report["skipped_geometry"]))
    # What the geometry comparison actually covered, so the coverage claim is measured
    # rather than assumed.
    covered_formats = set()
    covered_widths = set()
    covered_forms = set()
    covered_skinned = set()
    for name in geometry:
        info = survey(os.path.join(ALL, name))
        for formats in info["formats"].values():
            covered_formats |= formats
        covered_widths |= info["index"]
        covered_forms.add(info["form"])
        covered_skinned.add(info["skinned"])
    print("geometry coverage over %d models: forms %s, skinned %s" %
          (len(geometry), ", ".join(sorted(covered_forms)),
           ", ".join("yes" if v else "no" for v in (True, False) if v in covered_skinned)))
    print("  vertex formats: %s" % ", ".join(sorted(covered_formats)))
    print("  index widths: %s" % ", ".join("%d bit" % INDEX_BITS[w] for w in sorted(covered_widths)))
    print("primitives compared: %d, attribute arrays compared: %d" % (report["meshes"], report["attributes"]))
    print("float arrays bit identical: %d, within %g but not identical: %d" %
          (report["exact"], FLOAT_TOLERANCE, report["inexact"]))
    print("integer arrays (joints, indices) compared exactly: %d" % report["int_arrays"])
    print("materials.tsv line kinds compared: %s" % ", ".join(sorted(kinds)))
    if report["unmatched"]:
        print("attributes with no counterpart on the other side:")
        for item in sorted(set(report["unmatched"])):
            print("   %s" % item)
    for name, problems in failed:
        print()
        print("FAIL %s" % name)
        for problem in problems[:6]:
            print("   %s" % problem)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())