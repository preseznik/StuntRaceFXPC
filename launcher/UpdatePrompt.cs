namespace StuntRaceFX.Launcher;

// Both interfaces share the same owned confirmation and progress dialog.
internal sealed class UpdatePrompt
{
    private sealed record Owner(IntPtr Handle) : IWin32Window;
    private bool checking;

    internal async Task Check(bool manual, Func<bool> available, Func<IntPtr> handle, Action restart)
    {
        if (checking || !available() || (!manual && !LauncherUpdate.Portable)) return;
        checking = true;
        bool installing = false;
        try
        {
            var release = await LauncherRelease.LatestAsync();
            if (!available()) return;
            var owner = new Owner(handle());
            const string title = "Launcher updates";
            if (release == null)
            {
                if (manual) MessageBox.Show(owner, $"Build {GameContent.CurrentVersion} is up to date.", title);
                return;
            }
            if (!LauncherUpdate.Portable)
            {
                if (manual) MessageBox.Show(owner, $"Build {release.Build} is available. Open the portable launcher to install it.", title);
                return;
            }
            if (LauncherUpdate.ActiveBuild(LauncherUpdate.Home) < release.Build)
            {
                var offer = $"Build {release.Build} is available from GitHub. Download and install it ({release.Size / 1_000_000.0:F0} MB)?";
                if (MessageBox.Show(owner, offer, title, MessageBoxButtons.YesNo, MessageBoxIcon.Information) != DialogResult.Yes) return;
                installing = true;
                using var dialog = new Form { Text = title, StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog,
                    MinimizeBox = false, MaximizeBox = false, ClientSize = new Size(360, 85), ShowInTaskbar = false };
                var label = new Label { Dock = DockStyle.Top, Padding = new Padding(10), Height = 38, Text = "Connecting to GitHub…" };
                var cancelButton = new Button { Text = "Cancel", AutoSize = true, Location = new Point(10, 42) };
                dialog.Controls.AddRange([label, cancelButton]);
                using var cancel = new CancellationTokenSource();
                var progress = new Progress<string>(message => { if (!label.IsDisposed) label.Text = message; });
                Exception? failure = null;
                bool finished = false;
                void Cancel() { cancel.Cancel(); label.Text = "Cancelling…"; cancelButton.Enabled = false; }
                cancelButton.Click += (_, _) => Cancel();
                dialog.FormClosing += (_, e) => { if (!finished) { e.Cancel = true; Cancel(); } };
                dialog.Shown += async (_, _) =>
                {
                    try { await Task.Run(() => release.DownloadAsync(LauncherUpdate.Home, progress, cancel.Token)); }
                    catch (Exception ex) { failure = ex; }
                    finally { finished = true; dialog.Close(); }
                };
                dialog.ShowDialog(owner);
                if (failure is OperationCanceledException && cancel.IsCancellationRequested) return;
                if (failure != null) throw failure;
            }
            if (available() && MessageBox.Show(owner, $"Build {release.Build} is ready. Restart the launcher now?", title,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Information) == DialogResult.Yes) restart();
        }
        catch (Exception e)
        {
            if ((manual || installing) && available()) MessageBox.Show(new Owner(handle()), e.Message, "Launcher update", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        finally { checking = false; }
    }
}
