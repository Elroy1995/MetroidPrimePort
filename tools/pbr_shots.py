#!/usr/bin/env python3
"""A/B contact sheets of models under the PBR reflection probe (smoke builds only).

    tools/pbr_shots.py --out build/pbr-shots/metal \\
        --variant clamped=~/.local/share/"Metroid Prime"/mods/remastered-models \\
        --variant full=build/mpr/mods-test/metal \\
        --place chozo --place tallon --model 5644E7A1 --model 79D95DEC

One game is started per (variant, place), under Xvfb and with its own console port and
user dir, so runs do not touch another session's. A variant is `name=<mod folder>` (the
folder holding the CMDL/TXTR files) or `name=none` for retail. A place is a name from
PLACES, `<world>[:<mrea>]`, or `name=<file>.mpss`: a save state (F5 in game, then
`savestates/slotN.mpss` in the user dir), which puts the camera exactly where it was saved,
past any cutscene. The named places boot into normal gameplay, not a cinematic. A state is
a world load like any other, so it is no faster than those; it is for a spot they do not give.

Each game is driven through the debug console: it waits for `status` to report a running,
first-person game (and fails with the status text if that does not happen in time,
rather than sleeping and shooting whatever is there), then for every model, yaw and
probe mode takes a shot. The probe mode is switched live (`probe off|on|mirror|window`),
so it costs no relaunch.

Output, per place: `<out>/<place>.png` (rows = model, yaw and probe mode, columns =
variants, the first row the bare scene) and `<out>/<place>.tsv` with each cell's mean
luminance over the middle of the frame and the game's sky/probe/FOV status.
"""
import argparse
import concurrent.futures
import os
import shutil
import socket
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Boot rooms that start in normal gameplay. The default areas of the Frigate, Tallon and
# the Crater play an arrival cinematic, so those name another room.
PLACES = {
    "frigate": ("158EFE17", "07640602"),
    "chozo": ("83F6FF6F", "D5CDB809"),
    "phendrana": ("A8BE6291", "C00E3781"),
    "tallon": ("39F2DE28", "11A02448"),
    "mines": ("B1AC4D65", "430E999C"),
    "magmoor": ("3EF8237C", "3BEAADC9"),
    "crater": ("C13B09D1", "49CB2363"),
}
PROBE_MODES = ("off", "on", "mirror", "window")


class Failure(Exception):
    pass


