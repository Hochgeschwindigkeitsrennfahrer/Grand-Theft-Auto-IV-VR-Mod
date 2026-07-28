#!/usr/bin/env python3
"""Command-line controller input for the OpenXR Simulator runtime.

This tool uses only the Python standard library and writes the same file-based
IPC consumed by the runtime. It does not activate, focus, click, or automate
the simulator window.
"""

from __future__ import annotations

import argparse
import json
import os
import tempfile
import time
from pathlib import Path
from typing import Any


COMMAND_FILE = "controller_pose_command.json"
ACK_FILE = "command_ack.json"


def default_data_dir() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise RuntimeError("LOCALAPPDATA is not set; pass --data-dir explicitly")
    return Path(local_app_data) / "OpenXR-Simulator"


def next_sequence() -> int:
    # Microseconds since the Unix epoch fit exactly in uint64 and are naturally
    # monotonic for human-paced command invocations.
    return time.time_ns() // 1_000


def atomic_write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(payload, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def wait_for_ack(data_dir: Path, sequence: int, wait_ms: int) -> dict[str, Any] | None:
    if wait_ms <= 0:
        return None
    deadline = time.monotonic() + wait_ms / 1_000.0
    ack_path = data_dir / ACK_FILE
    while time.monotonic() < deadline:
        try:
            ack = json.loads(ack_path.read_text(encoding="utf-8"))
            if int(ack.get("sequence", -1)) == sequence:
                return ack
        except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError):
            pass
        time.sleep(0.01)
    raise TimeoutError(
        f"runtime did not acknowledge controller sequence {sequence} "
        f"within {wait_ms} ms"
    )


def hand_value(name: str) -> int:
    return {"left": 0, "right": 1, "both": 2}[name]


def validate_unit(name: str, value: float) -> float:
    if not 0.0 <= value <= 1.0:
        raise ValueError(f"{name} must be between 0 and 1")
    return value


def validate_axis(name: str, value: float) -> float:
    if not -1.0 <= value <= 1.0:
        raise ValueError(f"{name} must be between -1 and 1")
    return value


def build_set_payload(args: argparse.Namespace) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "hand": hand_value(args.hand),
        "sequence": args.sequence if args.sequence is not None else next_sequence(),
        "lease_ms": args.lease_ms,
        "neutral": False,
        # A set command is always a complete input snapshot.
        "stickX": validate_axis("stick-x", args.stick_x),
        "stickY": validate_axis("stick-y", args.stick_y),
        "trigger": validate_unit("trigger", args.trigger),
        "squeeze": validate_unit("squeeze", args.squeeze),
        "primary": bool(args.primary),
        "secondary": bool(args.secondary),
        "menu": bool(args.menu),
        "thumbClick": bool(args.thumb_click),
    }
    for argument, json_name in (
        ("pos_x", "posX"),
        ("pos_y", "posY"),
        ("pos_z", "posZ"),
        ("yaw", "yaw"),
        ("pitch", "pitch"),
    ):
        value = getattr(args, argument)
        if value is not None:
            payload[json_name] = value
    return payload


def build_neutral_payload(args: argparse.Namespace) -> dict[str, Any]:
    return {
        "hand": hand_value(args.hand),
        "sequence": args.sequence if args.sequence is not None else next_sequence(),
        "lease_ms": args.lease_ms,
        "neutral": True,
        "stickX": 0.0,
        "stickY": 0.0,
        "trigger": 0.0,
        "squeeze": 0.0,
        "primary": False,
        "secondary": False,
        "menu": False,
        "thumbClick": False,
    }


def issue(data_dir: Path, payload: dict[str, Any], wait_ms: int) -> dict[str, Any]:
    command_path = data_dir / COMMAND_FILE
    atomic_write_json(command_path, payload)
    result: dict[str, Any] = {
        "status": "written",
        "path": str(command_path),
        "command": payload,
    }
    ack = wait_for_ack(data_dir, int(payload["sequence"]), wait_ms)
    if ack is not None:
        result["status"] = "acknowledged"
        result["ack"] = ack
    return result


