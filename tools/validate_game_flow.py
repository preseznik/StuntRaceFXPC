"""Exercise the stock game through its Win32 windows. Inputs target only the child PID."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time


user = c.WinDLL("user32", use_last_error=True)
kernel = c.WinDLL("kernel32", use_last_error=True)
callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user.EnumWindows.argtypes = [callback, w.LPARAM]
user.EnumChildWindows.argtypes = [w.HWND, callback, w.LPARAM]
user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
user.GetWindowThreadProcessId.restype = w.DWORD
user.GetClassNameW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetWindowTextW.argtypes = [w.HWND, w.LPWSTR, c.c_int]
user.GetDlgItem.argtypes = [w.HWND, c.c_int]
user.GetDlgItem.restype = w.HWND
user.GetWindowLongW.argtypes = [w.HWND, c.c_int]
user.GetForegroundWindow.restype = w.HWND
user.SetForegroundWindow.argtypes = [w.HWND]
user.ShowWindow.argtypes = [w.HWND, c.c_int]
user.BringWindowToTop.argtypes = [w.HWND]
user.SetActiveWindow.argtypes = [w.HWND]
user.AttachThreadInput.argtypes = [w.DWORD, w.DWORD, w.BOOL]
user.PostMessageW.argtypes = [w.HWND, w.UINT, w.WPARAM, w.LPARAM]
user.IsWindowVisible.argtypes = [w.HWND]
user.IsWindow.argtypes = [w.HWND]

BOOT = "StuntRaceFXGame"
CLASS_BOARD = BOOT
RACE = BOOT
START = 0x0D
ESCAPE = 0x1B


def window_text(hwnd):
    buf = c.create_unicode_buffer(4096)
    user.GetWindowTextW(hwnd, buf, len(buf))
    return buf.value


def wait_for(description, probe, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = probe()
        if value:
            return value
        time.sleep(.03)
    raise AssertionError(f"Timed out waiting for {description}")


def private_desktop():
    user.GetThreadDesktop.argtypes = [w.DWORD]
    user.GetThreadDesktop.restype = w.HANDLE
    user.GetUserObjectInformationW.argtypes = [w.HANDLE, c.c_int, w.LPVOID, w.DWORD, c.POINTER(w.DWORD)]
    name = c.create_unicode_buffer(128)
    needed = w.DWORD()
    desktop = user.GetThreadDesktop(kernel.GetCurrentThreadId())
    return bool(user.GetUserObjectInformationW(desktop, 2, name, c.sizeof(name), c.byref(needed))
                and name.value.startswith("SRF-Navigation-"))


def run_on_private_desktop(helper):
    if not private_desktop():
        command = [helper.resolve(strict=True), sys.executable, *sys.argv]
        raise SystemExit(subprocess.run(command, stdout=sys.stdout, stderr=sys.stderr,
                                              creationflags=subprocess.CREATE_NO_WINDOW).returncode)


def dashboard_labels(image, low_raster=False, split=None):
    """Sample the cartridge panel's fixed label row, excluding changing digits."""
    from PIL import Image
    width, height = image.size
    if low_raster:
        w = min(width, height*4/3)
        h = w*3/4
        image = image.resize((320, 240), Image.Resampling.NEAREST,
                             box=((width-w)/2, (height-h)/2, (width+w)/2, (height+h)/2))
        width, height = image.size
    if split is not None:
        image = image.crop((0, split*(height//2), width, (split+1)*(height//2)))
        width, height = image.size
    scale = min(width/256, height/168)
    left = (width-256*scale)/2
    top = height-round(40*scale)
    return image.convert("RGB").resize((256, 12), Image.Resampling.NEAREST,
                                        box=(left, top, width-left, top+12*scale))


def pixel_difference(first, second):
    from PIL import ImageChops, ImageStat
    return sum(ImageStat.Stat(ImageChops.difference(first, second)).mean)/3


class GameFlow:
    def __init__(self, pid, directory=None):
        self.directory = directory
        self.isolated = private_desktop()
        self.pid = pid
        self.checks = []
        self.start = time.monotonic()

    def owned(self, hwnd):
        pid = w.DWORD()
        user.GetWindowThreadProcessId(hwnd, c.byref(pid))
        return self.pid == pid.value

    def find_window(self, klass, caption=None):
        found = []

        @callback
        def visit(hwnd, _):
            if self.owned(hwnd) and user.IsWindowVisible(hwnd):
                name = c.create_unicode_buffer(256)
                user.GetClassNameW(hwnd, name, len(name))
                if name.value == klass and (caption is None or window_text(hwnd).endswith(caption)):
                    found.append(hwnd)
            return True

        user.EnumWindows(visit, 0)
        return found[0] if found else None

    def window(self, klass, caption=None):
        return wait_for(f"{klass} {caption or ''} (PID {self.pid})", lambda: self.find_window(klass, caption))

    def confirm_to(self, source, klass, caption):
        # Title and board transitions have input gates. A slow first present
        # can leave the old screen gated after the nominal animation duration.
        source_caption = window_text(source)
        next_press = 0
        def check():
            nonlocal next_press
            target = self.find_window(klass, caption)
            if target:
                return target
            if (time.monotonic() >= next_press and self.owned(source)
                    and window_text(source) == source_caption):
                self.key(source, START)
                next_press = time.monotonic() + 2.75
        return wait_for(f"{source_caption} to {caption}", check, 25)

    def focus(self, hwnd):
        assert self.owned(hwnd)
        if self.isolated or user.GetForegroundWindow() == hwnd:
            return
        current = kernel.GetCurrentThreadId()
        foreground = user.GetForegroundWindow()
        other = user.GetWindowThreadProcessId(foreground, None) if foreground else 0
        target = user.GetWindowThreadProcessId(hwnd, None)
        attached = [thread for thread in {target, other} - {0, current}
                    if user.AttachThreadInput(current, thread, True)]
        try:
            user.ShowWindow(hwnd, 9)  # SW_RESTORE
            user.BringWindowToTop(hwnd)
            user.SetActiveWindow(hwnd)
            user.SetForegroundWindow(hwnd)
        finally:
            for thread in attached:
                user.AttachThreadInput(current, thread, False)
        wait_for(f"game window in foreground ({window_text(hwnd)}, foreground={user.GetForegroundWindow()}, "
                 f"target={hwnd}, threads={attached}, last_error={c.get_last_error()})",
                 lambda: user.GetForegroundWindow() == hwnd, 2)

    def key(self, hwnd, vk):
        self.focus(hwnd)
        assert user.PostMessageW(hwnd, 0x0100, vk, 1)
        time.sleep(.10)
        if self.owned(hwnd):
            assert user.PostMessageW(hwnd, 0x0101, vk, 0xC0000001)
        time.sleep(.07)

    def frame(self, hwnd):
        from PIL import Image, ImageGrab
        assert self.owned(hwnd)
        if self.isolated:
            path = self.directory / "navigation-frame.png"
            path.unlink(missing_ok=True)
            assert user.PostMessageW(hwnd, 0x8001, 0, 0)
            wait_for("navigation renderer readback (use srf_navigation_preview)", path.is_file, 5)
            with Image.open(path) as image:
                return image.convert("RGB")
        # ImageGrab returns the window (including chrome). Sample only its client area.
        frame = ImageGrab.grab(window=hwnd)
        client, window, origin = w.RECT(), w.RECT(), w.POINT()
        assert user.GetClientRect(hwnd, c.byref(client)) and user.GetWindowRect(hwnd, c.byref(window))
        assert user.ClientToScreen(hwnd, c.byref(origin))
        x, y = origin.x-window.left, origin.y-window.top
        return frame.crop((x,y,x+client.right,y+client.bottom))

    def record(self, step, hwnd, hud=None):
        item = {"step": step, "wall_seconds": round(time.monotonic() - self.start, 3),
                "window": window_text(hwnd)}
        if hud:
            item["hud"] = hud
        self.checks.append(item)

    def hud(self, hwnd, description, predicate):
        last = ""
        def probe():
            nonlocal last
            last = window_text(user.GetDlgItem(hwnd, 0))
            assert "Muted:" not in last, last
            return last if predicate(last) else None
        try:
            return wait_for(description, probe)
        except AssertionError as error:
            raise AssertionError(f"{error}; last HUD: {last}") from error

    def race(self, name, championship):
        window = wait_for(name + " race HUD", lambda: (h if (h := self.find_window(RACE)) and user.GetDlgItem(h, 0) else None))
        self.focus(window)
        action = "R retire" if championship else "R restart"
        grid = self.hud(window, f"{name} grid", lambda s: all(
            value in s for value in (name, "START LIGHTS", "Lap 1 / 3", action)))
        self.record("Starting grid", window, grid)
        def elapsed(hud):
            match = re.search(r"\bTime (\d+):(\d\d)\.(\d\d)", hud)
            return int(match[1])*6000 + int(match[2])*100 + int(match[3]) if match else -1
        go = self.hud(window, "green light and live race clock",
                      lambda s: "    GO" in s and elapsed(s) > 0)
        self.record("Green light", window, go)
        live = self.hud(window, "advancing unpaused race",
                        lambda s: "    GO" in s and "PAUSED" not in s and elapsed(s) >= elapsed(go) + 25)
        self.record("Driving", window, live)
        return window

    def idle_demo(self, title):
        def wait_caption(caption, timeout):
            return wait_for(caption, lambda: self.find_window(BOOT, caption), timeout)

        demo = wait_caption("Easy Ride Demo", 40)
        self.record("Easy Ride demo entered", demo)
        started = time.monotonic()
        for offset in (4, 10, 17, 24):
            time.sleep(max(0, started + offset - time.monotonic()))
            assert window_text(demo).endswith("Easy Ride Demo")
            self.record(f"Easy Ride camera at {offset}s", demo)
        returned = wait_caption("Stunt Race FX", 20)
        assert returned == title
        self.record("Demo returned to animated title", returned)
        time.sleep(7)
        self.record("Warm title prompt", returned)
        demo = wait_caption("Easy Ride Demo", 35)
        time.sleep(3)
        self.key(demo, ord("X"))  # SNES A is outside the demo interruption mask.
        time.sleep(2)
        assert window_text(demo).endswith("Easy Ride Demo")
        self.record("Ignored SNES A during demo", demo)
        self.key(demo, 0x25)  # D-pad Left through the same frontend input path.
        game = wait_caption("Game Selection", 5)
        self.record("Interrupted demo opened Game Selection", game)
        return game

    def start_novice(self, idle_demo=False):
        title = self.window(BOOT, "Stunt Race FX")
        self.focus(title)
        self.record("Nintendo splash and title window", title)
        time.sleep(9.1)  # PUSH START is on screen by VBlank 516.
        self.record("Title prompt", title)
        game = self.idle_demo(title) if idle_demo else self.confirm_to(title, BOOT, "Game Selection")
        self.record("Game Selection", game)
        time.sleep(.90)  # The entering board enables input on VBlank 41.
        car = self.confirm_to(game, BOOT, "Car Selection")
        self.record("Car Selection", car)
        time.sleep(1.05)  # Let the turntable slide into place.
        initials = self.confirm_to(car, BOOT, "Input Your Name")
        self.record("Initials editor", initials)
        self.key(initials, 0x26)  # Up changes a letter, Right moves its cursor.
        self.key(initials, 0x27)
        board = self.confirm_to(initials, CLASS_BOARD, "Speed Trax")
        self.focus(board)
        self.record("Speed Trax class selection", board)
        time.sleep(2.35)  # Class input opens on VBlank 119.
        play = self.confirm_to(board, CLASS_BOARD, "Play")
        self.record("Play / Cancel", play)
        self.key(play, START)
        return wait_for("Novice race HUD", lambda: (play if user.GetDlgItem(play, 0) else None))

    def championship(self, idle_demo=False):
        self.start_novice(idle_demo)
        race = self.race("Easy Ride", championship=True)
        self.key(race, ESCAPE)
        returned = self.window(BOOT, "Game Selection")
        self.record("Returned from championship", returned)
        self.key(returned, ESCAPE)

    def quick_race(self, course, reference):
        race = self.race(course, championship=False)
        for step, shown in (("visible", True), ("hidden", False), ("restored", True)):
            if step != "visible":
                self.key(race, ord("H"))
            def matches():
                diff = pixel_difference(reference, dashboard_labels(self.frame(race)))
                return (diff,) if (diff < 5 if shown else diff > 35) else None
            difference, = wait_for("rendered dashboard " + step, matches, 5)
            self.record(f"Dashboard {step} (label pixel difference {difference:.2f})", race)
        self.key(race, ord("P"))
        self.hud(race, "paused race", lambda s: "PAUSED" in s)
        self.key(race, ord("R"))
        grid = self.hud(race, "unpaused restart on grid",
                        lambda s: "START LIGHTS" in s and "PAUSED" not in s and "Time 0:00.00" in s)
        self.record("Status toggle, pause and restart", race, grid)
        self.key(race, ESCAPE)
        assert self.window(BOOT, "Quick Race") == race
        self.key(race, ESCAPE)
        assert self.window(BOOT, "Game Selection") == race
        self.key(race, ESCAPE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--private-desktop", type=Path, help="Run on a private desktop with the navigation test build")
    parser.add_argument("--quick-only", action="store_true")
    parser.add_argument("--idle-demo", action="store_true")
    for name in ("exe", "generated", "rom", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    if args.private_desktop:
        run_on_private_desktop(args.private_desktop)
    user.SetProcessDPIAware()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    exe, rom = args.exe.resolve(strict=True), args.rom.resolve(strict=True)
    world = (args.generated / "harbor-city" / "world.srfworld").resolve(strict=True)
    result = {"executable": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
    with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
        from PIL import Image
        directory = Path(temporary).resolve()
        reference = directory / "reference.png"
        subprocess.run([exe, "--race", "--world", world, "--course", "harbor-city", "--car", "coupe",
                        "--rom", rom, "--width", "960", "--height", "540", "--steps", "90",
                        "--screenshot", reference], check=True, creationflags=subprocess.CREATE_NO_WINDOW)
        with Image.open(reference) as image:
            labels = dashboard_labels(image)
        for mode in (("race",) if args.quick_only else ("game", "race")):
            cmd = [str(exe), f"--{mode}", "--world", str(world), "--course", "harbor-city",
                   "--car", "coupe", "--rom", str(rom), "--width", "960", "--height", "540"]
            if mode == "game":
                cmd.extend(["--progress-file", str(Path(temporary).resolve() / "speed-trax.dat")])
            proc = subprocess.Popen(cmd, cwd=directory, creationflags=subprocess.CREATE_NO_WINDOW,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            flow = GameFlow(proc.pid, directory)
            try:
                if mode == "game":
                    flow.championship(args.idle_demo)
                else:
                    flow.quick_race("Harbor City", labels)
                stdout, stderr = proc.communicate(timeout=10)
                assert proc.returncode == 0, (proc.returncode, stdout, stderr)
                result[mode] = {"checks": flow.checks, "exit_code": proc.returncode}
            except Exception:
                print(json.dumps(flow.checks, indent=2), flush=True)
                raise
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate(timeout=5)
    args.output.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(f"Game flow checks passed. {args.output}")


if __name__ == "__main__":
    main()

