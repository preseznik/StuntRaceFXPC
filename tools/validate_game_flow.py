"""Exercise the stock game through its Win32 windows. Inputs target only the child PID."""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time


user = c.WinDLL("user32", use_last_error=True)
kernel = c.WinDLL("kernel32", use_last_error=True)
callback = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
user.EnumWindows.argtypes = [callback, w.LPARAM]
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

BOOT = "StuntRaceFXBoot"
CLASS_BOARD = "StuntRaceFXClassBoard"
RACE = "StuntRaceFXMeshInspector"
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


class GameFlow:
    def __init__(self, pid):
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
        if user.GetForegroundWindow() == hwnd:
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
        window = self.window(RACE)
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

    def championship(self, idle_demo=False):
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
        self.confirm_to(play, RACE, None)
        race = self.race("Easy Ride", championship=True)
        self.key(race, ESCAPE)
        returned = self.window(BOOT, "Game Selection")
        self.record("Returned from championship", returned)
        self.key(returned, ESCAPE)

    def quick_race(self, course):
        race = self.race(course, championship=False)
        status = user.GetDlgItem(race, 0)
        def visible():
            return bool(user.GetWindowLongW(status, -16) & 0x10000000)
        initial = visible()
        self.key(race, ord("H"))
        assert visible() != initial
        self.key(race, ord("H"))
        assert visible() == initial
        self.key(race, ord("P"))
        self.hud(race, "paused race", lambda s: "PAUSED" in s)
        self.key(race, ord("R"))
        grid = self.hud(race, "unpaused restart on grid",
                        lambda s: "START LIGHTS" in s and "PAUSED" not in s and "Time 0:00.00" in s)
        self.record("Status toggle, pause and restart", race, grid)
        self.key(race, ESCAPE)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--idle-demo", action="store_true")
    for name in ("exe", "generated", "rom", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    exe, rom = args.exe.resolve(strict=True), args.rom.resolve(strict=True)
    world = (args.generated / "harbor-city" / "world.srfworld").resolve(strict=True)
    result = {"executable": str(exe), "sha256": hashlib.sha256(exe.read_bytes()).hexdigest()}
    with tempfile.TemporaryDirectory(dir=args.output.parent) as temporary:
        for mode in ("game", "race"):
            cmd = [str(exe), f"--{mode}", "--world", str(world), "--course", "harbor-city",
                   "--car", "coupe", "--rom", str(rom), "--width", "960", "--height", "540"]
            if mode == "game":
                cmd.extend(["--progress-file", str(Path(temporary).resolve() / "speed-trax.dat")])
            proc = subprocess.Popen(cmd, creationflags=subprocess.CREATE_NO_WINDOW,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            flow = GameFlow(proc.pid)
            try:
                if mode == "game":
                    flow.championship(args.idle_demo)
                else:
                    flow.quick_race("Harbor City")
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
    print(f"Passed championship and Quick Race flow. {args.output}")


if __name__ == "__main__":
    main()

