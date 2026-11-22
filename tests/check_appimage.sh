#!/usr/bin/env bash
# Prueft das AppImage in einer frischen Umgebung ohne Quellordner
# (docs/appimage.md):
#   bash tests/check_appimage.sh [<AppImage>]      (Standard dawd/build/appimage/unidaw-*-x86_64.AppImage)
#
# bubblewrap (Paket bubblewrap) baut die Umgebung: nur /usr und /etc des
# Systems (nur lesen), leeres Home und /tmp, eigenes Netz (nur loopback),
# eigene PIDs, kein Audio-Geraet, kein ~/dev. Das AppImage laeuft dort ohne
# FUSE (APPIMAGE_EXTRACT_AND_RUN=1) wie ein Nutzer es startet: Starter im
# Vordergrund. Geprueft: Hilfe und Lizenzen, UI und Vendor-Dateien laden,
# Kits, Plugin-Profile und Rezepte aus dem AppImage, Modelle im Nutzerordner
# gesucht (Hinweis auf fetch-models), ein Render mit 4OSC ist hoerbar, die
# gebuendelten Bibliotheken kommen aus dem AppImage (libstdc++ nur, wenn die
# des Systems nicht neuer ist), Ctrl+C (SIGINT) beendet unidaw.
# Mit FUSE auf dem System laeuft vorab --help im normalen Modus (Mount).
#
# UNIDAW_CHECK_BASE=ubuntu-22.04: statt /usr und /etc dieses Rechners die
# schlanke aeltere Basis aus tools/buildroot.sh (glibc 2.35, nur python3,
# curl und libasound2) — der Start auf einer aelteren Distribution.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APPIMAGE="${1:-}"
if [ -z "$APPIMAGE" ]; then
    APPIMAGE="$(ls "$ROOT"/dawd/build/appimage/unidaw-*-x86_64.AppImage 2>/dev/null | head -1)"
fi
[ -f "$APPIMAGE" ] || { echo "check_appimage: no AppImage (bash tools/build_appimage.sh)" >&2; exit 1; }
command -v bwrap >/dev/null || { echo "check_appimage: bwrap missing (package bubblewrap)" >&2; exit 1; }
APPIMAGE="$(readlink -f "$APPIMAGE")"

if command -v fusermount3 >/dev/null || command -v fusermount >/dev/null; then
    "$APPIMAGE" --help | grep -q 'Usage:' || { echo "check_appimage: --help via FUSE failed" >&2; exit 1; }
    echo "ok: --help via FUSE mount"
fi

SYS=""
case "${UNIDAW_CHECK_BASE:-}" in
    "") ;;
    ubuntu-22.04) SYS="$(bash "$ROOT/tools/buildroot.sh" run)" ;;
    *) echo "check_appimage: unknown UNIDAW_CHECK_BASE (ubuntu-22.04)" >&2; exit 1 ;;
esac
if [ -n "$SYS" ]; then
    echo "check: base $SYS ($(bwrap --ro-bind "$SYS" / --proc /proc --dev /dev /usr/bin/ldd --version | head -1))"
fi

args=(--ro-bind "$SYS/usr" /usr --ro-bind "$SYS/etc" /etc
      --tmpfs /home --tmpfs /tmp --dir /home/user --dir /run/user/1000
      --proc /proc --dev /dev
      --ro-bind "$APPIMAGE" /opt/unidaw.AppImage
      --unshare-all --die-with-parent --new-session
      --clearenv --setenv HOME /home/user --setenv PATH /usr/bin:/bin --setenv USER user
      --setenv XDG_RUNTIME_DIR /run/user/1000 --setenv APPIMAGE_EXTRACT_AND_RUN 1
      --setenv UNIDAW_SOURCE_DIR "$ROOT")
for d in bin sbin lib lib64 lib32; do
    if [ -L "$SYS/$d" ]; then args+=(--symlink "$(readlink "$SYS/$d")" "/$d")
    elif [ -d "$SYS/$d" ]; then args+=(--ro-bind "$SYS/$d" "/$d"); fi
done

bwrap "${args[@]}" /usr/bin/python3 -I - <<'EOF'
import json, os, signal, subprocess, sys, time, urllib.request

APP = "/opt/unidaw.AppImage"
PORT = 7400
fails = []

def check(ok, msg):
    print(("ok: " if ok else "FAIL: ") + msg, flush=True)
    if not ok:
        fails.append(msg)

def get(path, timeout=5):
    with urllib.request.urlopen(f"http://127.0.0.1:{PORT}{path}", timeout=timeout) as r:
        return r.status, r.read()

check(not os.path.exists(os.environ["UNIDAW_SOURCE_DIR"]) and not os.path.exists(os.path.expanduser("~/dev")), "no source folder in the sandbox")

r = subprocess.run([APP, "--help"], capture_output=True, text=True, timeout=120)
check(r.returncode == 0 and "Usage:" in r.stdout and "fetch-models" in r.stdout, f"--help: {r.returncode} {r.stdout[:80]!r} {r.stderr[-300:]!r}")
r = subprocess.run([APP, "licenses"], capture_output=True, text=True, timeout=120)
check(r.returncode == 0 and "Affero" in r.stdout and "commit" in r.stdout, f"licenses: {r.stdout[:200]!r}")