class Console:
    def __init__(self, port, deadline, proc):
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                if proc.poll() is not None:
                    raise Failure("the game exited (%s) before its console came up" % proc.returncode)
                if time.monotonic() >= deadline:
                    raise Failure("no console on port %d" % port)
                time.sleep(0.25)
        self.sock.settimeout(30)
        self.reader = self.sock.makefile("r", encoding="utf-8", errors="replace")

    def try_cmd(self, line):
        """Returns (ok, reply lines, error text)."""
        try:
            self.sock.sendall((line + "\n").encode())
            out = []
            while True:
                reply = self.reader.readline()
                if not reply:
                    raise Failure("the game closed the console during `%s`" % line)
                reply = reply.rstrip("\n")
                if reply.startswith("=> "):
                    return reply == "=> ok", out, reply[3:]
                out.append(reply)
        except OSError as e:
            raise Failure("`%s`: %s" % (line, e))

    def cmd(self, line):
        ok, out, err = self.try_cmd(line)
        if not ok:
            raise Failure("`%s`: %s" % (line, err))
        return out


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def ensure_display(display):
    if subprocess.run(["xdpyinfo", "-display", display], capture_output=True).returncode == 0:
        return None
    xvfb = subprocess.Popen(["Xvfb", display, "-screen", "0", "1280x720x24", "-ac"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + 10
    while subprocess.run(["xdpyinfo", "-display", display], capture_output=True).returncode != 0:
        if time.monotonic() >= deadline:
            xvfb.kill()
            raise Failure("Xvfb %s did not start" % display)
        time.sleep(0.2)
    return xvfb


def status_fields(lines):
    f = {"raw": " | ".join(lines)}
    for l in lines:
        if l.startswith("frame "):
            f["state"] = l.rsplit(" ", 1)[1]
        elif l.startswith("world "):
            f["where"] = l
        elif l.startswith("first person "):
            f["gameplay"] = l == "first person 1, cinematic 0"
        elif l.startswith("sky "):
            f["sky"] = l[4:]
        elif l.startswith("probe "):
            f["probe"] = l[6:]
            f["weight"] = l.split()[3].rstrip(",")
        elif l.startswith("camera "):
            f["fov"] = l.split()[3]
    return f


def wait_status(con, deadline, want, what):
    """Poll `status` until want(fields) holds; the console is the clock, not a sleep."""
    last = "no reply"
    while True:
        ok, out, err = con.try_cmd("status")
        if ok:
            f = status_fields(out)
            if want(f):
                return f
            last = f["raw"]
        else:
            last = err
        if time.monotonic() >= deadline:
            raise Failure("timed out waiting for %s; last status: %s" % (what, last))
        time.sleep(0.25)


def state_world(path):
    with open(path, "rb") as f:
        head = f.read(12)
    if head[:4] != b"MPSS":
        raise Failure("%s is not a save state" % path)
    return "%08X" % struct.unpack("<I", head[8:12])[0]


def run_game(args, variant, mod, place, world, mrea, state=None):
    """One game: returns the cells [(row label, bmp path)] and the status it shot under."""
    tag = "%s-%s" % (place, variant)
    run = os.path.join(args.out, "run", tag)
    shutil.rmtree(run, ignore_errors=True)
    mods = os.path.join(run, "mods")
    os.makedirs(mods)
    os.makedirs(os.path.join(run, "user"))
    if mod is not None:
        os.symlink(mod, os.path.join(mods, "mod"))
    boot = world + (":" + mrea if mrea else "")
    if state is not None:
        # Loaded from a room known to be in gameplay, since a state cannot load in a cinematic.
        os.makedirs(os.path.join(run, "user", "savestates"))
        shutil.copy(state, os.path.join(run, "user", "savestates", "slot1.mpss"))
        boot = ":".join(PLACES["frigate"])
    port = free_port()
    env = dict(os.environ, DISPLAY=args.display, SDL_AUDIO_DRIVER="dummy",
               MP_USER_PATH=os.path.join(run, "user"), MP_MODS=mods,
               MP_CACHE_PATH=os.path.join(ROOT, "build/sweep-out/cache"),
               MP_FAST_BOOT="1", MP_DISABLE_AI_AUDIO="1", MP_ASPECT="16:9",
               MP_CONSOLE=str(port), MP_BOOT_WORLD=boot)
    # MP_BOOT_WORLD rather than MP_SMOKE_WORLD: the latter starts a new game on the Frigate
    # and only warps once its intro cinematic has played out.
    for name in ("MP_PBR_PROBE", "MP_SMOKE_WORLD", "MP_SMOKE_WORLD_AREA"):
        env.pop(name, None)
    log = open(os.path.join(run, "game.log"), "w")
    started = time.monotonic()
    proc = subprocess.Popen([os.path.join(args.build, "metroid_prime_port"), args.iso], cwd=run,
                            env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    cells = []
    try:
        deadline = started + args.ready_timeout
        con = Console(port, deadline, proc)
        def gameplay(f):
            return f.get("state") == "0" and f.get("gameplay") and world in f.get("where", "")

        if state is not None:
            wait_status(con, deadline, lambda f: f.get("state") == "0" and f.get("gameplay"),
                        "the boot room")
            con.cmd("state load 1")
            con.cmd("wait 2")
            last = " ".join(con.cmd("state last"))
            if not last.startswith("Loading"):
                raise Failure("state load: " + last)
            # the outgoing world may be the state's own; let it go before looking for gameplay
            con.try_cmd("wait 30")

        # An arrival cinematic starts a moment after the room does, so one good status is not
        # enough: it has to hold for a second of game time.
        while True:
            st = wait_status(con, deadline, gameplay, "first-person gameplay")
            con.cmd("wait 60")
            if gameplay(status_fields(con.cmd("status"))):
                break
        if mrea and mrea not in st["where"]:
            raise Failure("booted into %s, not MREA %s" % (st["where"], mrea))

        def shot(label):
            con.cmd("wait 2")
            f = status_fields(con.cmd("status"))
            if not gameplay(f):
                raise Failure("left first-person gameplay before `%s`: %s" % (label, f["raw"]))
            path = [l for l in con.cmd("shot") if l.endswith(".bmp")]
            if not path:
                raise Failure("shot gave no file")
            cells.append((label, path[-1].split()[-1]))

        shot("scene")
        # the bare scene at each yaw, which is what `window` has to line up with
        for yaw in args.yaw:
            if yaw is not None:
                con.cmd("face %s" % yaw)
                shot("scene yaw %s" % yaw)
        con.cmd("viewmodel light 1")
        for model in args.model:
            ok, _, err = con.try_cmd("viewmodel " + model)
            if not ok:
                cells.append(("%s: %s" % (model, err), None))
                continue
            deadline = time.monotonic() + args.step_timeout
            while not any(" loaded " in l for l in con.cmd("viewmodel status")):
                if time.monotonic() >= deadline:
                    raise Failure("model %s did not load" % model)
                time.sleep(0.1)
            for yaw in args.yaw:
                if yaw is not None:
                    con.cmd("face %s" % yaw)
                for mode in args.probe:
                    con.cmd("probe " + mode)
                    if mode != "off":
                        # six faces, one a frame, and only while something PBR is drawn
                        con.cmd("wait 8")
                        # retail materials never draw through the PBR path, and then it stays empty
                        st = wait_status(con, time.monotonic() + args.step_timeout,
                                         lambda f: f.get("weight", "0") != "0"
                                         or f.get("probe", "").endswith("draws 0"), "the probe to fill")
                    shot("%s%s %s" % (model, "" if yaw is None else " yaw %s" % yaw, mode))
        st = status_fields(con.cmd("status"))
        return cells, st, time.monotonic() - started
    except Failure as e:
        raise Failure("%s: %s (log %s)" % (tag, e, log.name))
    finally:
        # Not `quit`: quitting with a viewmodel up runs into a static destructor crash.
        try:
            os.killpg(proc.pid, 15)
            proc.wait(5)
        except (subprocess.TimeoutExpired, ProcessLookupError):
            try:
                os.killpg(proc.pid, 9)
            except ProcessLookupError:
                pass
        log.close()


def sheet(args, place, variants, results):
    from PIL import Image, ImageDraw, ImageStat
    labels = []
    for v in variants:
        for label, _ in results[v][0]:
            if label not in labels:
                labels.append(label)
    w, h, left, top = args.cell, args.cell * 9 // 16, 190, 22
    img = Image.new("RGB", (left + w * len(variants), top + h * len(labels)), (24, 24, 24))
    draw = ImageDraw.Draw(img)
    rows = []
    for x, v in enumerate(variants):
        draw.text((left + x * w + 6, 5), v, fill=(255, 255, 255))
        cells = dict(results[v][0])
        for y, label in enumerate(labels):
            path = cells.get(label)
            if x == 0:
                draw.text((6, top + y * h + 6), label.replace(" ", "\n", 1), fill=(255, 255, 255))
            if path is None:
                continue
            shot = Image.open(path).convert("RGB")
            # the middle of the frame is the model; its luminance is the number to compare
            cw, ch = shot.size
            mid = shot.crop((cw * 3 // 8, ch // 4, cw * 5 // 8, ch * 3 // 4)).convert("L")
            rows.append((label, v, "%.1f" % ImageStat.Stat(mid).mean[0]))
            img.paste(shot.resize((w, h), Image.LANCZOS), (left + x * w, top + y * h))
    out = os.path.join(args.out, place + ".png")
    img.save(out)
    with open(os.path.join(args.out, place + ".tsv"), "w") as f:
        for v in variants:
            st = results[v][1]
            f.write("# %s: %s; fov %s; sky %s; probe %s; %.0fs\n" % (
                v, st.get("where"), st.get("fov"), st.get("sky"), st.get("probe"), results[v][2]))
        f.write("cell\t" + "\t".join(variants) + "\n")
        for label in labels:
            lum = {v: l for (lab, v, l) in rows if lab == label}
            f.write(label + "\t" + "\t".join(lum.get(v, "-") for v in variants) + "\n")
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--out", required=True, help="output directory (sheets, and run/ with logs and shots)")
    p.add_argument("--variant", action="append", required=True, metavar="NAME=MODDIR|none")
    p.add_argument("--place", action="append", metavar="NAME|WORLD[:MREA]|NAME=STATE.mpss",
                   help="default chozo; names: " + ", ".join(PLACES))
    p.add_argument("--model", action="append", required=True, metavar="CMDL")
    p.add_argument("--probe", default="off,on", help="probe modes to shoot, of " + ",".join(PROBE_MODES))
    p.add_argument("--yaw", default="", help="comma-separated player yaws; default leaves the boot facing")
    p.add_argument("--build", default=os.path.join(ROOT, "build/fm-smoke"), help="smoke build directory")
    p.add_argument("--iso", default=os.environ.get("MP_ISO"), help="disc image (or MP_ISO)")
    p.add_argument("--display", default=":99", help="X display; an Xvfb is started on it if nothing is there")
    p.add_argument("--jobs", type=int, default=2, help="games at once")
    p.add_argument("--ready-timeout", type=float, default=60, help="seconds from launch to a running game")
    p.add_argument("--step-timeout", type=float, default=15, help="seconds for a model load or probe fill")
    p.add_argument("--cell", type=int, default=480, help="cell width in the sheet")
    args = p.parse_args()

    if not args.iso or not os.path.isfile(args.iso):
        p.error("no disc image: pass --iso or set MP_ISO")
    if args.display in (":0", ":1") or args.display == os.environ.get("DISPLAY"):
        p.error("not on a real display; captures there have reset the GPU")
    args.out = os.path.abspath(args.out)
    args.build = os.path.abspath(args.build)
    args.probe = args.probe.split(",")
    if [m for m in args.probe if m not in PROBE_MODES]:
        p.error("--probe takes " + ",".join(PROBE_MODES))
    args.yaw = args.yaw.split(",") if args.yaw else [None]
    variants = {}
    for v in args.variant:
        name, _, path = v.partition("=")
        path = None if path == "none" else os.path.abspath(os.path.expanduser(path))
        if path is not None and not os.path.isdir(path):
            p.error("variant %s: no folder %s" % (name, path))
        variants[name] = path
    places = {}
    for name in args.place or ["chozo"]:
        if name in PLACES:
            places[name] = PLACES[name]
        elif "=" in name:
            name, _, path = name.partition("=")
            path = os.path.abspath(os.path.expanduser(path))
            try:
                places[name] = (state_world(path), None, path)
            except (OSError, Failure) as e:
                p.error("place %s: %s" % (name, e))
        else:
            world, _, mrea = name.upper().partition(":")
            places[name.replace(":", "-")] = (world, mrea or None)
    os.makedirs(args.out, exist_ok=True)

    xvfb = ensure_display(args.display)
    failed = False
    try:
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            jobs = {(place, v): pool.submit(run_game, args, v, variants[v], place, *places[place])
                    for place in places for v in variants}
            for place in places:
                results = {}
                for v in variants:
                    try:
                        results[v] = jobs[place, v].result()
                    except Failure as e:
                        failed = True
                        print("FAILED " + str(e), file=sys.stderr)
                if results:
                    done = [v for v in variants if v in results]
                    print(sheet(args, place, done, results))
                    print(open(os.path.join(args.out, place + ".tsv")).read(), end="")
    finally:
        if xvfb is not None:
            xvfb.terminate()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
