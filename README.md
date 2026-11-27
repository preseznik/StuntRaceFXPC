# Stunt Race FX PC

An unofficial native Windows port of the SNES racer, with desktop and OpenXR VR play, Quick Race, LAN multiplayer, and three graphics styles.

## Play

You need Windows x64 and your own **Stunt Race FX (USA) (Rev 1)** ROM. The launcher builds the game locally from your ROM. Keep it available when you play.

1. Extract the portable launcher and open `StuntRaceFX.Launcher.exe`.
2. Open **Settings → Game files**, select your ROM and a writable build folder, then click **Build game**.
3. Open **Play**, choose **Full game** or **Quick Race**, and start. Choose **VR** to use your active OpenXR runtime.

Under **Settings → Graphics → VR**, **OpenXR runtime** defaults to Automatic. Choose an installed runtime such as SteamVR or Virtual Desktop (VDXR), or use **Choose runtime file…** for another runtime’s JSON manifest. The override applies only to this game. After a VR launch, this panel shows the runtime and headset reported by the game.

Use **WASD** or the arrow keys to drive, **Space** to boost, and **P** to pause. Adjust gamepad bindings in **Settings → Controls**.

The portable launcher checks for stable GitHub releases at startup. Use **Settings → Game files → Check for updates** to check manually. Accept the download to install a verified copy under `.launcher-updates`. Your usual shortcut opens the new version. Choose **Build game** to apply native game changes to your local build.

## Build from source

From a source checkout on Windows x64 with PowerShell 7 and the .NET 10 SDK:

```powershell
./tools/Bootstrap-Toolchain.ps1
./tools/Build-Launcher.ps1 -BundleToolchain
```

Open the launcher in `artifacts/launcher` and follow the steps above. To rebuild the package, pass a fresh path with `-OutputDirectory`.

Stunt Race FX and its original game content belong to their respective rights holders. See [third-party notices](THIRD_PARTY_NOTICES.md).
