using System.Windows;
using System.Windows.Controls;
using Button = System.Windows.Controls.Button;
using Orientation = System.Windows.Controls.Orientation;

namespace StuntRaceFX.Launcher.Modern;

internal partial class GridWindow
{
    private LauncherLook look;
    private ResourceDictionary? themeColors;
    private bool DeskHome => page == Page.Play && look == LauncherLook.RaceDesk;
    private bool GalleryVisible => page == Page.Setup || (page == Page.Play && look == LauncherLook.CourseAtlas);

    private void ChangeLook(LauncherLook value)
    {
        if (look == value) return;
        look = value;
        settings = settings with { WpfTheme = value.ToString() };
        ApplyLook();
        SetPlayLayout();
        RefreshSetup();
    }

    private void ApplyLook()
    {
        if (themeColors != null) Resources.MergedDictionaries.Remove(themeColors);
        themeColors = null;
        if (look != LauncherLook.StartingGrid)
        {
            themeColors = new ResourceDictionary
            {
                Source = new Uri($"/StuntRaceFX.Launcher;component/Modern/{look}.xaml", UriKind.Relative)
            };
            Resources.MergedDictionaries.Add(themeColors);
        }
        ApplyContrast();
        bool desk = look == LauncherLook.RaceDesk;
        bool atlas = look == LauncherLook.CourseAtlas;
        NavRailWidth.Width = new GridLength(desk ? 150 : 0);
        TopNavHeight.Height = new GridLength(atlas ? 52 : 0);
        BottomNavHeight.Height = new GridLength(!desk && !atlas ? 54 : 0);
        Grid.SetRow(NavBar, desk ? 2 : atlas ? 1 : 4);
        Grid.SetColumnSpan(NavBar, desk ? 1 : 2);
        Grid.SetRowSpan(NavBar, desk ? 2 : 1);
        NavBar.BorderThickness = desk ? new Thickness(0, 0, 1, 0) : atlas ? new Thickness(0, 0, 0, 1) : new Thickness(0, 1, 0, 0);
        NavItems.Orientation = desk ? Orientation.Vertical : Orientation.Horizontal;
        NavItems.Margin = desk ? new Thickness(8, 18, 8, 0) : new Thickness(14, 4, 14, 4);
        foreach (Button link in NavItems.Children)
        {
            link.HorizontalContentAlignment = desk ? System.Windows.HorizontalAlignment.Left : System.Windows.HorizontalAlignment.Center;
            link.Padding = desk ? new Thickness(10, 10, 10, 10) : new Thickness(20, 8, 20, 8);
            link.Margin = desk ? new Thickness(0, 0, 0, 5) : new Thickness(0);
        }

        // In Atlas the selected lobby's course photograph leads the driver column.
        var previewColumn = atlas ? DriverPanel : LobbyBoard;
        if (NetworkPreview.Parent != previewColumn)
        {
            ((StackPanel)NetworkPreview.Parent).Children.Remove(NetworkPreview);
            previewColumn.Children.Insert(atlas ? 0 : previewColumn.Children.Count, NetworkPreview);
        }

        // With a persistent destination rail, section tabs give the editor the full remaining width.
        DockPanel.SetDock(SettingsSections, desk ? Dock.Top : Dock.Left);
        SettingsSections.Width = desk ? double.NaN : 175;
        SettingsSections.Margin = desk ? new Thickness(0, 0, 0, 8) : new Thickness(0, 0, 16, 0);
        SettingsRail.Orientation = desk ? Orientation.Horizontal : Orientation.Vertical;
        foreach (Button link in SettingsRail.Children)
            link.Padding = desk ? new Thickness(11, 6, 11, 6) : new Thickness(16, 8, 16, 8);
    }

    private void SetPlayLayout()
    {
        bool inline = page == Page.Play && look != LauncherLook.StartingGrid;
        Home.Visibility = Show(page == Page.Play && !inline);
        Setup.Visibility = Show(inline || page == Page.Setup);
        ApplySetup.Visibility = Show(page == Page.Setup);
        InlineActions.Visibility = Show(inline);
        Grid.SetColumn(InlineActions, DeskHome ? 0 : 1);
        Grid.SetColumnSpan(InlineActions, DeskHome ? 2 : 1);
        Grid.SetRowSpan(CatalogPane, DeskHome ? 1 : 2);
        Grid.SetRowSpan(FieldPane, inline ? 1 : 2);
        DeskModeBar.Visibility = Show(DeskHome);
        ModeLabel.Visibility = ModeChoice.Visibility = Show(!DeskHome);
    }

    private void FocusPlayAction()
    {
        var launch = look == LauncherLook.StartingGrid ? Start : InlineStart;
        var repair = look == LauncherLook.StartingGrid ? Repair : InlineRepair;
        (launch.IsEnabled ? launch : repair).Focus();
    }
}