def run_self_test() -> dict[str, Any]:
    with tempfile.TemporaryDirectory(prefix="openxr-simulator-cli-") as directory:
        data_dir = Path(directory)
        set_args = argparse.Namespace(
            hand="right",
            sequence=101,
            lease_ms=250,
            stick_x=-0.5,
            stick_y=0.75,
            trigger=0.25,
            squeeze=0.5,
            primary=True,
            secondary=False,
            menu=True,
            thumb_click=False,
            pos_x=0.2,
            pos_y=None,
            pos_z=-0.4,
            yaw=None,
            pitch=-0.3,
        )
        payload = build_set_payload(set_args)
        atomic_write_json(data_dir / COMMAND_FILE, payload)
        decoded = json.loads((data_dir / COMMAND_FILE).read_text(encoding="utf-8"))
        assert decoded == payload
        assert decoded["hand"] == 1
        assert decoded["sequence"] == 101
        assert decoded["lease_ms"] == 250
        assert decoded["primary"] is True
        assert decoded["secondary"] is False
        assert decoded["stickX"] == -0.5
        assert decoded["squeeze"] == 0.5

        neutral_args = argparse.Namespace(
            hand="both", sequence=102, lease_ms=500
        )
        neutral = build_neutral_payload(neutral_args)
        assert neutral["hand"] == 2
        assert neutral["neutral"] is True
        assert all(
            neutral[key] in (0.0, False)
            for key in (
                "stickX",
                "stickY",
                "trigger",
                "squeeze",
                "primary",
                "secondary",
                "menu",
                "thumbClick",
            )
        )

        runtime_source = (
            Path(__file__).resolve().parents[1] / "src" / "runtime.cpp"
        ).read_text(encoding="utf-8")
        assert (
            "static bool IsInputFocusForeground()" in runtime_source
            and "if (IsHeadlessMode()) return false;" in runtime_source
            and "const bool previewWindowFocused = "
            "rt::IsInputFocusForeground();" in runtime_source
        )
        assert "GetAsyncKeyState(VK_SPACE)" in runtime_source
        return {
            "status": "PASS",
            "checks": [
                "atomic command write/read",
                "full right-hand input snapshot",
                "explicit both-hands neutral snapshot",
                "sequence and lease fields",
                "headless mode excludes global keyboard controller injection",
            ],
        }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Issue controller state without controlling the simulator UI.",
        epilog=(
            "For a hidden, logically-focused runtime set "
            "OPENXR_SIMULATOR_HEADLESS=1 on the OpenXR host process. "
            "For visible mode with GTA owning focus set "
            "OPENXR_SIMULATOR_EXTERNAL_FOCUS_EXE=GTAIV.exe."
        ),
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        help=r"IPC directory (default: %%LOCALAPPDATA%%\OpenXR-Simulator)",
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    set_parser = subparsers.add_parser(
        "set", help="write one complete persistent per-hand input snapshot"
    )
    set_parser.add_argument("--hand", choices=("left", "right", "both"), required=True)
    set_parser.add_argument("--sequence", type=int)
    set_parser.add_argument(
        "--lease-ms",
        type=int,
        default=2_000,
        help="neutralize when this lease expires; 0 persists until superseded",
    )
    set_parser.add_argument("--stick-x", type=float, default=0.0)
    set_parser.add_argument("--stick-y", type=float, default=0.0)
    set_parser.add_argument("--trigger", type=float, default=0.0)
    set_parser.add_argument("--squeeze", type=float, default=0.0)
    set_parser.add_argument(
        "--primary", action=argparse.BooleanOptionalAction, default=False
    )
    set_parser.add_argument(
        "--secondary", action=argparse.BooleanOptionalAction, default=False
    )
    set_parser.add_argument(
        "--menu", action=argparse.BooleanOptionalAction, default=False
    )
    set_parser.add_argument(
        "--thumb-click", action=argparse.BooleanOptionalAction, default=False
    )
    set_parser.add_argument("--pos-x", type=float)
    set_parser.add_argument("--pos-y", type=float)
    set_parser.add_argument("--pos-z", type=float)
    set_parser.add_argument("--yaw", type=float)
    set_parser.add_argument("--pitch", type=float)
    set_parser.add_argument("--wait-ms", type=int, default=0)

    neutral_parser = subparsers.add_parser(
        "neutral", help="explicitly release all inputs on one or both hands"
    )
    neutral_parser.add_argument(
        "--hand", choices=("left", "right", "both"), default="both"
    )
    neutral_parser.add_argument("--sequence", type=int)
    neutral_parser.add_argument("--lease-ms", type=int, default=2_000)
    neutral_parser.add_argument("--wait-ms", type=int, default=0)

    subparsers.add_parser("status", help="print the most recent runtime acknowledgment")
    subparsers.add_parser("self-test", help="run filesystem/payload smoke checks")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    try:
        if args.command == "self-test":
            print(json.dumps(run_self_test(), indent=2))
            return 0

        data_dir = args.data_dir if args.data_dir is not None else default_data_dir()
        if args.command == "status":
            ack_path = data_dir / ACK_FILE
            print(ack_path.read_text(encoding="utf-8"), end="")
            return 0

        if args.lease_ms < 0:
            raise ValueError("lease-ms must be zero or greater")
        payload = (
            build_set_payload(args)
            if args.command == "set"
            else build_neutral_payload(args)
        )
        print(json.dumps(issue(data_dir, payload, args.wait_ms), indent=2))
        return 0
    except (OSError, RuntimeError, TimeoutError, ValueError) as error:
        parser.exit(2, f"error: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
