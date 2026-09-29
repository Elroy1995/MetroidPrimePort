#!/usr/bin/env python3
"""Generate platform/port_ap_pickups_data.inc: randomprime's per-pickup room
patches and the retail pickup models, for Archipelago games.

In a randomized game a pickup no longer holds its retail item, so the retail
item's cutscene (Morph Ball, Varia, artifact totems...) must not play and the
pickup must look like what it holds. randomprime (MIT, see NOTICE) does the
first with two things, precomputed per pickup in its src/pickup_meta.rs.in:
  - objects_to_remove: the cinema relay, "Player Hint Disable Controls", the
    artifact totem layer switch and logbook screen, per room;
  - post_pickup_relay_connections: what the retail cinema's end did (jingle,
    HUD memo timers, doors, the Varia room's lights), moved onto a new
    "Randomizer Post Pickup Relay" that the pickup sets to zero on Arrived.

This turns both into the op stream of platform/port_skip_cutscenes.cpp,
applied after the skippable cutscene ops (always on in AP games), and checks
every op against the retail disc with those ops already applied.

    tools/gen_ap_pickup_patches.py <disc> <randomprime>/src/pickup_meta.rs.in \\
        platform/port_ap_pickups_data.inc
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_skippable_cutscenes import (  # noqa: E402
    OP_ADDCONN, OP_DELETE, OP_PUSH, apply_ops, load_sclys, parse_scly, split_name)

HERE = os.path.dirname(os.path.abspath(__file__))
SKIP_INC = os.path.join(HERE, '..', 'platform', 'port_skip_cutscenes_data.inc')
LANDING_SITE = 0xB2701146

STATES = {'ZERO': 9}
MSGS = {'ACTIVATE': 0x1, 'DECREMENT': 0x5, 'INCREMENT': 0x7, 'RESET_AND_START': 0xB,
        'SET_TO_MAX': 0xC, 'SET_TO_ZERO': 0xD, 'PLAY': 0x14}
ARRIVED, SET_TO_ZERO = 1, 0xD
RELAY = 0x15

# Model keys past the item types (0-40).
MODEL_MAIN_POWER_BOMB = 41
MODEL_OTHER = 42  # items for other games; see OTHER_MODEL


def read_skip_inc(path):
    text = open(path).read()
    rooms = {int(m[1], 16): (int(m[2]), int(m[3]))
             for m in re.finditer(r'\{0x([0-9A-F]{8}), (\d+), (\d+)\}', text)}

    def blob(name):
        body = text.split('static const unsigned char %s[] = {' % name)[1].split('};')[0]
        return bytes(int(x, 16) for x in re.findall(r'0x([0-9A-F]{2})', body))

    ops = blob('kSkipOps')
    return {r: ops[o:o + n] for r, (o, n) in rooms.items()}, blob('kLandingOps')


def parse_meta(path):
    """[(mrea, [(pickup id, [conn])], [removed id])] from pickup_meta.rs.in."""
    text = open(path).read()
    rooms = []
    for block in text.split('RoomInfo {')[1:]:
        mrea = int(re.search(r'res_id::MREA>::new\(0x([0-9A-Fa-f]+)\)', block)[1], 16)
        pickups = []
        body = block.split('pickup_locations: &[', 1)[1]
        for loc in body.split('PickupLocation {')[1:]:
            pid = int(re.search(r'location: ScriptObjectLocation \{ layer: \d+, instance_id: (\d+)',
                                loc)[1])
            conns = []
            post = loc.split('post_pickup_relay_connections: &[', 1)[1].split(']', 1)[0]
            for st, msg, tgt in re.findall(r'ConnectionState::(\w+),\s*message: ConnectionMsg::(\w+),'
                                           r'\s*target_object_id: (0x[0-9a-fA-F]+|\d+)', post):
                conns.append((STATES[st], MSGS[msg], int(tgt, 0)))
            pickups.append((pid, conns))
        removed = []
        otr = block.split('objects_to_remove: &[', 1)[1].split('\n        },', 1)[0]
        for ids in re.findall(r'instance_ids: &\[([^\]]*)\]', otr):
            removed += [int(x) for x in ids.replace(' ', '').split(',') if x]
        rooms.append((mrea, pickups, removed))
    return rooms


def room_ops(mrea, scly, pickups, removed):
    _, layers = parse_scly(scly)
    objs = {o['id']: o for _, ob in layers for o in ob}
    ops = b''
    for oid in removed:
        # The skippable patch already dropped a few of these.
        if oid in objs:
            ops += struct.pack('>BI', OP_DELETE, oid)
    top = max(o['id'] & 0xFFFF for o in objs.values())
    area = layers[0][1][0]['id'] & 0x03FF0000
    for pid, conns in pickups:
        assert pid in objs and objs[pid]['type'] == 0x11, '%08X: pickup %X' % (mrea, pid)
        if not conns:
            continue
        top += 1
        relay = area | top
        assert relay not in objs, '%08X: relay id %X taken' % (mrea, relay)
        props = struct.pack('>I', 2) + b'Randomizer Post Pickup Relay\0' + b'\x01'
        ops += struct.pack('>BBBIH', OP_PUSH, 0, RELAY, relay, len(conns))
        ops += b''.join(struct.pack('>III', *c) for c in conns)
        ops += struct.pack('>I', len(props)) + props
        ops += struct.pack('>BIIII', OP_ADDCONN, pid, ARRIVED, SET_TO_ZERO, relay)
    return ops


def pickup_models(sclys):
    """Model key -> (model, acs, character, animation), from the disc's
    upgrade pickups (capacity > 0)."""
    out = {}
    for mrea in sorted(sclys):
        for _, objs in parse_scly(sclys[mrea])[1]:
            for o in objs:
                if o['type'] != 0x11:
                    continue
                t = split_name(o['props'])[2]
                item, cap = struct.unpack_from('>II', t, 60)
                model = struct.unpack_from('>IIII', t, 84)
                if cap == 0 or (model[0] == 0xFFFFFFFF and model[1] == 0xFFFFFFFF):
                    continue
                key = MODEL_MAIN_POWER_BOMB if item == 7 and cap > 1 else item
                # Elite Quarters' Phazon Suit borrows the Gravity Suit model.
                if item != 23:
                    out.setdefault(key, model)
    return out


# Other games' items have no model of their own on the disc (randomprime's
# Cog/Zoomer/Nothing are custom assets): they show as the 100-energy orb, which
# no upgrade uses.
OTHER_MODEL = (0xFFFFFFFF, 0x3F21A526, 0, 0)


def main():
    disc, meta, out = sys.argv[1:4]
    sclys = load_sclys(disc)
    skip, landing = read_skip_inc(SKIP_INC)
    table = []
    for mrea, pickups, removed in parse_meta(meta):
        base = sclys[mrea]
        if mrea in skip:
            base, misses = apply_ops(base, skip[mrea])
            assert not misses, (hex(mrea), misses)
        if mrea == LANDING_SITE:
            base, misses = apply_ops(base, landing)
            assert not misses, 'landing'
        ops = room_ops(mrea, base, pickups, removed)
        if not ops:
            continue
        result, misses = apply_ops(base, ops)
        assert not misses, (hex(mrea), misses)
        # Every object the new relays talk to must still be there (randomprime
        # keeps a few that point outside the room; those it drops too).
        left = {o['id'] for _, ob in parse_scly(result)[1] for o in ob}
        for pid, conns in pickups:
            for c in conns:
                if c[2] not in left and c[2] in {o['id'] for _, ob in parse_scly(sclys[mrea])[1]
                                                  for o in ob}:
                    raise SystemExit('%08X: relay target %X removed' % (mrea, c[2]))
        table.append((mrea, ops))
    table.sort()
    models = pickup_models(sclys)
    models[MODEL_OTHER] = OTHER_MODEL

    blob = bytearray()
    with open(out, 'w') as f:
        f.write('// Generated by tools/gen_ap_pickup_patches.py from randomprime\'s\n'
                '// pickup_meta.rs.in (MIT, see NOTICE) and the retail disc. Do not edit.\n')
        f.write('static const PickupRoom kPickupRooms[] = {\n')
        for r, ops in table:
            f.write('    {0x%08X, %d, %d},\n' % (r, len(blob), len(ops)))
            blob += ops
        f.write('};\n\nstatic const unsigned char kPickupOps[] = {\n')
        for i in range(0, len(blob), 24):
            f.write('    ' + ','.join('0x%02X' % b for b in blob[i:i + 24]) + ',\n')
        f.write('};\n\n// Item type (0-40), 41 = main Power Bomb, 42 = another game\'s item.\n'
                'static const PickupModelEntry kPickupModels[] = {\n')
        for key in sorted(models):
            f.write('    {%d, 0x%08X, 0x%08X, %d, %d},\n' % ((key,) + models[key]))
        f.write('};\n')
    print('%d rooms, %d bytes of ops, %d models' % (len(table), len(blob), len(models)))


if __name__ == '__main__':
    main()
