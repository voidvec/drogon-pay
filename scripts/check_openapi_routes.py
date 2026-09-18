#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""OpenAPI contract gate (CI gate + local check).

Two independent rule sets over the pay server's HTTP surface:

  LINT    the spec is internally coherent: parses under the indentation subset
          the route check relies on, has unique operationIds, every $ref
          resolves, and every operation states its auth posture.

  ROUTES  the spec and the code agree on what exists. Source A is the set of
          routes the binary actually registers (PayPlugin::registerHttpHandlers
          plus the example host's ADD_METHOD_TO macros); source B is the set of
          path+method pairs in examples/pay-server/openapi.yaml. Any one-sided
          difference fails, as does a disagreement about whether a route is
          API-key authenticated.

Standard library only: the CI runners have no PyYAML, and a gate that cannot
run there is not a gate. The YAML reader below therefore understands the narrow
subset the shipped spec uses (block mappings, block scalars, comments) and
nothing more -- if someone introduces flow mappings at path level the reader
says so instead of silently mis-parsing.

Exit code 0 = pass; 1 = violations printed one per line.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PLUGIN_CC = REPO_ROOT / "libs/drogon-pay/src/PayPlugin.cc"
HOST_CONTROLLER_DIR = REPO_ROOT / "examples/pay-server/controllers"
SPEC = REPO_ROOT / "examples/pay-server/openapi.yaml"

HTTP_METHODS = ("get", "post", "put", "patch", "delete", "head", "options", "trace")

# Routes that may legally exist on one side only. Every entry needs a reason;
# an entry without one is itself a violation (see check_route_parity).
EXCLUSIONS: dict[str, str] = {
    # "OPTIONS /api/pay/example": "why this is allowed to diverge",
}


class SpecError(Exception):
    """The spec uses YAML we refuse to guess about."""


# ---------------------------------------------------------------- spec reader


def _significant_lines(text: str):
    """Yield (indent, stripped_line) skipping blanks, comments, block scalars."""
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        raw = lines[i].rstrip()
        stripped = raw.lstrip(" ")
        indent = len(raw) - len(stripped)
        i += 1
        if not stripped or stripped.startswith("#"):
            continue
        yield indent, stripped
        # A value ending in | or > opens a block scalar: everything indented
        # deeper than that key belongs to the scalar, not to the mapping.
        if stripped.endswith(("|", "|-", "|+", ">", ">-", ">+")):
            for j in range(i, len(lines)):
                probe = lines[j]
                if probe.strip() and (len(probe) - len(probe.lstrip(" "))) <= indent:
                    i = j
                    break
            else:
                i = len(lines)


def parse_spec_paths(text: str) -> dict[str, dict[str, int]]:
    """Return {path: {method: indent_of_operation_block}} for the paths: block."""
    found: dict[str, dict[str, int]] = {}
    in_paths = False
    current_path: str | None = None
    for indent, line in _significant_lines(text):
        if indent == 0:
            in_paths = line == "paths:"
            current_path = None
            if line.rstrip(":") == "paths" and not line.endswith(":"):
                raise SpecError("paths: must be a block mapping")
            continue
        if not in_paths:
            continue
        if indent == 2:
            if not line.endswith(":"):
                raise SpecError(f"unexpected line inside paths: {line!r}")
            current_path = line[:-1].strip()
            if not current_path.startswith("/"):
                raise SpecError(f"path key does not start with '/': {current_path!r}")
            found[current_path] = {}
            continue
        if indent == 4 and current_path is not None:
            key = line[:-1].strip() if line.endswith(":") else line
            if key in HTTP_METHODS:
                found[current_path][key] = indent
    if not found:
        raise SpecError("spec has no paths: block")
    return found


def _operation_blocks(text: str) -> list[tuple[str, str, list[str]]]:
    """Return (path, method, body_lines) for every operation in paths:."""
    ops: list[tuple[str, str, list[str]]] = []
    path: str | None = None
    method: str | None = None
    body: list[str] = []
    in_paths = False

    def flush():
        if method is not None and path is not None:
            ops.append((path, method, body.copy()))

    for indent, line in _significant_lines(text):
        if indent == 0:
            flush_path = line == "paths:"
            if in_paths:
                flush()
            in_paths, path, method, body = flush_path, None, None, []
            continue
        if not in_paths:
            continue
        if indent == 2:
            flush()
            path, method, body = line[:-1].strip(), None, []
        elif indent == 4:
            flush()
            method, body = line[:-1].strip(), []
        elif method is not None:
            body.append(line)
    flush()
    return ops


# --------------------------------------------------------------- source A: code


def _methods_from_code_fragment(fragment: str) -> set[str]:
    return {m.lower() for m in re.findall(r"drogon::([A-Za-z]+)", fragment)}


def plugin_default_base_path() -> str:
    text = PLUGIN_CC.read_text(encoding="utf-8")
    m = re.search(r'get\("base_path",\s*"([^"]*)"\)', text)
    if not m:
        raise SpecError(f"{PLUGIN_CC.relative_to(REPO_ROOT)}: cannot find the base_path default")
    return m.group(1)


def _split_ternary(expr: str):
    """Split `cond ? a : b` at top level, ignoring `::` and string contents."""
    depth = 0
    in_string = False
    qpos = None
    i = 0
    while i < len(expr):
        ch = expr[i]
        if in_string:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                in_string = False
            i += 1
            continue
        if ch == '"':
            in_string = True
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        elif ch == "?" and depth == 0 and qpos is None:
            qpos = i
        elif ch == ":" and depth == 0 and qpos is not None:
            if expr[i + 1 : i + 2] == ":" or expr[i - 1 : i] == ":":
                i += 1
                continue
            return expr[:qpos].strip(), expr[qpos + 1 : i].strip(), expr[i + 1 :].strip()
        i += 1
    return None


def _resolve(expr: str, default_base: str, variables: dict[str, str]) -> str:
    """Evaluate a registerHandler path expression for the configured default."""
    expr = " ".join(expr.split())
    if not expr:
        raise SpecError("empty path expression")
    ternary = _split_ternary(expr)
    if ternary:
        cond, when_true, when_false = ternary
        match = re.fullmatch(r'(\w+)\s*==\s*"([^"]*)"', cond)
        if not match:
            raise SpecError(f"unsupported conditional in path expression: {cond!r}")
        branch = when_true if default_base == match.group(2) else when_false
        return _resolve(branch, default_base, variables)
    out = ""
    for token in _split_concat(expr):
        token = token.strip()
        if not token:
            continue
        literal = re.fullmatch(r'"([^"]*)"', token)
        if literal:
            out += literal.group(1)
            continue
        # `std::string("...")` is a cast, not a value we need to model.
        cast = re.fullmatch(r"std::string\(\s*\"([^\"]*)\"\s*\)", token)
        if cast:
            out += cast.group(1)
            continue
        if token == "basePath_":
            out += default_base
            continue
        if token in variables:
            out += _resolve(variables[token], default_base, variables)
            continue
        raise SpecError(f"cannot resolve path token {token!r} in {expr!r}")
    if not out.startswith("/"):
        raise SpecError(f"resolved path {out!r} is not absolute (from {expr!r})")
    return out


def _split_concat(expr: str) -> list[str]:
    """Split on '+' outside string literals."""
    parts: list[str] = []
    current: list[str] = []
    in_string = False
    for ch in expr:
        if ch == '"':
            in_string = not in_string
        if ch == "+" and not in_string:
            parts.append("".join(current))
            current = []
            continue
        current.append(ch)
    parts.append("".join(current))
    return parts


def code_routes() -> tuple[dict[str, bool], dict[str, set[str]]]:
    """Return ({'METHOD /path': is_api_key_protected}, {'/path': {methods}})."""
    protected: dict[str, bool] = {}
    per_path: dict[str, set[str]] = {}

    default_base = plugin_default_base_path()
    text = PLUGIN_CC.read_text(encoding="utf-8")

    # Path arguments may name a local `const std::string` (the QR route does),
    # so collect those declarations and resolve them through the same rules.
    variables = {
        m.group(1): m.group(2)
        for m in re.finditer(r"const std::string (\w+)\s*=\s*([^;]+);", text)
    }

    for m in re.finditer(r"app\.registerHandler\(([^;]*)\)\s*;", text, re.S):
        block = m.group(1)
        parts = _split_top_level(block)
        if len(parts) < 3:
            raise SpecError(f"unexpected registerHandler call shape: {block[:60]!r}")
        path = _resolve(parts[0], default_base, variables)
        handler = parts[1].strip()
        methods = _methods_from_code_fragment(parts[2])
        if not methods:
            raise SpecError(f"registerHandler for {path} declares no methods")
        is_protected = handler.startswith("authed(")
        for verb in methods:
            key = f"{verb.upper()} {path}"
            protected[key] = is_protected
            per_path.setdefault(path, set()).add(verb)

    for ctrl in sorted(HOST_CONTROLLER_DIR.glob("*.h")):
        text = ctrl.read_text(encoding="utf-8")
        for m in re.finditer(
            r"ADD_METHOD_TO\(\s*\w+::(\w+)\s*,\s*\"([^\"]+)\"\s*,\s*([^)]*)\)", text
        ):
            path = m.group(2)
            verbs = set()
            for raw in m.group(3).split(","):
                verb = raw.strip().strip('"').lower()
                if verb in HTTP_METHODS:
                    verbs.add(verb)
            if not verbs:
                raise SpecError(f"{ctrl.name}: ADD_METHOD_TO for {path} declares no method")
            for verb in verbs:
                key = f"{verb.upper()} {path}"
                protected.setdefault(key, False)
                per_path.setdefault(path, set()).add(verb)
    return protected, per_path


def _split_top_level(block: str) -> list[str]:
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    in_string = False
    escaped = False
    for ch in block:
        if in_string:
            current.append(ch)
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                in_string = False
            continue
        if ch == '"':
            in_string = True
            current.append(ch)
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(ch)
    if current:
        parts.append("".join(current))
    return [p.strip() for p in parts if p.strip()]


# ------------------------------------------------------------- source B: spec


def spec_routes(paths: dict[str, dict[str, int]]) -> dict[str, set[str]]:
    out: dict[str, set[str]] = {}
    for path, ops in paths.items():
        out[path] = set(ops)
    return out


def _body_value(body: list[str], key: str) -> str | None:
    for line in body:
        if line.startswith(key + ":"):
            return line[len(key) + 1 :].strip()
    return None


def _has_key(body: list[str], key: str) -> bool:
    return any(line.startswith(key + ":") for line in body)


# --------------------------------------------------------------------- checks


def _component_scheme_names(text: str) -> set[str]:
    """Names declared under components.securitySchemes."""
    names: set[str] = set()
    inside = False
    for indent, line in _significant_lines(text):
        if indent == 2:
            inside = line == "securitySchemes:"
        elif indent == 4 and inside and line.endswith(":"):
            names.add(line[:-1].strip())
    return names


def check_lint(text: str) -> list[str]:
    errors: list[str] = []

    if not re.search(r"^openapi:\s*3\.", text, re.M):
        errors.append("[lint openapi-version] spec must declare openapi: 3.x")
    if not re.search(r"^info:", text, re.M):
        errors.append("[lint info] spec has no info: block")
    elif not re.search(r"^  version:", text, re.M):
        errors.append("[lint info] info: block has no version")

    seen_ops: dict[str, str] = {}
    declared_schemes = _component_scheme_names(text)
    global_security = bool(re.search(r"^security:", text, re.M))

    for path, method, body in _operation_blocks(text):
        where = f"{method.upper()} {path}"
        op_id = _body_value(body, "operationId")
        if not op_id:
            errors.append(f"[lint operationId] {where} has no operationId")
        elif op_id in seen_ops:
            errors.append(
                f"[lint operationId] duplicate {op_id!r}: {seen_ops[op_id]} and {where}"
            )
        else:
            seen_ops[op_id] = where
        if not _body_value(body, "summary"):
            errors.append(f"[lint summary] {where} has no summary")
        if not _has_key(body, "responses"):
            errors.append(f"[lint responses] {where} declares no responses")
        for line in body:
            key = line.split(":")[0].strip().strip('"')
            if re.fullmatch(r"\d{3}", key) and not 100 <= int(key) <= 599:
                errors.append(f"[lint responses] {where}: status {key} is not valid")
        sec = _body_value(body, "security")
        if sec is None and not global_security:
            errors.append(
                f"[lint security] {where} declares no security and the spec has no "
                f"global security block"
            )
        if sec and sec != "[]":
            for scheme in re.findall(r"(\w+):\s*\[\]", sec):
                if scheme not in declared_schemes:
                    errors.append(f"[lint security] {where} uses undeclared scheme {scheme}")

    for ref in sorted(set(re.findall(r"\$ref:\s*'?(#/[\w/\-]+)'?", text))):
        parts = ref[len("#/") :].split("/")
        if parts[:2] != ["components", "schemas"] and len(parts) < 3:
            errors.append(f"[lint ref] only #/components/... refs are supported: {ref}")
            continue
        name = parts[-1]
        if not re.search(rf"^    {re.escape(name)}:\s*$", text, re.M):
            errors.append(f"[lint ref] {ref} has no definition under components")

    for key, reason in EXCLUSIONS.items():
        if not reason.strip():
            errors.append(f"[lint exclusions] EXCLUSIONS entry {key} has no reason")
    return errors


def check_route_parity(
    text: str, paths: dict[str, dict[str, int]]
) -> list[str]:
    errors: list[str] = []
    protected, code_paths = code_routes()
    spec_paths = spec_routes(paths)

    code_keys = {f"{v.upper()} {p}" for p, verbs in code_paths.items() for v in verbs}
    spec_keys = {f"{v.upper()} {p}" for p, verbs in spec_paths.items() for v in verbs}

    for key in sorted(code_keys - spec_keys):
        if key in EXCLUSIONS:
            continue
        errors.append(f"[routes missing-from-spec] {key} is registered by code but absent")
    for key in sorted(spec_keys - code_keys):
        if key in EXCLUSIONS:
            continue
        errors.append(f"[routes missing-from-code] {key} is documented but not registered")
    for key in sorted(EXCLUSIONS):
        if key not in code_keys and key not in spec_keys:
            errors.append(f"[routes stale-exclusion] {key} no longer diverges; drop it")

    # Auth posture: a documented route must state the same protection the code
    # applies, otherwise the spec advertises a route as public that actually
    # needs a key (or the reverse).
    for path, method, body in _operation_blocks(text):
        key = f"{method.upper()} {path}"
        if key not in protected:
            continue
        sec = _body_value(body, "security")
        declares_public = sec == "[]"
        if method == "options":
            if not declares_public:
                errors.append(
                    f"[routes auth] OPTIONS {path} must declare security: [] "
                    f"(preflight bypasses the API-key check)"
                )
            continue
        if protected[key] and declares_public:
            errors.append(
                f"[routes auth] {key} is API-key protected in code but public in the spec"
            )
        if not protected[key] and not declares_public:
            errors.append(
                f"[routes auth] {key} is unprotected in code but the spec relies on the "
                f"global security block"
            )
    return errors


# ------------------------------------------------------------------------ main


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--spec", type=Path, default=SPEC)
    ap.add_argument("--lint-only", action="store_true", help="skip the code/spec route diff")
    ap.add_argument("--routes-only", action="store_true", help="skip the structural lint")
    ap.add_argument("--print-routes", action="store_true", help="dump the parsed route sets")
    args = ap.parse_args(argv)

    try:
        text = args.spec.read_text(encoding="utf-8")
        paths = parse_spec_paths(text)
    except (OSError, SpecError) as exc:
        print(f"OpenAPI contract gate FAILED: {exc}")
        return 1

    if args.print_routes:
        _, code_paths = code_routes()
        print("# registered by code")
        for path, verbs in sorted(code_paths.items()):
            print(f"{path}: {','.join(sorted(verbs))}")
        print("# documented")
        for path, verbs in sorted(paths.items()):
            print(f"{path}: {','.join(sorted(verbs))}")
        return 0

    violations: list[str] = []
    if not args.routes_only:
        violations += check_lint(text)
    if not args.lint_only:
        violations += check_route_parity(text, paths)

    if violations:
        print(f"OpenAPI contract gate FAILED ({len(violations)} violation(s)):")
        for v in violations:
            print("  " + v)
        return 1
    scope = "lint + routes" if not (args.lint_only or args.routes_only) else (
        "lint" if args.lint_only else "routes"
    )
    total = sum(len(verbs) for verbs in paths.values())
    suffix = "" if args.lint_only else ", code and spec agree"
    print(
        f"OpenAPI contract gate passed ({scope}): {len(paths)} paths / {total} "
        f"operations documented{suffix}."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
