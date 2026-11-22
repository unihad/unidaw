# Security policy

## Supported versions

Only the latest release gets security fixes.

## Report a vulnerability

Please do **not** open a public issue. Report it privately through
GitHub: [Report a vulnerability](../../security/advisories/new) (tab
"Security" of this repository).

Please include the unidaw version, what an attacker can do, and the steps
to repeat it.

unidaw is made by one person. There is no promise of a reply or a fix
within a set time. Security problems get priority over other work.

## Scope

unidaw runs on your own computer. These parts are in scope:

- The HTTP API: it listens on 127.0.0.1 only, checks the `Host` and
  `Origin` headers, and needs the token of the running session for every
  command. A way around these checks (for example from a website in your
  browser) is a vulnerability.
- File access through the API (upload, render, project folders).
- The AppImage and the MCP server (`mcp/unidaw_mcp.py`).

Out of scope: plugins you load into unidaw. They run your own third-party
code with your user's rights, by design.
