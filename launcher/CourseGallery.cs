using System.Text.Json;

namespace StuntRaceFX.Launcher;

internal sealed class CourseGallery : Form
{
    internal GameCourse Selection { get; private set; }
    internal CourseGallery(IEnumerable<GameCourse> courses, GameCourse selected, string output, string car, RaceStyle style)
    {
        Selection = selected;
        Text = "Browse courses"; StartPosition = FormStartPosition.CenterParent;
        AutoScaleMode = AutoScaleMode.Dpi; ClientSize = new Size(760, 480); MinimumSize = new Size(480, 320);
        Padding = new Padding(16);
        var list = new CourseTiles { Name = "CourseGallery" };
        list.LoadCourses(courses, selected, output, car, style);
        var choose = new Button { Text = "Select course", Tag = SurfaceRole.Primary, AutoSize = true, DialogResult = DialogResult.OK };
        var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
        var buttons = new FlowLayoutPanel { AutoSize = true, Dock = DockStyle.Bottom, FlowDirection = FlowDirection.RightToLeft };
        buttons.Controls.AddRange([choose, cancel]); Controls.Add(list); Controls.Add(buttons);
        AcceptButton = choose; CancelButton = cancel;
        list.SelectedIndexChanged += (_, _) => {
            choose.Enabled = list.SelectedItems.Count == 1;
            if (choose.Enabled) Selection = (GameCourse)list.SelectedItems[0].Tag!;
        };
        list.ItemActivate += (_, _) => { if (choose.Enabled) DialogResult = DialogResult.OK; };
        Shown += (_, _) => { list.Focus(); list.FocusedItem?.EnsureVisible(); };
    }


}

internal sealed class CourseTiles : ListView
{
    private readonly ImageList thumbnails = new() { ColorDepth = ColorDepth.Depth32Bit, ImageSize = new Size(176, 104) };
    internal LauncherDesign Design = LauncherDesign.For(LauncherLook.RaceDesk);
    internal CourseTiles()
    {
        Dock = DockStyle.Fill; AccessibleName = "Courses"; View = View.LargeIcon;
        LargeImageList = thumbnails; MultiSelect = false; HideSelection = false; UseCompatibleStateImageBehavior = false;
        OwnerDraw = true; DoubleBuffered = true;
        DrawItem += (_, e) =>
        {
            var item = e.Item!;
            var b = e.Bounds;
            bool selected = item.Selected;
            var background = selected ? Design.Accent : Design.Card;
            using var fill = new SolidBrush(background);
            e.Graphics.FillRectangle(fill,b);
            var picture = new Rectangle(b.X+3,b.Y+3,b.Width-6,Math.Min(b.Height-Font.Height-8,thumbnails.ImageSize.Height));
            if (picture.Height > 0 && thumbnails.Images.ContainsKey(item.ImageKey))
                thumbnails.Draw(e.Graphics,picture.X,picture.Y,picture.Width,picture.Height,thumbnails.Images.IndexOfKey(item.ImageKey));
            else if (picture.Height > 0)
                TextRenderer.DrawText(e.Graphics,"No capture",Font,picture,selected ? Design.OnAccent : Design.Muted,
                    TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            var label = new Rectangle(b.Left+3,picture.Bottom,b.Width-6,b.Bottom-picture.Bottom);
            TextRenderer.DrawText(e.Graphics,item.Text,Font,label,selected ? Design.OnAccent : Design.Text,
                TextFormatFlags.VerticalCenter | TextFormatFlags.HorizontalCenter | TextFormatFlags.SingleLine | TextFormatFlags.EndEllipsis);
            if (Focused && item.Focused) ControlPaint.DrawFocusRectangle(e.Graphics,Rectangle.Inflate(b,-2,-2));
        };
    }
    internal void LoadCourses(IEnumerable<GameCourse> courses, GameCourse selected, string output, string car, RaceStyle style)
    {
        BeginUpdate();
        try
        {
            Items.Clear(); thumbnails.Images.Clear();
            BuildRecord? record = null;
            try { if (!string.IsNullOrWhiteSpace(output)) record = BuildPipeline.LastSuccess(output); }
            catch (Exception error) when (error is IOException or InvalidDataException or JsonException or ArgumentException or UnauthorizedAccessException) { }
            foreach (var course in courses)
            {
                var item = new ListViewItem(course.Name) { Tag = course };
                if (record != null)
                {
                    var stage = Path.Combine(output, "builds", record.Directory);
                    var file = GameContent.CaptureName(course.Id, car, style, record.ContentVersion);
                    try
                    {
                        var path = Path.Combine(stage, file);
                        if (File.Exists(path))
                        {
                            using var source = Image.FromFile(path);
                            using var thumb = new Bitmap(thumbnails.ImageSize.Width, thumbnails.ImageSize.Height);
                            using (var canvas = Graphics.FromImage(thumb))
                            {
                                canvas.Clear(LauncherTabs.Surface);
                                var scale = Math.Min((float)thumb.Width / source.Width, (float)thumb.Height / source.Height);
                                int w = (int)(source.Width * scale), h = (int)(source.Height * scale);
                                canvas.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
                                canvas.DrawImage(source, (thumb.Width - w) / 2, (thumb.Height - h) / 2, w, h);
                            }
                            thumbnails.Images.Add(course.Id, thumb);
                            // Materialize the native image list before disposing the source bitmap.
                            _ = thumbnails.Handle;
                            item.ImageKey = course.Id;
                        }
                    }
                    catch (Exception error) when (error is IOException or UnauthorizedAccessException or ArgumentException or OutOfMemoryException) { }
                }
                Items.Add(item);
                if (course == selected) { item.Selected = true; item.Focused = true; }
            }
        }
        finally { EndUpdate(); }
    }
    protected override void Dispose(bool disposing)
    {
        if (disposing) thumbnails.Dispose();
        base.Dispose(disposing);
    }
}
