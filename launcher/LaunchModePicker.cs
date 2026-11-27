using System.ComponentModel;

namespace StuntRaceFX.Launcher;

internal sealed class LaunchModePicker : TableLayoutPanel
{
    private readonly RadioButton full = Choice("Full game"), quick = Choice("Quick Race");
    private LauncherDesign design = LauncherDesign.For(LauncherLook.RaceDesk);
    internal event EventHandler? ModeChanged;

    internal LaunchModePicker()
    {
        Name = "LaunchMode"; AccessibleName = "Launch mode"; AccessibleRole = AccessibleRole.Grouping;
        Dock = DockStyle.Top; AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Margin = Padding.Empty; ColumnCount = 2; RowCount = 1;
        ColumnStyles.Add(new(SizeType.Percent,50)); ColumnStyles.Add(new(SizeType.Percent,50));
        Controls.AddRange([full,quick]); full.Checked = true;
        quick.CheckedChanged += (_,_) => { SetDesign(design); ModeChanged?.Invoke(this,EventArgs.Empty); };
        full.CheckedChanged += (_,_) => SetDesign(design);
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    internal string SelectedMode
    {
        get => quick.Checked ? quick.Text : full.Text;
        set { if (value == quick.Text) quick.Checked = true; else full.Checked = true; }
    }

    internal void SetDesign(LauncherDesign value)
    {
        design = value;
        foreach (var item in new[] { full,quick })
        {
            item.BackColor = design.Field;
            item.FlatAppearance.CheckedBackColor = design.Accent;
            item.FlatAppearance.BorderColor = item.Checked ? design.Accent : design.Rule;
            item.ForeColor = item.Checked ? design.OnAccent : design.Text;
        }
    }

    private static RadioButton Choice(string name) => new() { Text = name, Appearance = Appearance.Button,
        FlatStyle = FlatStyle.Flat, AutoSize = true, Dock = DockStyle.Fill, Margin = Padding.Empty,
        Padding = new Padding(4,5,4,5), TextAlign = ContentAlignment.MiddleCenter, UseVisualStyleBackColor = false };
}

