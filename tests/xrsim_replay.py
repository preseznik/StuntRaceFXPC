"""Run a 4WD course fixture twice in one xr-sim session, restarting with R."""

import argparse
import ctypes
from ctypes import wintypes
import json
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


FIXTURES = Path(__file__).resolve().parent / "fixtures"
RESULTS = {"easy-ride": (1637, 3, 58), "aqua-tunnel": (2206, 4, 18)}


def wait_for(game, description, probe, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if game.poll() is not None:
            raise RuntimeError(f"Game exited with {game.returncode} while waiting for {description}")
        try:
            value = probe()
        except (FileNotFoundError, PermissionError, json.JSONDecodeError):
            # xr-sim publishes files while the game is running.
            value = None
        if value:
            return value
        time.sleep(0.1)
    raise TimeoutError(f"Timed out waiting for {description}")


def game_window(pid):
    user32 = ctypes.WinDLL("user32", use_last_error=True)
    find = user32.FindWindowExW
    find.argtypes = [wintypes.HWND, wintypes.HWND, wintypes.LPCWSTR, wintypes.LPCWSTR]
    find.restype = wintypes.HWND
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    window = None
    while True:
        window = find(None, window, "StuntRaceFXGame", None)
        if not window:
            raise RuntimeError(f"No companion window for game PID {pid}")
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(window, ctypes.byref(owner))
        if owner.value == pid:
            return user32, window


def post_to_game(pid, message, key=0):
    user32, window = game_window(pid)
    if not user32.PostMessageW(window, message, key, 0):
        raise ctypes.WinError(ctypes.get_last_error())


def caption_ends_with(game, suffix):
    user32, window = game_window(game.pid)
    caption = ctypes.create_unicode_buffer(256)
    user32.GetWindowTextW(window, caption, len(caption))
    return caption.value.endswith(suffix)


def capture(game, run, tag, *, panel=False):
    command = run / "command.tmp"
    command.write_text(f"capture size 700 740\nshot {tag}\n", encoding="ascii")
    command.replace(run / "command.txt")
    folder = run / "capture"
    shot = wait_for(game, f"{tag} capture", lambda: json.loads((folder / f"{tag}.json").read_bytes()))
    layers = shot["layers"]
    expected = ["quad"] if panel else ["projection", "quad"]
    if (shot["sessionState"] != "FOCUSED" or [layer["type"] for layer in layers] != expected
            or (not panel and layers[0]["viewCount"] != 2) or shot["pacing"]["hz"] != 90):
        raise RuntimeError(f"{tag}: expected focused 90 Hz output with {expected}")
    if shot["gate"]["discarded"] or shot["gate"]["outOfOrder"]:
        raise RuntimeError(f"{tag}: xr-sim reported invalid frame submission order: {shot['gate']}")
    eyes = [(folder / f"{tag}_{eye}.png").read_bytes() for eye in ("left", "right")]
    if not all(eye.startswith(b"\x89PNG\r\n\x1a\n") for eye in eyes) or eyes[0] == eyes[1]:
        raise RuntimeError(f"{tag}: missing or identical eye images")
    print(f"Captured {tag} at frame {shot['frameIndex']}", flush=True)
    return eyes


def check_aqua_sky(eyes, tag, underwater):
    from PIL import Image

    for side, data in zip(("left", "right"), eyes):
        with Image.open(io.BytesIO(data)) as image:
            sky = image.convert("RGB").crop((0, 0, image.width, image.height // 3))
        colors = sky.getcolors(sky.width * sky.height)
        daylight = sum(n for n, (r, g, b) in colors if r >= 125 and g >= 165 and b >= 195)
        water = sum(n for n, (r, g, b) in colors if r <= 20 and g >= 65 and b >= 125)
        print(f"{tag} {side} sky: {daylight} daylight pixels, {water} underwater pixels", flush=True)
        expected, other = (water, daylight) if underwater else (daylight, water)
        if expected < sky.width * sky.height // 50 or expected <= other:
            raise RuntimeError(f"{tag}: wrong backdrop palette in {side} eye")


def replay(game, run, course):
    log = run / "openxr-last-drive.log"
    phase, place, seconds = RESULTS[course]
    first = [f"Finished three laps at phase {phase}", f"Result: 3 laps, place {place}, {seconds} seconds left"]
    restart = f"Restart at phase {phase}"

    def events():
        return [line for line in log.read_text().splitlines()
                if line.startswith(("Finished three laps", "Result:", "Restart at phase"))]

    def check_events(expected):
        actual = events()
        if actual != expected[:len(actual)]:
            raise RuntimeError(f"Unexpected race events: {actual}")
        return actual == expected

    def focused_frames():
        state = json.loads((run / "state.json").read_bytes())
        if (state["pid"] == game.pid and state["runtime"] == "xr-sim" and state["graphics"] == "D3D11"
                and state["sessionState"] == "FOCUSED"):
            return state["layeredFrames"]
        return 0

    wait_for(game, "settled starting grid", lambda: focused_frames() >= 45, 30)
    grid = capture(game, run, "grid")
    if course == "aqua-tunnel":
        check_aqua_sky(grid, "grid", False)
        for phase_number, mode, tag in ((321, "transition", "entry"), (340, "underwater", "underwater"),
                                        (713, "transition", "exit"), (741, "surface", "resurfaced")):
            event = f"Aqua backdrop {mode} at phase {phase_number}"
            wait_for(game, event, lambda: event in log.read_text().splitlines(), 70)
            if mode != "transition":
                # Allow the recovered palette fade to finish before sampling the panorama.
                start = wait_for(game, "focused xr-sim frame", focused_frames)
                wait_for(game, tag, lambda: focused_frames() >= start + 145)
            eyes = capture(game, run, tag)
            if mode != "transition":
                check_aqua_sky(eyes, tag, mode == "underwater")
    wait_for(game, "first three-lap finish", lambda: check_events(first), 180)
    wait_for(game, "first results board", lambda: caption_ends_with(game, "RACE RESULTS \u2014 Quick Race"))
    time.sleep(2)  # The cartridge lap tally takes 90 video ticks.
    finish = capture(game, run, "finish", panel=True)
    if finish == grid:
        raise RuntimeError("Finish rendered the starting grid")

    post_to_game(game.pid, 0x0100, ord("R"))  # WM_KEYDOWN on the companion window
    wait_for(game, "post-finish restart", lambda: check_events(first + [restart]))
    # The first native updates sort the four cars into their grid standings.
    restarted = wait_for(game, "focused restart frame", focused_frames)
    wait_for(game, "settled restarted grid", lambda: focused_frames() >= restarted + 45)
    if capture(game, run, "restarted-grid") != grid:
        raise RuntimeError("Restart did not reproduce the initial stereo grid")
    wait_for(game, "second three-lap finish", lambda: check_events(first + [restart] + first), 200)
    wait_for(game, "second results board", lambda: caption_ends_with(game, "RACE RESULTS \u2014 Quick Race"))
    time.sleep(2)
    if capture(game, run, "second-finish", panel=True) != finish:
        raise RuntimeError("Second finish did not reproduce the first stereo finish")
    def tap(key):
        post_to_game(game.pid, 0x0100, key)
        time.sleep(.15)
        post_to_game(game.pid, 0x0101, key)
        time.sleep(.15)

    tap(0x0D)
    wait_for(game, "race again or choose course", lambda: caption_ends_with(game, "Race again"))
    tap(0x27)
    tap(0x0D)
    wait_for(game, "course picker", lambda: caption_ends_with(game, "Quick Race"))
    post_to_game(game.pid, 0x0010)  # WM_CLOSE
    if game.wait(timeout=10) != 0:
        raise RuntimeError(f"Game exited with {game.returncode}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("game", "world", "runtime_dll"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--course", choices=RESULTS, default="easy-ride")
    parser.add_argument("--private-desktop", type=Path)
    parser.add_argument("--small-views", action="store_true", help="Use the offscreen software-rendering profile")
    args = parser.parse_args()
    sys.path.insert(0, str(FIXTURES.parents[1] / "tools"))
    from validate_game_flow import run_on_private_desktop
    from validate_seamless_navigation import xr_navigation_environment
    if args.private_desktop:
        run_on_private_desktop(args.private_desktop)
    if not os.environ.get("SRF_ROM"):
        print("Set SRF_ROM to the local USA Rev 1 ROM to run this test")
        return 77
    rom = Path(os.environ["SRF_ROM"]).resolve(strict=True)
    runtime = args.runtime_dll.resolve(strict=True)
    command = [args.game.resolve(strict=True), "--world", args.world.resolve(strict=True), "--race",
               "--rom", rom, "--vr", "--course", args.course, "--vr-hz", "90", "--replay-input",
               FIXTURES / (args.course.replace("-", "_") + "_clean_inputs.csv")]
    run = Path(tempfile.mkdtemp(prefix=f"xrsim-{args.course}-", dir=Path.cwd()))
    print(f"Game logs and stereo captures: {run}", flush=True)
    env = dict(os.environ, **xr_navigation_environment(run, runtime, small_views=args.small_views))
    if args.small_views:
        command += ["--vr-scale", "50", "--width", "640", "--height", "480"]
    with (run / "game-output.log").open("wb") as output:
        game = subprocess.Popen(command, cwd=run, env=env, stdout=output, stderr=subprocess.STDOUT)
        try:
            replay(game, run, args.course)
        finally:
            if game.poll() is None:
                game.kill()
                game.wait()
    print("Both three-lap finishes and the post-finish restart passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
