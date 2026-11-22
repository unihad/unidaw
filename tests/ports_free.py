#!/usr/bin/env python3
"""Exit 0, wenn alle Ports einer Suite mit Basis-Port P frei sind, sonst 1.

Eine Suite (tests/run.sh, beide Gruppen) belegt TCP P bis P + 3, P + 7, P + 9
(resources_relocated_flow in Gruppe A bzw. B), P + 20, P + 22 und UDP P + 1000 bis P + 1003 (OSC-Tests). tests/run.sh sucht damit
einen freien Basis-Port, damit zwei Suiten (Worktrees, pre-push-Hook) nicht
dieselbe Instanz benutzen — Todo 2026-11-09-pre-push-hook-rot-test-instanz-stirbt.

Aufruf: python3 tests/ports_free.py P
"""
import socket
import sys


def free(port, kind):
    s = socket.socket(socket.AF_INET, kind)
    # TIME_WAIT eines frueheren Laufs zaehlt nicht als belegt (dawd bindet
    # ebenso mit SO_REUSEADDR), ein lauschender Socket schon
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def main():
    p = int(sys.argv[1])
    tcp = [p, p + 1, p + 2, p + 3, p + 7, p + 9, p + 20, p + 22]
    udp = [p + 1000, p + 1001, p + 1002, p + 1003]
    ok = all(free(x, socket.SOCK_STREAM) for x in tcp) and all(free(x, socket.SOCK_DGRAM) for x in udp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