# eigene Prozessgruppe: SIGINT an die Gruppe wie Ctrl+C im Terminal
starter = subprocess.Popen([APP, "--port", str(PORT), "--no-open"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True, start_new_session=True)
up = False
for _ in range(600):
    try:
        up = get("/health", timeout=1)[0] == 200
    except Exception:
        pass
    if up or starter.poll() is not None:
        break
    time.sleep(0.1)
check(up, "unidaw answers on /health")
if not up:
    print(starter.stdout.read() if starter.poll() is not None else "", flush=True)
    err = os.path.expanduser(f"~/.local/state/unidaw/{PORT}/unidaw.err")
    if os.path.exists(err):
        print(open(err).read()[-2000:])
    sys.exit(1)

token = open(os.path.expanduser(f"~/.config/unidaw/api-token-{PORT}")).read().strip()

def api(op, **kw):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}/command", data=json.dumps(dict(op=op, **kw)).encode(),
                                 headers={"Authorization": f"Bearer {token}"}, method="POST")
    with urllib.request.urlopen(req, timeout=120) as r:
        return json.loads(r.read())

status, body = get("/ui")
check(status == 200 and b"seedPanel" in body and b"dawd-token" in body, f"UI loads: {status} {len(body)} bytes")
status, _ = get("/vendor/webawesome/dist/webawesome.loader.js")
check(status == 200, "vendor files next to the UI")

libs = api("sampleLibraries").get("libraries", [])
check(bool(libs) and libs[0].get("builtin") is True and libs[0].get("files", 0) > 0, f"built-in kits: {libs[:1]}")
plugins = api("listPlugins").get("plugins", [])
check(any("purpose" in p for p in plugins), "plugin profiles from the AppImage")
r = api("audioToMidi", file="/tmp/none.wav")
check(r.get("ok") is False and "/home/user/.local/share/unidaw/models" in r.get("error", "") and "fetch-models" in r.get("error", ""), f"models looked up in the user folder: {r.get('error')}")

proj = "/home/user/song"
os.makedirs(proj)
steps = [("treeSeed", dict(dir=proj, label="appimage")),
         ("addSection", dict(name="M", length=4)),
         ("createTrack", dict(type="midi", instrument="4OSC"))]
ok = True
for op, kw in steps:
    r = api(op, **kw)
    ok = ok and r.get("ok") is True
    if op == "createTrack":
        track = r.get("index")
check(ok, "project with a 4OSC track")
r = api("addMidiClip", track=track, section="M", notes=[{"pitch": 60, "start": 0, "length": 2}])
check(r.get("ok") is True, f"notes: {r}")
out = os.path.join(proj, "render.wav")
r = api("render", file=out)
peak = api("peak").get("peak", 0)
check(r.get("ok") is True and os.path.getsize(out) > 100000 and peak > 0.01, f"render audible: {r.get('ok')} peak {peak:.3f}")

# Bibliotheken des laufenden unidaw: keine, die das AppImage mitbringt, vom
# System (was alsa-lib und JACK per dlopen nachladen, kommt vom System)
def exe_of(p):
    try:
        return os.readlink(f"/proc/{p}/exe")
    except OSError:
        return ""
pid = next(int(p) for p in os.listdir("/proc") if p.isdigit() and os.path.basename(exe_of(p)) == "unidaw")
exe = os.readlink(f"/proc/{pid}/exe")
maps = {l.split()[-1] for l in open(f"/proc/{pid}/maps") if l.rstrip().endswith(".so") or ".so." in l}
bundled = {n.split(".so")[0] for n in os.listdir(os.path.join(os.path.dirname(os.path.dirname(exe_of(pid))), "lib"))}
system = sorted(m for m in maps if m.startswith("/usr/") and os.path.basename(m).split(".so")[0] in bundled)
check(exe.endswith("/usr/bin/unidaw") and not exe.startswith("/usr/"), f"unidaw runs from the AppImage: {exe}")
check(any("libonnxruntime" in m for m in maps), "ONNX Runtime loaded")
check(len(bundled) > 5 and not system, f"bundled libraries not loaded from the system: {system}")

# C++-Laufzeit (usr/lib/cxx): die des Systems nur, wenn sie neuer ist
import re
def newest_cxx(path):
    try:
        data = open(path, "rb").read()
    except OSError:
        return -1
    return max((int(v) for v in re.findall(rb"GLIBCXX_3\.4\.([0-9]+)", data)), default=-1)
libdir = os.path.dirname(os.path.dirname(exe)) + "/lib"
sys_cxx = next((f for f in ("/usr/lib/x86_64-linux-gnu/libstdc++.so.6", "/usr/lib64/libstdc++.so.6", "/usr/lib/libstdc++.so.6")
                if os.path.exists(f)), "")
want_system = newest_cxx(sys_cxx) > newest_cxx(libdir + "/cxx/libstdc++.so.6")
loaded = next((m for m in maps if "/libstdc++.so" in m), "")
check(loaded.startswith(libdir + "/cxx/") != want_system,
      f"libstdc++ from {'the system (newer)' if want_system else 'the AppImage'}: {loaded}")

os.killpg(starter.pid, signal.SIGINT)
try:
    starter.wait(timeout=30)
    gone = True
except subprocess.TimeoutExpired:
    gone = False
time.sleep(0.5)
check(gone and not os.path.exists(f"/proc/{pid}"), "Ctrl+C stops starter and unidaw")

print(f"{'FAILED' if fails else 'all ok'}: {len(fails)} failure(s)")
sys.exit(1 if fails else 0)
EOF
