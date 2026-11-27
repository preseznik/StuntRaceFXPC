using System.Windows;
using System.Windows.Automation.Peers;
using System.Windows.Automation.Provider;
using System.Windows.Input;
using Button = System.Windows.Controls.Button;
using ComboBox = System.Windows.Controls.ComboBox;
using Slider = System.Windows.Controls.Slider;
using CheckBox = System.Windows.Controls.CheckBox;

namespace StuntRaceFX.Launcher.Modern;

internal partial class GridWindow
{
    private readonly LauncherGamepad gamepad = new();

    private bool probingPad;

    private async void PollPad()
    {
        if (probingPad) return;
        probingPad = true;
        ushort? buttons;
        try
        {
            // Probing absent XInput slots can stall long enough to skip several UI frames.
            buttons = await Task.Run(() => gamepad.TryRead(out var down) ? (ushort?)down : null);
        }
        finally { probingPad = false; }
        if (!IsActive || closed) return;
        padTimer.Interval = buttons.HasValue ? TimeSpan.FromMilliseconds(90) : TimeSpan.FromSeconds(3);
        if (buttons is not ushort down) { lastButtons = ushort.MaxValue; return; }
        int pressed = down & ~lastButtons;
        lastButtons = down;
        if (pressed == 0) return;
        var focus = Keyboard.FocusedElement as UIElement;
        ComboBox? combo = focus as ComboBox;
        if (focus is System.Windows.Controls.ComboBoxItem row)
            combo = System.Windows.Controls.ItemsControl.ItemsControlFromItemContainer(row) as ComboBox;
        if (combo is { IsDropDownOpen: true } && combo.Items.Count > 0)
        {
            if ((pressed & 3) != 0) combo.SelectedIndex = Math.Clamp(combo.SelectedIndex + ((pressed & 1) != 0 ? -1 : 1), 0, combo.Items.Count - 1);
            if ((pressed & 0x3000) != 0) combo.IsDropDownOpen = false;
            return;
        }
        if ((pressed & 0x300) != 0)
        {
            var destinations = new[] { Page.Play, Page.Network, Page.Settings };
            int current = page == Page.Setup ? 0 : Array.IndexOf(destinations, page);
            ShowPage(destinations[(current + ((pressed & 0x100) != 0 ? 2 : 1)) % 3]);
            (page switch { Page.Network => LanNav, Page.Settings => SettingsNav, _ => PlayNav }).Focus();
            return;
        }
        if ((pressed & 0x2000) != 0) { ShowPage(Page.Play); FocusPlayAction(); return; }
        if ((pressed & 0x1000) != 0)
        {
            if (focus is ComboBox picker) picker.IsDropDownOpen = true;
            else if (GalleryVisible && Tiles.IsKeyboardFocusWithin) { ShowPage(Page.Play); FocusPlayAction(); }
            else if (focus is System.Windows.Controls.MenuItem item)
                ((IInvokeProvider)new MenuItemAutomationPeer(item)).Invoke();
            else if (focus is System.Windows.Controls.RadioButton mode) mode.IsChecked = true;
            else if (focus is CheckBox check) ((IToggleProvider)new CheckBoxAutomationPeer(check)).Toggle();
            else if (focus is Button button && button.IsEnabled)
                ((IInvokeProvider)new ButtonAutomationPeer(button)).Invoke();
            return;
        }
        if ((pressed & 0x10) != 0 && page == Page.Play && Start.IsEnabled) { StartGame(this, new RoutedEventArgs()); return; }
        if ((pressed & 12) != 0 && combo is { Items.Count: > 0 })
        {
            combo.SelectedIndex = Math.Clamp(combo.SelectedIndex + ((pressed & 4) != 0 ? -1 : 1), 0, combo.Items.Count - 1);
            return;
        }
        if ((pressed & 12) != 0 && focus is Slider slider)
        {
            slider.Value = Math.Clamp(slider.Value + ((pressed & 4) != 0 ? -slider.SmallChange : slider.SmallChange), slider.Minimum, slider.Maximum);
            return;
        }
        var direction = (pressed & 15) switch
        {
            1 => FocusNavigationDirection.Up,
            2 => FocusNavigationDirection.Down,
            4 => FocusNavigationDirection.Left,
            8 => FocusNavigationDirection.Right,
            _ => (FocusNavigationDirection?)null
        };
        if (direction is { } move) (focus ?? this).MoveFocus(new TraversalRequest(move));
    }
}
