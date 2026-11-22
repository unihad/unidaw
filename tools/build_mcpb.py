#!/usr/bin/env python3
"""Baut den MCP-Server als MCP-Bundle (.mcpb) und den Eintrag fuer die
MCP-Registry (server.json) — docs/mcp-veroeffentlichen.md.

    python3 tools/build_mcpb.py [<ausgabe-ordner>]     (Standard dawd/build/mcpb)

- Version aus dawd/CMakeLists.txt; mcp/manifest.json, mcp/.claude-plugin/
  plugin.json und VERSION in mcp/unidaw_mcp.py muessen sie tragen, sonst
  Abbruch
- Bundle = Zip mit manifest.json, unidaw_mcp.py und LICENSE, feste Zeitstempel:
  gleicher Stand = gleiche Pruefsumme
- server.json zeigt auf das Bundle im GitHub-Release v<version>
  (UNIDAW_GITHUB_REPO, Standard unihad/unidaw) und traegt seine SHA-256
"""
import hashlib
import json
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.environ.get("UNIDAW_GITHUB_REPO", "unihad/unidaw")
REGISTRY_NAME = "io.github." + REPO
SCHEMA = "https://static.modelcontextprotocol.io/schemas/2025-12-11/server.schema.json"
DESCRIPTION = "Make music with unidaw, a DAW for people and AI agents (Linux, needs a running unidaw)"


def die(msg):
    sys.exit(f"build_mcpb: {msg}")


def read_json(rel):
    with open(os.path.join(ROOT, rel)) as f:
        return json.load(f)


def cmake_version():
    with open(os.path.join(ROOT, "dawd", "CMakeLists.txt")) as f:
        m = re.search(r"^project\(unidaw VERSION ([0-9.]+)", f.read(), re.M)
    if not m:
        die("no version in dawd/CMakeLists.txt")
    return m.group(1)


def check_versions(version):
    with open(os.path.join(ROOT, "mcp", "unidaw_mcp.py")) as f:
        m = re.search(r'^VERSION = "([^"]+)"', f.read(), re.M)
    found = {
        "mcp/unidaw_mcp.py VERSION": m.group(1) if m else None,
        "mcp/manifest.json": read_json("mcp/manifest.json").get("version"),
        "mcp/.claude-plugin/plugin.json": read_json("mcp/.claude-plugin/plugin.json").get("version"),
    }
    bad = {k: v for k, v in found.items() if v != version}
    if bad:
        die(f"version {version} (dawd/CMakeLists.txt) differs: {bad}")


def build(out):
    version = cmake_version()
    check_versions(version)
    if len(DESCRIPTION) > 100:
        die("registry description longer than 100 characters")
    os.makedirs(out, exist_ok=True)
    name = f"unidaw-mcp-{version}.mcpb"
    path = os.path.join(out, name)
    files = [("manifest.json", "mcp/manifest.json"), ("unidaw_mcp.py", "mcp/unidaw_mcp.py"), ("LICENSE", "LICENSE")]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, rel in files:
            with open(os.path.join(ROOT, rel), "rb") as f:
                data = f.read()
            info = zipfile.ZipInfo(arc, date_time=(2026, 1, 1, 0, 0, 0))
            info.external_attr = (0o644 << 16)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
    with open(path, "rb") as f:
        sha = hashlib.sha256(f.read()).hexdigest()
    with open(path + ".sha256", "w") as f:
        f.write(f"{sha}  {name}\n")
    server = {
        "$schema": SCHEMA,
        "name": REGISTRY_NAME,
        "title": "unidaw",
        "description": DESCRIPTION,
        "repository": {"url": f"https://github.com/{REPO}", "source": "github", "subfolder": "mcp"},
        "websiteUrl": f"https://github.com/{REPO}",
        "version": version,
        "packages": [{
            "registryType": "mcpb",
            "identifier": f"https://github.com/{REPO}/releases/download/v{version}/{name}",
            "version": version,
            "fileSha256": sha,
            "transport": {"type": "stdio"},
        }],
    }
    with open(os.path.join(out, "server.json"), "w") as f:
        json.dump(server, f, indent=2)
        f.write("\n")
    print(path)
    print(os.path.join(out, "server.json"))


if __name__ == "__main__":
    build(os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(ROOT, "dawd", "build", "mcpb"))
