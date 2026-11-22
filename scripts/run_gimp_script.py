#!/usr/bin/env python3
"""Run a Python script *inside* GIMP through its MCP server, then verify what it wrote.

Why this exists: the editor's icon atlas is authored by scripts/make_editor_icons.py, which has to execute inside the
running GIMP (its drawing primitives are GIMP's own anti-aliased selections). Doing that by hand — start GIMP, start the
MCP server, paste code — is how assets end up stale relative to their source. This makes regeneration one command:

    python3 scripts/run_gimp_script.py scripts/make_editor_icons.py

The script file is read here and exec'd in GIMP's PyGObject console with __name__ set to its module name, so it can be
imported for its data (icon_table()) or run as a generator. Requires GIMP running with the MCP plugin's server on
port 9877 — see the gimp-mcp-drive skill for setup; `--check` reports whether it is reachable and says what to run.

The mcp client library comes from uv, so nothing needs installing:
    uv run --with "mcp>=1.0" python scripts/run_gimp_script.py ...
"""

import argparse
import asyncio
import json
import os
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

UVX = os.path.expanduser("~/.local/bin/uvx")
DEFAULT_PORT = "9877"


async def _call(session, tool, arguments):
    result = await session.call_tool(tool, arguments)
    parts = []
    for block in result.content:
        parts.append(getattr(block, "text", None) or str(block))
    text = "\n".join(parts)
    # The GIMP plugin reports failures as a plain "Error: ..." string with is_error unset, so a status check on the
    # protocol flag alone would call every failure a success. Inspect the text.
    failed = bool(getattr(result, "isError", False)) or text.startswith("Error:")
    return text, failed


async def _run(path, port, extra_eval):
    env = {"GIMP_MCP_PORT": port, "GIMP_MCP_TIMEOUT": "600"}
    params = StdioServerParameters(command=UVX, args=["gimp-mcp-plugin@latest"], env=env)

    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            status_text, _ = await _call(session, "check_server", {})
            print("[server]", status_text.strip().replace("\n", " ")[:200])

            source = open(path).read()
            module_name = os.path.splitext(os.path.basename(path))[0]
            # One multi-line entry: the plugin execs each list item independently and strips leading indentation, so a
            # block split across items fails with "expected an indented block".
            code = (
                "import traceback\n"
                f"_src = {source!r}\n"
                f"_ns = {{'__name__': {module_name!r}, '__file__': {os.path.abspath(path)!r}}}\n"
                "try:\n"
                "    exec(compile(_src, _ns['__file__'], 'exec'), _ns)\n"
                "    print('[loaded]', _ns['__name__'])\n"
                "except Exception:\n"
                "    print('FAIL', traceback.format_exc())\n"
            )
            text, failed = await _call(session, "call_api", {"api_path": "exec",
                                                             "args": ["pyGObject-console", [code]]})
            print(text)
            if failed or "FAIL" in text:
                return 1

            run_code = (
                "import os, traceback\n"
                f"os.chdir({os.getcwd()!r})\n"
                "try:\n"
                "    _result = _ns.get('run')\n"
                "    if _result is None:\n"
                "        print('[skip] script defines no run()')\n"
                "    else:\n"
                "        print('[result]', _result())\n"
                "except Exception:\n"
                "    print('FAIL', traceback.format_exc())\n"
            )
            text, failed = await _call(session, "call_api", {"api_path": "exec",
                                                             "args": ["pyGObject-console", [run_code]]})
            print(text)
            if failed or "FAIL" in text:
                return 1

            if extra_eval:
                text, _ = await _call(session, "call_api", {"api_path": "exec",
                                                            "args": ["pyGObject-console", [extra_eval]]})
                print(text)
            return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("script", nargs="?", help="python file to execute inside GIMP")
    parser.add_argument("--port", default=os.environ.get("GIMP_MCP_PORT", DEFAULT_PORT))
    parser.add_argument("--eval", help="extra one-line python to evaluate in the same namespace afterwards")
    parser.add_argument("--check", action="store_true", help="only report whether GIMP's MCP server answers")
    args = parser.parse_args()

    if args.check:
        print(f"checking GIMP MCP on port {args.port} ...")
        return asyncio.run(_check_only(args.port))

    if not args.script or not os.path.isfile(args.script):
        parser.error("need an existing script path")
    return asyncio.run(_run(args.script, args.port, args.eval))


async def _check_only(port):
    env = {"GIMP_MCP_PORT": port}
    params = StdioServerParameters(command=UVX, args=["gimp-mcp-plugin@latest"], env=env)
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            text, failed = await _call(session, "check_server", {})
            print(text)
            return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
