#!/usr/bin/env python3

import argparse
import json
import pathlib
import re
import sys


def descriptor_commands(api_dir):
    commands = set()
    for path in pathlib.Path(api_dir).glob("remote-*.json"):
        with path.open(encoding="utf-8") as f:
            descriptor = json.load(f)
        if descriptor.get("hook") == "cb_cmds":
            commands.add(descriptor["name"])
    return commands


def direct_run_commands(path):
    text = pathlib.Path(path).read_text(encoding="utf-8")
    return set(re.findall(r'run\("([^"]+)"', text))


def report_missing(label, missing):
    if not missing:
        return
    print(f"{label} is missing direct coverage for {len(missing)} command(s):")
    for command in sorted(missing):
        print(f"  {command}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--api-dir", required=True)
    parser.add_argument("--unit-source", required=True)
    parser.add_argument("--db-source", required=True)
    args = parser.parse_args()

    commands = descriptor_commands(args.api_dir)
    if not commands:
        print(f"no cb_cmds API descriptors found in {args.api_dir}", file=sys.stderr)
        return 1

    unit_missing = commands - direct_run_commands(args.unit_source)
    db_missing = commands - direct_run_commands(args.db_source)

    report_missing("unit tests", unit_missing)
    report_missing("DB integration tests", db_missing)

    if unit_missing or db_missing:
        return 1

    print(f"cb_cmds direct unit coverage: {len(commands)}/{len(commands)}")
    print(f"cb_cmds direct DB coverage: {len(commands)}/{len(commands)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
