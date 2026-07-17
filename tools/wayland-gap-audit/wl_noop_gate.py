#!/usr/bin/env python3
"""wl_noop_gate — build-gate check for unmarked empty Wayland request handlers.

BRO-216 was a silent MISSING global; its inverse is advertising an interface but
leaving a request handler as a silent no-op. This gate scans waylandd.c for
static request handlers whose body does *nothing observable* — only `(void)`
parameter casts — and are NOT one of:

  * a destructor (body calls wl_resource_destroy), which is a correct no-op;
  * already marked with WAYLAND_UNIMPLEMENTED / WAYLAND_INTENTIONAL_NOOP;
  * on the explicit allowlist of genuine protocol-defined no-ops.

Every such handler should either DO its minimum contract, POST the protocol
error, or be explicitly marked so a client invoking it becomes observable at
runtime (see the markers in waylandd.c and artifacts/brook-api-gap-audit-plan.md
P1.1). Default: report + exit 0. `--strict`: exit non-zero if any are found (CI).

Heuristic, not a full C parser: it brace-matches top-level `static void name(...)`
bodies and inspects their statements. Good enough for waylandd's hand-written
dispatch handlers; false positives go on the allowlist.
"""
import argparse
import re
import sys

# Handlers whose empty body is genuinely correct and need not be marked.
# Keyed by function name. Destructors are auto-detected (wl_resource_destroy).
ALLOWLIST = {
    # e.g. requests the protocol defines as advisory/no-op for a server with no
    # IME/compositor-side state. Add with a one-line justification.
}

FUNC_RE = re.compile(r'^static\s+void\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(', re.M)
MARKER_RE = re.compile(r'WAYLAND_(UNIMPLEMENTED|INTENTIONAL_NOOP)\s*\(')
# A "does something" statement: anything that isn't a (void) cast, a comment,
# a brace, or whitespace.
VOIDCAST_RE = re.compile(r'^\s*\(void\)')


def _find_body(src, open_paren_idx):
    """Given the index of a function's '(', return (params, body_text) or None."""
    # find matching ')'
    depth = 0
    i = open_paren_idx
    while i < len(src):
        if src[i] == '(':
            depth += 1
        elif src[i] == ')':
            depth -= 1
            if depth == 0:
                break
        i += 1
    # find the opening brace after the params
    j = src.find('{', i)
    if j < 0:
        return None
    depth = 0
    k = j
    while k < len(src):
        if src[k] == '{':
            depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0:
                break
        k += 1
    return src[j + 1:k]


def _is_empty_noop(body):
    """True if the body contains no observable statement (only (void) casts,
    comments, braces, whitespace)."""
    # strip block comments and line comments
    body = re.sub(r'/\*.*?\*/', '', body, flags=re.S)
    body = re.sub(r'//[^\n]*', '', body)
    # remove every (void)<expr>; cast (they may share a line with real code)
    body = re.sub(r'\(void\)\s*[A-Za-z_][A-Za-z0-9_]*\s*;', '', body)
    # anything left that isn't whitespace or a brace = an observable statement
    return re.sub(r'[\s{}]', '', body) == ''


def scan(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    findings = []
    for m in FUNC_RE.finditer(src):
        name = m.group(1)
        # Restrict to actual Wayland request handlers: their signature always
        # takes a (struct wl_client *, struct wl_resource *, ...). This excludes
        # signal handlers, bind callbacks, and other helpers.
        sig_end = src.find('{', m.end())
        params = src[m.end():sig_end] if sig_end > 0 else ""
        if 'wl_resource' not in params:
            continue
        body = _find_body(src, m.end() - 1)
        if body is None:
            continue
        if 'wl_resource_destroy' in body:
            continue  # destructor: correct no-op
        if MARKER_RE.search(body):
            continue  # explicitly marked
        if name in ALLOWLIST:
            continue
        if _is_empty_noop(body):
            findings.append(name)
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", nargs="?", default="tools/waylandd-pkg/waylandd.c",
                    help="path to waylandd.c")
    ap.add_argument("--strict", action="store_true",
                    help="exit non-zero if any unmarked empty handler is found")
    args = ap.parse_args()

    findings = scan(args.source)
    if findings:
        print(f"wl_noop_gate: {len(findings)} unmarked empty request handler(s) "
              f"in {args.source}:")
        for n in findings:
            print(f"  - {n}  (mark with WAYLAND_UNIMPLEMENTED/INTENTIONAL_NOOP, "
                  f"implement, or allowlist)")
        if args.strict:
            return 1
    else:
        print(f"wl_noop_gate: OK — every empty request handler in {args.source} "
              f"is a destructor, marked, or allowlisted.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
