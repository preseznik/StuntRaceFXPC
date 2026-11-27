using System.Drawing.Drawing2D;

namespace StuntRaceFX.Launcher;

internal enum LauncherLook { RaceDesk, CourseAtlas, StartingGrid }
internal enum SurfaceRole { Card, Caption, Primary, Section }

// A look belongs to a window. The launch profile and LAN session never depend on it.
internal sealed record LauncherDesign(LauncherLook Look, Color Canvas, Color Card, Color Field, Color Text,
    Color Muted, Color Accent, Color OnAccent, Color Rule)
{
    internal static readonly string[] Names = ["Race Desk", "Course Atlas", "Starting Grid"];
    private static Color R(int value) => Color.FromArgb(unchecked((int)0xff000000) | value);
    internal static LauncherDesign For(LauncherLook look)
    {
        if (SystemInformation.HighContrast)
            return new(look, SystemColors.Control, SystemColors.Control, SystemColors.Window, SystemColors.WindowText,
                SystemColors.ControlText, SystemColors.Highlight, SystemColors.HighlightText, SystemColors.ControlDark);
        var colors = look switch {
            LauncherLook.CourseAtlas => new[] { 0x151d24, 0x222e37, 0x303f49, 0xf2f2e9, 0xb4c0c2, 0xf8c055, 0x202229, 0x455360 },
            LauncherLook.StartingGrid => [0x171719, 0x272627, 0x393537, 0xfff4dd, 0xc7bcb1, 0xff8548, 0x251c17, 0x514745],
            _ when Application.IsDarkModeEnabled => [0x151d2a, 0x222e40, 0x2d3b50, 0xf4f7ff, 0xbac7da, 0x91b9ff, 0x102446, 0x465872],
            _ => [0xe9edf2, 0xffffff, 0xf3f6fa, 0x192c44, 0x526279, 0x1859ae, 0xffffff, 0xb9c6d5]
        };
        return new(look,R(colors[0]),R(colors[1]),R(colors[2]),R(colors[3]),R(colors[4]),R(colors[5]),R(colors[6]),R(colors[7]));
    }

    internal void Apply(Control control, Color? background = null)
    {
        var role = control.Tag as SurfaceRole?;
        var surface = role == SurfaceRole.Card ? Card : background ?? Canvas;
        control.BackColor = surface;
        control.ForeColor = role == SurfaceRole.Caption ? Muted : Text;
        switch (control)
        {
            case LauncherTabs tabs: tabs.Design = this; break;
            case RaceBanner banner: banner.Design = this; break;
            case ComboBox box: box.FlatStyle = FlatStyle.Flat; box.BackColor = Field; break;
            case TextBoxBase input: input.BorderStyle = BorderStyle.FixedSingle; input.BackColor = Field; break;
            case NumericUpDown number: number.BackColor = Field; break;
            case ListBox list: list.BackColor = Field; list.BorderStyle = BorderStyle.None; break;
            case CourseTiles tiles: tiles.Design = this; tiles.BackColor = Canvas; tiles.BorderStyle = BorderStyle.None; break;
            case ListView list: list.BackColor = Field; list.BorderStyle = BorderStyle.None; break;
            case Button button:
                button.Paint -= PaintDisabledButton;
                button.Paint += PaintDisabledButton;
                bool primary = role == SurfaceRole.Primary;
                button.FlatStyle = FlatStyle.Flat;
                button.FlatAppearance.BorderColor = primary ? Accent : Rule;
                button.FlatAppearance.BorderSize = primary ? 0 : 1;
                button.UseVisualStyleBackColor = false;
                button.BackColor = primary ? Accent : Field;
                button.ForeColor = primary ? OnAccent : Text;
                int v = Look == LauncherLook.StartingGrid ? 7 : 4;
                button.Padding = new Padding(button.LogicalToDeviceUnits(primary ? 18 : 11),button.LogicalToDeviceUnits(v),button.LogicalToDeviceUnits(primary ? 18 : 11),button.LogicalToDeviceUnits(v));
                if (role == SurfaceRole.Section) button.BackColor = Card;
                break;
        }
        foreach (Control child in control.Controls) Apply(child, surface);
        if (control is LaunchModePicker modes) modes.SetDesign(this);
        control.Invalidate();
    }

    // Native flat buttons retain their accent fill when disabled on some Windows themes.
    private static void PaintDisabledButton(object? sender, PaintEventArgs e)
    {
        if (sender is not Button { Enabled:false, Parent: { } parent } button) return;
        var bounds = button.ClientRectangle;
        if (bounds.Width < 2 || bounds.Height < 2) return;
        var background = parent.BackColor; var foreground = parent.ForeColor;
        e.Graphics.Clear(background);
        using var border = new Pen(SystemInformation.HighContrast ? SystemColors.GrayText : Color.FromArgb(70,foreground));
        e.Graphics.DrawRectangle(border,0,0,bounds.Width-1,bounds.Height-1);
        var ink = SystemInformation.HighContrast ? SystemColors.GrayText :
            Color.FromArgb((foreground.R*2+background.R)/3,(foreground.G*2+background.G)/3,(foreground.B*2+background.B)/3);
        TextRenderer.DrawText(e.Graphics,button.Text,button.Font,bounds,ink,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
    }

    internal static TableLayoutPanel Column()
    {
        var box = new TableLayoutPanel { ColumnCount = 1, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink,
            Dock = DockStyle.Top, Margin = Padding.Empty };
        box.ColumnStyles.Add(new(SizeType.Percent, 100));
        return box;
    }
    private static readonly Font CardTitleFont = new("Segoe UI", 12, FontStyle.Bold);
    internal static TableLayoutPanel CardBox(string title)
    {
        var card = Column(); card.Tag = SurfaceRole.Card;
        card.Padding = new Padding(16, 12, 16, 14); card.Margin = new Padding(0, 0, 0, 12);
        card.Controls.Add(new Label { Text = title, AutoSize = true, Dock = DockStyle.Top,
            Font = CardTitleFont, Margin = new Padding(0, 0, 0, 10) });
        return card;
    }
    internal static TableLayoutPanel FieldBox(string title, Control input)
    {
        var field = Column(); field.Margin = new Padding(0, 0, 8, 4);
        field.Controls.Add(new Label { Text = title, AutoSize = true, Dock = DockStyle.Top, Margin = new Padding(0, 0, 0, 3), Tag = SurfaceRole.Caption });
        input.Dock = DockStyle.Top; input.Margin = Padding.Empty;
        field.Controls.Add(input); return field;
    }
    internal static TableLayoutPanel Columns(params int[] weights)
    {
        var row = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, ColumnCount = weights.Length, Margin = Padding.Empty };
        foreach (var weight in weights) row.ColumnStyles.Add(new(SizeType.Percent, weight));
        return row;
    }
}

