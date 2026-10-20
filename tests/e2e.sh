#!/bin/bash
# End-to-end check without the printer: run the app against a TCP sink, print
# simplex / duplex jobs, and verify page order, pause items and back rotation.
# Usage: tests/e2e.sh <scratch-dir>
set -euo pipefail
T=${1:?scratch dir}; ROOT=$(cd "$(dirname "$0")/.." && pwd); A=$ROOT/build/p1505-printer-app
PORT=8699; SINK=9199
rm -rf "$T"; mkdir -p "$T/home/.local/share" "$T/out"; cd "$T"

cat > five.ps <<'EOF'
%!PS
1 1 5 { /n exch def /Helvetica-Bold findfont 200 scalefont setfont 200 400 moveto n 1 string cvs show showpage } for
EOF
ps2pdf five.ps five.pdf
ps2pdf -dLastPage=1 five.ps one.pdf

# One file per connection.
cat > sink.py <<EOF
import socket
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", $SINK)); s.listen(4); n = 0
while True:
    c, _ = s.accept(); n += 1
    with open(f"out/conn{n}.bin", "wb") as f:
        while (d := c.recv(65536)): f.write(d)
EOF
python3 -I sink.py & SINKPID=$!
HOME=$T/home "$A" server -o server-port=$PORT -o log-file="$T/log" & APPPID=$!
trap 'kill $SINKPID $APPPID 2>/dev/null' EXIT
sleep 2
HOME=$T/home "$A" add -d t -m hp_laserjet_p1505 -v socket://127.0.0.1:$SINK -o server-port=$PORT

run() {   # run <name> <file> [options...]
  local name=$1 file=$2; shift 2
  HOME=$T/home "$A" submit -d t -o server-port=$PORT "$@" "$file" >/dev/null
  for _ in $(seq 60); do grep -q "Job $((++JOBS))\] Completed" log 2>/dev/null && break; JOBS=$((JOBS-1)); sleep 1; done
  JOBS=$((JOBS+1))
  sleep 1
  cat out/*.bin > "$name.bin"; rm -f out/*.bin
}
JOBS=0
run simplex five.pdf
run long five.pdf -o sides=two-sided-long-edge
run short five.pdf -o sides=two-sided-short-edge
run one one.pdf -o sides=two-sided-long-edge

python3 -I - "$ROOT" <<'EOF'
import sys, subprocess
sys.path.insert(0, sys.argv[1] + "/tests")
from xqxdump import parse

def pages(name):
    o = parse(open(name + ".bin", "rb").read())
    assert o.get("end"), name + ": no END_DOC"
    return o, [dict(p["items"]) for p in o["pages"]]

def ink_top(name, n):
    # Which half holds the digit: top = upright front, bottom = rotated 180.
    o = parse(open(name + ".bin", "rb").read())
    p = o["pages"][n]
    open("pg.jbg", "wb").write(p["bih"] + p["jbig"])
    subprocess.run([sys.argv[1] + "/build/test_xqx", "topbm", "pg.jbg", "pg.pbm"], check=True)
    d = open("pg.pbm", "rb").read().split(b"\n", 2)[2]
    return sum(d[:len(d)//2]) > sum(d[len(d)//2:])   # PBM: set bits = black

o, ps = pages("simplex")
assert len(ps) == 5 and all(0x20000012 not in p for p in ps), "simplex"
assert dict(o["doc"])[0x10000002] == 0

for mode, rotated in (("long", True), ("short", False)):
    o, ps = pages(mode)
    assert dict(o["doc"])[0x10000002] == 2
    assert [p[0x20000012] for p in ps] == [0, 1, 1, 2, 3, 3], mode
    assert ps[3][0x2000000E] == ps[0][0x2000000E], "blank back must be full size"
    # sent order: 1 3 5 blank 4 2 -> fronts upright, backs rotated for long edge
    assert ink_top(mode, 0), mode + " front upright"
    assert ink_top(mode, 4) != rotated, mode + " back rotation"

o, ps = pages("one")
assert len(ps) == 1 and ps[0][0x20000012] == 0, "one-page duplex"
print("e2e ok: simplex, long/short manual duplex (order, pauses, rotation), one-page duplex")
EOF
