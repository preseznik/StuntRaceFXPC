namespace StuntRaceFX.Launcher;

// The launcher reads the controller only while Starting Grid or one of its dialogs owns focus.
internal sealed class GridNavigation : IDisposable
{
    private readonly MainForm owner;
    private readonly System.Windows.Forms.Timer timer = new() { Interval = 60 };
    private Form? previousWindow;
    private ushort previous;
    private readonly LauncherGamepad pad = new();

    internal GridNavigation(MainForm owner)
    {
        this.owner = owner;
        timer.Tick += (_,_) => Poll();
    }

    internal void Enable(bool enabled)
    {
        previousWindow = null;
        timer.Enabled = enabled;
    }

    private void Poll()
    {
        var window = Form.ActiveForm;
        if (window == null || !window.Enabled || (window != owner && window.Owner != owner))
        { previousWindow = null; return; }
        if (!pad.TryRead(out var buttons)) { timer.Interval = 1000; previousWindow = null; return; }
        timer.Interval = 60;
        ushort pressed = (ushort)(buttons & ~previous);
        previous = buttons;
        if (previousWindow == window && pressed != 0) Dispatch(window,pressed);
        previousWindow = window;
    }

    internal void Dispatch(Form window, ushort buttons)
    {
        if (window == owner && owner.HandleLobbyMenu(buttons)) return;
        var target = window.ActiveControl;
        while (target is ContainerControl container && container.ActiveControl != null) target = container.ActiveControl;
        var combo = target as ComboBox;
        if ((buttons & 0x2000) != 0) // B
        {
            if (combo is { DroppedDown:true }) combo.DroppedDown = false;
            else if (window != owner) window.DialogResult = DialogResult.Cancel;
            else owner.BackFromGrid();
            return;
        }
        if ((buttons & 0x300) != 0 && window == owner) { owner.StepDestination((buttons & 0x100) != 0 ? -1 : 1); return; }
        if ((buttons & 0x10) != 0 && window.AcceptButton != null) { window.AcceptButton.PerformClick(); return; }
        if ((buttons & 0x1010) != 0) // A, or Start in a view without a default action
        {
            if (window == owner && owner.ActivateLobbyControl(target)) return;
            if (combo != null) combo.DroppedDown = !combo.DroppedDown;
            else if (target is IButtonControl button) button.PerformClick();
            else if (target is CheckBox check) check.Checked = !check.Checked;
            else if (target is RadioButton radio) radio.Checked = true;
            else window.AcceptButton?.PerformClick();
            return;
        }
        if ((buttons & 15) == 0) return;
        int step = (buttons & 5) != 0 ? -1 : 1;
        bool horizontal = (buttons & 12) != 0;
        if (combo != null && (combo.DroppedDown || horizontal) && combo.Items.Count > 0)
            combo.SelectedIndex = Math.Clamp(combo.SelectedIndex+step,0,combo.Items.Count-1);
        else if (horizontal && target is TabControl tab && tab.TabCount > 0)
            tab.SelectedIndex = Math.Clamp(tab.SelectedIndex+step,0,tab.TabCount-1);
        else if (horizontal && target is TrackBar slider)
            slider.Value = Math.Clamp(slider.Value+step*slider.SmallChange,slider.Minimum,slider.Maximum);
        else if (horizontal && target is NumericUpDown number)
            number.Value = Math.Clamp(number.Value+step*number.Increment,number.Minimum,number.Maximum);
        else if (horizontal && target is ListView list && list.Items.Count > 0)
        {
            var item = list.Items[Math.Clamp((list.FocusedItem?.Index ?? 0)+step,0,list.Items.Count-1)];
            item.Selected = item.Focused = true; item.EnsureVisible();
        }
        else if (horizontal && target is ListBox choices && choices.Items.Count > 0)
            choices.SelectedIndex = Math.Clamp(choices.SelectedIndex+step,0,choices.Items.Count-1);
        else window.SelectNextControl(target,step>0,true,true,true);
    }

    public void Dispose() => timer.Dispose();
}

