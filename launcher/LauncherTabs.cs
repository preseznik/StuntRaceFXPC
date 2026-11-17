namespace StuntRaceFX.Launcher;

// Keep native tab selection, keyboard navigation and accessibility. Paint only
// the surrounding chrome so the Windows dark theme has no white tab-page frame.
internal sealed class LauncherTabs : TabControl
{
    public LauncherTabs()
    {
        SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true);
        DrawMode = TabDrawMode.OwnerDrawFixed;
    }
    internal static Color Surface => SystemInformation.HighContrast ? SystemColors.Control : Application.IsDarkModeEnabled ? Color.FromArgb(32, 32, 32) : Color.FromArgb(248, 249, 251);
    internal static Color Ink => SystemInformation.HighContrast ? SystemColors.ControlText : Application.IsDarkModeEnabled ? Color.WhiteSmoke : Color.FromArgb(30, 38, 48);
    internal static Color Accent => SystemInformation.HighContrast ? SystemColors.Highlight : Application.IsDarkModeEnabled ? Color.FromArgb(130, 185, 255) : Color.FromArgb(0, 91, 181);
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Surface);
        for (var index = 0; index < TabCount; ++index)
        {
            var bounds = GetTabRect(index);
            TextRenderer.DrawText(e.Graphics, TabPages[index].Text, Font, bounds, index == SelectedIndex ? Accent : Ink,
                TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            if (index != SelectedIndex) continue;
            using var pen = new Pen(Accent, Math.Max(2, DeviceDpi / 48));
            e.Graphics.DrawLine(pen, bounds.Left + 8, bounds.Bottom - 2, bounds.Right - 8, bounds.Bottom - 2);
            if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(e.Graphics, bounds, Ink, Surface);
        }
    }
}
