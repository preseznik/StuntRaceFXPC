"""Check the live cartridge HUD in both xr-sim eyes and the Quick Race picker on a private desktop."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from validate_game_flow import BOOT, ESCAPE, START, GameFlow, run_on_private_desktop, user, wait_for, window_text
from validate_seamless_navigation import headset_capture, xr_navigation_environment


def run(args, directory):
    from PIL import Image, ImageChops, ImageStat
    env = dict(os.environ, **xr_navigation_environment(directory, args.xrsim, small_views=True))
    command = [args.preview.resolve(strict=True), "--world", args.world.resolve(strict=True),
               "--rom", args.rom.resolve(strict=True), "--progress-file", directory/"progress.sav",
               "--race", "--car", "coupe", "--vr", "--hide-race-hud", "--vr-scale", "75",
               "--width", "640", "--height", "480"]
    if args.style != "classic":
        command.append("--"+args.style)
    with (directory/"game.log").open("w") as log:
        proc = subprocess.Popen(command, cwd=directory, env=env, stdout=log, stderr=log,
                                creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        flow = GameFlow(proc.pid)
        host = flow.window(BOOT, "Easy Ride VR")

        def capture(label, layers):
            def ready():
                if proc.poll() is not None:
                    raise AssertionError(f"Game exited with {proc.returncode}")
                try:
                    state = json.loads((directory/"state.json").read_bytes())
                except (OSError, ValueError):
                    return False
                return (state["sessionState"] == "FOCUSED" and state["layeredFrames"] > 2
                        and state["layersLastFrame"] == len(layers))
            wait_for(label, ready)
            headset_capture(directory, label, layers)

        for label, shown in (("initially-hidden", False), ("shown", True), ("hidden-again", False), ("restored", True)):
            if label != "initially-hidden":
                flow.key(host, ord("H"))
            capture(label, ["projection", "quad"] if shown else ["projection"])
        with Image.open(directory/"capture"/"initially-hidden_left.png") as left, Image.open(
                directory/"capture"/"initially-hidden_right.png") as right:
            assert ImageChops.difference(left.convert("RGB"), right.convert("RGB")).getbbox(), "identical stereo views"
            for eye in (left, right):
                # The open sky above the grid is flat Classic blue or the enhanced cloud material.
                sky = ImageStat.Stat(eye.convert("RGB").crop((80, 35, 260, 92)))
                assert (max(sky.stddev) > 3) == (args.style != "classic"), (args.style, sky.stddev)
        for side in ("left", "right"):
            counts = []
            for label in ("initially-hidden", "shown", "hidden-again", "restored"):
                with Image.open(directory/"capture"/f"{label}_{side}.png") as image:
                    # The two large bezel panels use this cartridge palette entry.
                    counts.append(sum(n for n, rgb in image.convert("RGB").getcolors(image.width*image.height)
                                      if rgb == (189, 189, 173)))
            assert min(counts[1::2]) > max(counts[::2])+300, (side, counts)
        # Turn on the stationary grid. Both projection views must respond beneath the HUD.
        cmd = directory / "command.tmp"
        cmd.write_text("head rot 40 0 0\n", encoding="ascii")
        cmd.replace(directory / "command.txt")
        def turned():
            try:
                return abs(json.loads((directory/"state.json").read_bytes())["head"]["ypr"][0] - 40) < .1
            except (OSError, ValueError):
                return False
        wait_for("tracked head turn", turned)
        capture("head-turn", ["projection", "quad"])
        for side in ("left", "right"):
            with Image.open(directory/"capture"/f"restored_{side}.png") as straight, Image.open(
                    directory/"capture"/f"head-turn_{side}.png") as turned_eye:
                change = ImageChops.difference(straight.convert("RGB"), turned_eye.convert("RGB")).crop((80, 90, 400, 240))
                assert max(ImageStat.Stat(change).mean) > 8, f"{args.style}: {side} view ignored head turn"
        flow.key(host, ESCAPE)
        assert flow.window(BOOT, "Quick Race") == host
        capture("picker", ["quad"])
        # Sit down after navigating the menu. F2 immediately after launch should
        # produce the same horizon as automatic seating in both eyes.
        cmd.write_text("head pose 0 1.12 0 0 0 0\n", encoding="ascii")
        cmd.replace(directory / "command.txt")
        def seated():
            try:
                return abs(json.loads((directory/"state.json").read_bytes())["head"]["pos"][1] - 1.12) < .01
            except (OSError, ValueError):
                return False
        wait_for("seated in picker", seated)
        flow.key(host, START)
        assert flow.window(BOOT, "Easy Ride VR") == host
        flow.key(host, ord("P"))  # Hold car, suspension, and opponents still for the A/B capture.
        capture("grid", ["projection", "quad"])
        flow.key(host, 0x71)  # F2
        capture("manual-seat", ["projection", "quad"])
        for side in ("left", "right"):
            with Image.open(directory/"capture"/f"grid_{side}.png") as auto, Image.open(
                    directory/"capture"/f"manual-seat_{side}.png") as manual:
                horizon = ImageChops.difference(auto.convert("RGB"), manual.convert("RGB")).crop((40, 65, 440, 170))
                assert max(ImageStat.Stat(horizon).mean) < 2, f"{args.style}: {side} automatic seat differs from F2"
        flow.key(host, ESCAPE)
        assert flow.window(BOOT, "Quick Race") == host
        capture("back", ["quad"])
        if args.all_courses:
            for index, course in enumerate(("Aqua Tunnel", "Sunset Valley", "Night Owl", "King's Forest",
                    "Sea Breeze", "White Land", "Night Cruise", "Lake Side", "Big Ravine", "Sky Ramp",
                    "Harbor City", "Easy Ride"), start=1):
                flow.key(host, 0x27)  # Right on the actual headset board, including wraparound.
                wait_for(course + " selection", lambda: course + "  /  " in window_text(host))
                flow.key(host, START)
                try:
                    assert flow.window(BOOT, course + " VR") == host
                except AssertionError as error:
                    raise AssertionError(f"{course}: {window_text(host)}, process {proc.poll()}") from error
                capture(f"course-{index}", ["projection", "quad"])
                flow.key(host, ESCAPE)
                assert flow.window(BOOT, "Quick Race") == host
                capture(f"picker-{index}", ["quad"])
        user.PostMessageW(host, 0x10, 0, 0)
        assert proc.wait(12) == 0
    finally:
        if proc.poll() is None:
            proc.terminate()
            proc.wait(10)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("preview", "world", "xrsim", "private-desktop"):
        parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--style", choices=("classic", "n64", "remastered"), default="classic")
    parser.add_argument("--all-courses", action="store_true", help="Enter every Speed Trax grid from the headset picker")
    parser.add_argument("--rom", type=Path, default=os.environ.get("SRF_ROM"))
    parser.add_argument("--output", type=Path, default=Path(tempfile.gettempdir()))
    args = parser.parse_args()
    if args.rom is None:
        sys.exit(77)
    run_on_private_desktop(args.private_desktop)
    args.output.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix=f"xrsim-hud-{args.style}-", dir=args.output.resolve()))
    print(directory, flush=True)
    run(args, directory)