// Render the player's installed course capture into the header with enough contrast for the title.
internal sealed class RaceBanner : Control
{
    internal LauncherDesign Design = LauncherDesign.For(LauncherLook.RaceDesk);
    internal Image? CourseImage;
    internal string Subtitle = "";
    internal bool Hero;
    internal string Mode = "";
    internal RaceBanner()
    {
        DoubleBuffered = true; ResizeRedraw = true; TabStop = false;
        AccessibleRole = AccessibleRole.Graphic;
        Dock = DockStyle.Fill; Margin = Padding.Empty;
    }
    protected override void OnPaint(PaintEventArgs e)
    {
        if (Width < 2 || Height < 2) return;
        var g = e.Graphics;
        bool contrast = SystemInformation.HighContrast;
        var baseColor = contrast ? SystemColors.Window : Color.FromArgb(19, 32, 43);
        g.Clear(baseColor);
        if (!contrast)
        {
            if (CourseImage != null)
            {
                float ratio = Math.Max((float)Width / CourseImage.Width, (float)Height / CourseImage.Height);
                int w = (int)Math.Ceiling(ratio*CourseImage.Width), h = (int)Math.Ceiling(ratio*CourseImage.Height);
                g.DrawImage(CourseImage, (Width-w)/2, (Height-h)/2, w, h);
                using var shade = new LinearGradientBrush(ClientRectangle, Color.FromArgb(Hero ? 10 : 240,baseColor),
                    Color.FromArgb(Hero ? 232 : 35,baseColor), Hero ? 90f : 0f);
                g.FillRectangle(shade, ClientRectangle);
            }
            else
            {
                using var stripe = new SolidBrush(Color.FromArgb(48,Design.Accent));
                for (int notch=Width/2; notch<Width+Height; notch+=38)
                    g.FillPolygon(stripe,new Point[] {new Point(notch,0),new Point(notch+15,0),new Point(notch+15-Height,Height),new Point(notch-Height,Height)});
            }
        }
        bool grid = Hero && Design.Look == LauncherLook.StartingGrid && Width > LogicalToDeviceUnits(420);
        float unit = DeviceDpi / 96f;
        int inset = (int)(16 * unit);
        float x = inset, width = Width - 2 * inset;
        if (grid)
        {
            float start = Width * 0.61f;
            using var panel = new SolidBrush(contrast ? SystemColors.Window : Color.FromArgb(243,29,26,26));
            g.FillPolygon(panel, new PointF[] {new(start+Height*.13f,0),new(Width,0),new(Width,Height),new(start,Height)});
            x = start + Height*.07f + inset; width = Width-x-inset;
        }
        if (width <= 0) return;
        var ink = contrast ? SystemColors.WindowText : Color.White;
        using var brush = new SolidBrush(ink);
        using var caption = new Font("Segoe UI", 13*unit, GraphicsUnit.Pixel);
        float size = (Hero ? 36 : 23)*unit;
        string face = grid ? "Bahnschrift" : "Segoe UI";
        // Keep a two-word course name readable even on the narrow 150% layout.
        using (var initial = new Font(face,size,FontStyle.Bold,GraphicsUnit.Pixel))
            size *= Math.Clamp(width / Math.Max(1,g.MeasureString(Text,initial).Width),.58f,1f);
        using var title = new Font(face,size, FontStyle.Bold,GraphicsUnit.Pixel);
        using var format = new StringFormat { Trimming = StringTrimming.EllipsisWord };
        float titleHeight = title.GetHeight(g)*1.17f;
        float subHeight = Math.Min(caption.GetHeight(g)*3, g.MeasureString(Subtitle,caption,(int)width,format).Height+2);
        float modeHeight = grid ? caption.GetHeight(g)+6*unit : 0;
        float total = titleHeight+subHeight+modeHeight;
        float y = grid || !Hero ? Math.Max(2,(Height-total)/2) : Math.Max(2,Height-total-inset);
        if (grid) {
            using var accentText = new SolidBrush(Design.Accent);
            g.DrawString(Mode,caption,accentText,x,y); y += modeHeight;
        }
        g.DrawString(Text,title,brush,new RectangleF(x,y,width,titleHeight),format);
        g.DrawString(Subtitle,caption,brush,new RectangleF(x,y+titleHeight,width,subHeight),format);
        using var accent = new SolidBrush(Design.Accent); g.FillRectangle(accent,0,Height-3,Width,3);
    }
}
