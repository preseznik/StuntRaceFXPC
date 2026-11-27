using System.Runtime.InteropServices;

namespace StuntRaceFX.Launcher;

internal sealed class LauncherGamepad
{
    private uint slot;
    private bool hasPad;

    [StructLayout(LayoutKind.Sequential)]
    private struct State
    {
        public uint Packet;
        public ushort Buttons;
        public byte LeftTrigger, RightTrigger;
        public short LeftX, LeftY, RightX, RightY;
    }

    [DllImport("xinput1_4.dll")]
    private static extern uint XInputGetState(uint index, out State state);

    internal bool TryRead(out ushort buttons)
    {
        buttons = 0;
        if (!hasPad)
        {
            for (slot = 0; slot < 4; ++slot)
                if (XInputGetState(slot, out _) == 0) break;
            hasPad = slot < 4;
        }
        if (!hasPad) return false;
        if (XInputGetState(slot, out var state) != 0) { hasPad = false; return false; }
        buttons = state.Buttons;
        return true;
    }
}
