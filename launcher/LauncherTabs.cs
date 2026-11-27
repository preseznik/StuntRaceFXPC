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
    internal LauncherDesign Design = LauncherDesign.For(LauncherLook.RaceDesk);
    protected override void OnPaint(PaintEventArgs e)
    {
        e.Graphics.Clear(Design.Canvas);
        bool main = Name == "LauncherTabs";
        bool rail = Alignment == TabAlignment.Left;
        if (rail && TabCount > 0) {
            using var fill = new SolidBrush(Design.Card);
            e.Graphics.FillRectangle(fill, 0, 0, GetTabRect(0).Right, Height);
        }
        using var glyph = new Font("Segoe MDL2 Assets", Font.Size);
        string[] icons = ["\uE768","\uE716","\uE713"];
        for (var index = 0; index < TabCount; ++index)
        {
            var bounds = GetTabRect(index);
            bool highlighted = main && index == SelectedIndex;
            if (highlighted)
            {
                using var fill = new SolidBrush(Design.Accent);
                e.Graphics.FillRectangle(fill, Rectangle.Inflate(bounds,-3,-3));
            }
            var ink = highlighted ? Design.OnAccent : index == SelectedIndex ? Design.Accent : Design.Text;
            var labelBounds = bounds;
            var alignment = TextFormatFlags.HorizontalCenter;
            if (main && index < icons.Length)
            {
                int gap = LogicalToDeviceUnits(8);
                int iconWidth = TextRenderer.MeasureText(e.Graphics,icons[index],glyph).Width;
                int textWidth = TextRenderer.MeasureText(e.Graphics,TabPages[index].Text,Font).Width;
                int left = rail ? gap : Math.Max(gap,(bounds.Width-iconWidth-gap-textWidth)/2);
                var iconBounds = new Rectangle(bounds.X+left,bounds.Y,iconWidth,bounds.Height);
                TextRenderer.DrawText(e.Graphics,icons[index],glyph,iconBounds,ink,TextFormatFlags.VerticalCenter | TextFormatFlags.HorizontalCenter);
                labelBounds.X = iconBounds.Right+gap/2;
                labelBounds.Width = Math.Max(1,bounds.Right-labelBounds.X);
                alignment = TextFormatFlags.Left;
            }
            TextRenderer.DrawText(e.Graphics, TabPages[index].Text, Font, labelBounds, ink,
                alignment | TextFormatFlags.VerticalCenter | TextFormatFlags.SingleLine);
            if (index != SelectedIndex) continue;
            using var pen = new Pen(Design.Accent, Math.Max(2, DeviceDpi / 48));
            if (!main) e.Graphics.DrawLine(pen, bounds.Left + 8, bounds.Bottom - 2, bounds.Right - 8, bounds.Bottom - 2);
            if (Focused && ShowFocusCues) ControlPaint.DrawFocusRectangle(e.Graphics, Rectangle.Inflate(bounds, -3, -3), ink, highlighted ? Design.Accent : Design.Canvas);
        }
    }
}
