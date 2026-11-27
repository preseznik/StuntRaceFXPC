using System.Globalization;

namespace StuntRaceFX.Launcher;

internal sealed class ControllerDialog : Form
{
    private sealed record Choice(string Value, string Label);
    private readonly Dictionary<string, ComboBox> fields = [];
    private readonly NumericUpDown deadzone = new() { Minimum = 1, Maximum = 30000, Increment = 500, Width = 130 };
    private readonly NumericUpDown trigger = new() { Minimum = 1, Maximum = 30000, Increment = 500, Width = 130 };
    private readonly Label error = new() { AutoSize = true, ForeColor = Color.Firebrick, Dock = DockStyle.Fill };
    private readonly string path;

    internal ControllerDialog(string path)
    {
        this.path = path;
        Text = "Controller settings";
        ClientSize = new Size(680, 690);
        MinimumSize = new Size(620, 560);
        StartPosition = FormStartPosition.CenterParent;
        MaximizeBox = MinimizeBox = false;
        Font = new Font("Segoe UI", 10);
        AutoScaleMode = AutoScaleMode.Dpi;
        var root = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(20), ColumnCount = 1, RowCount = 5 };
        root.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        root.Controls.Add(new Label { AutoSize = true, MaximumSize = new Size(630, 0), Text =
            "Choose original digital steering or proportional stick steering. D-pad and keyboard keep their original button behavior. Triggers remain digital.\nSettings apply on the next game launch.", Margin = new Padding(0, 0, 0, 14) });
        var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        var grid = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 2 };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 165));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (var binding in ControllerProfile.Bindings)
        {
            var combo = new ComboBox { Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList,
                DisplayMember = nameof(Choice.Label), ValueMember = nameof(Choice.Value), AccessibleName = binding.Label };
            foreach (var value in binding.Axis ? ControllerProfile.Axes : ControllerProfile.Buttons)
                combo.Items.Add(new Choice(value, ControllerProfile.LabelFor(value)));
            fields.Add(binding.Key, combo);
            AddRow(binding.Label, combo);
        }
        var steeringMode = new ComboBox { Name = "steering_mode", AccessibleName = "Steering response",
            Dock = DockStyle.Fill, DropDownStyle = ComboBoxStyle.DropDownList,
            DisplayMember = nameof(Choice.Label), ValueMember = nameof(Choice.Value) };
        steeringMode.Items.AddRange([new Choice("digital", "Original digital"), new Choice("analog", "Proportional analog")]);
        fields.Add("steering_mode", steeringMode);
        AddRow("Steering response", steeringMode);
        deadzone.AccessibleName = "Stick deadzone";
        trigger.AccessibleName = "Trigger threshold";
        AddRow("Stick deadzone", deadzone);
        AddRow("Trigger threshold", trigger);
        scroll.Controls.Add(grid);
        root.Controls.Add(scroll);
        root.Controls.Add(new Label { Text = "Values: 1–30000. Settings apply on the next game launch.\nSave writes controller.ini in the selected build folder.", AutoSize = true, Margin = new Padding(0, 10, 0, 8) });
        root.Controls.Add(error);
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, FlowDirection = FlowDirection.RightToLeft, Margin = new Padding(0, 8, 0, 0) };
        var save = new Button { Text = "Save", Tag = SurfaceRole.Primary, AutoSize = true };
        var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
        var defaults = new Button { Text = "Restore defaults", AutoSize = true };
        buttons.Controls.AddRange([save, cancel, defaults]);
        root.Controls.Add(buttons);
        Controls.Add(root);
        AcceptButton = save;
        CancelButton = cancel;
        defaults.Click += (_, _) => Fill(new ControllerProfile());
        save.Click += (_, _) =>
        {
            try
            {
                var profile = new ControllerProfile();
                foreach (var (key, combo) in fields) profile.Set(key, ((Choice)combo.SelectedItem!).Value);
                profile.Set("deadzone", deadzone.Value.ToString(CultureInfo.InvariantCulture));
                profile.Set("trigger_threshold", trigger.Value.ToString(CultureInfo.InvariantCulture));
                profile.Save(this.path);
                DialogResult = DialogResult.OK;
                Close();
            }
            catch (Exception exception) when (exception is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException)
            { error.Text = exception.Message; }
        };
        try { Fill(ControllerProfile.Load(path)); }
        catch (Exception exception) when (exception is IOException or InvalidDataException or UnauthorizedAccessException or ArgumentException)
        {
            Fill(new ControllerProfile());
            error.Text = "Existing settings could not be loaded. Save replaces them with the values shown; Cancel keeps them.\n" + exception.Message;
        }

        void AddRow(string label, Control control)
        {
            var row = grid.RowCount++;
            grid.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            grid.Controls.Add(new Label { Text = label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(0, 7, 8, 7) }, 0, row);
            control.Margin = new Padding(0, 3, 0, 3);
            grid.Controls.Add(control, 1, row);
        }
    }

    private void Fill(ControllerProfile profile)
    {
        foreach (var (key, combo) in fields)
            combo.SelectedItem = combo.Items.Cast<Choice>().Single(choice => choice.Value == profile[key]);
        deadzone.Value = int.Parse(profile["deadzone"], CultureInfo.InvariantCulture);
        trigger.Value = int.Parse(profile["trigger_threshold"], CultureInfo.InvariantCulture);
    }


}
