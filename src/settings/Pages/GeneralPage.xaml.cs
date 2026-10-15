// 全般のページ。サービスの状態と版を出し、インストール・アンインストールをする。
// インストールは自分で行わず CLI（DisplayRescueService.exe install）を起動し（ServiceSetup）、その間も画面は固まらない（async）
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using DisplayRescue.Settings.Interop;
using DisplayRescue.Settings.Services;

namespace DisplayRescue.Settings.Pages;

public partial class GeneralPage : UserControl
{
    private bool _olderThanInstalled;

    // アンインストールで設定も消えるので、キーとマスコットのページに読み直してもらう
    public event EventHandler? SettingsRemoved;

    public GeneralPage()
    {
        InitializeComponent();
        Refresh();
    }

    // サービスの状態と版、ボタンを今の状態に合わせる
    public void Refresh()
    {
        var state = Core.QueryServiceState();
        StatusText.Text = state switch
        {
            ServiceState.Running => "実行中です。割り当てたキーで切り替えられます。",
            ServiceState.Stopped => "停止しています。キーは効きません(再インストールすると起動します)。",
            ServiceState.Pending => "起動または停止の途中です。",
            ServiceState.NotInstalled => "インストールされていません。「インストール」でキーが使えるようになります。",
            _ => "状態を取得できませんでした。",
        };

        // 版。古い zip の設定アプリで再インストールすると古い版に戻るので、そのときは赤字で知らせる
        var current = Core.Version;
        var installed = Core.InstalledVersion;
        _olderThanInstalled = installed is not null && current < installed;
        VersionText.Text = installed is null
            ? $"この設定アプリのバージョン: {current}"
            : $"インストール済みのバージョン: {installed} / この設定アプリのバージョン: {current}"
              + (_olderThanInstalled ? "\n(この設定アプリのほうが古いため、再インストールすると古いバージョンに戻ります)" : "");
        if (_olderThanInstalled) VersionText.Foreground = WarningBrush;
        else VersionText.ClearValue(TextBlock.ForegroundProperty);

        var isInstalled = state != ServiceState.NotInstalled;

        // インストール済みのファイルが、この設定アプリのフォルダのものと違えば知らせる。版の番号が同じでも中身が違うことがある
        // （ビルドし直して、再インストールを忘れたときなど）
        if (isInstalled && DiffersFromInstalled() is { } differs)
        {
            VersionText.Text += $"\nインストール済みの{differs}が、この設定アプリのフォルダのものと違います。「再インストール」で入れ直してください。";
            VersionText.Foreground = WarningBrush;
        }
        InstallButton.Content = isInstalled ? "再インストール" : "インストール";
        UninstallButton.IsEnabled = isInstalled;
    }

    internal static Brush WarningBrush { get; } = new SolidColorBrush(Color.FromRgb(0xE8, 0x3B, 0x2A));

    // インストールするファイル（サービス本体・マスコット）のうち、インストール先と大きさか更新日時が違う最初のもの。同じなら null。
    // インストールはファイルをコピーする（CopyFileW は更新日時もそのまま写す）ので、同じものなら大きさも日時も一致する
    private static string? DiffersFromInstalled()
    {
        foreach (var name in new[] { "DisplayRescueService.exe", "DisplayRescueMascot.exe" })
        {
            var local = new FileInfo(Path.Combine(AppFiles.Folder, name));
            var installed = new FileInfo(Path.Combine(Core.InstallDirectory, name));
            if (!local.Exists) continue;    // 比べるものが無い
            if (!installed.Exists || local.Length != installed.Length || local.LastWriteTimeUtc != installed.LastWriteTimeUtc) return name;
        }
        return null;
    }

    private async void OnInstall(object sender, RoutedEventArgs e)
    {
        if (_olderThanInstalled && !Confirm("DisplayRescueの再インストール",
                "インストール済みのバージョンのほうが新しいです。再インストールすると、この設定アプリの古いバージョンに戻ります。\n続けますか？"))
        {
            return;
        }
        await RunAsync("install", "インストール");
    }

    private async void OnUninstall(object sender, RoutedEventArgs e)
    {
        if (!Confirm("DisplayRescueのアンインストール",
                "サービスと設定(キーの割り当てとマスコット)を削除します。アンインストールすると、キーで画面構成を戻せなくなります。\n続けますか？"))
        {
            return;
        }
        await RunAsync("uninstall", "アンインストール");
        SettingsRemoved?.Invoke(this, EventArgs.Empty);
    }

    // CLI を動かして出力を表示する。数秒かかる（サービスの停止を待つ）ので、その間はボタンを押せなくする
    private async Task RunAsync(string command, string what)
    {
        InstallButton.IsEnabled = UninstallButton.IsEnabled = false;
        OutputBox.Visibility = Visibility.Visible;
        OutputBox.Clear();
        try
        {
            var exitCode = await ServiceSetup.RunAsync(command, line =>
            {
                OutputBox.AppendText(line + Environment.NewLine);
                OutputBox.ScrollToEnd();
            });
            if (exitCode != 0) OutputBox.AppendText($"{what}に失敗しました(終了コード{exitCode})。{Environment.NewLine}");
            else if (command == "install") await ImportBundledMascotAsync();
        }
        catch (Exception ex)
        {
            OutputBox.AppendText($"{what}に失敗しました: {ex.Message}{Environment.NewLine}");
        }
        InstallButton.IsEnabled = true;
        Refresh();
    }

    // インストールできたら、マスコットのモデルとモーションがまだ何も入っていなければ、zip に入れた既定のもの（mascot\）を取り込む。
    // 変換に数秒かかる。失敗してもインストールは済んでいるので、知らせるだけ（マスコットのページから入れ直せる）
    private async Task ImportBundledMascotAsync()
    {
        try
        {
            var count = await MascotAssets.ImportBundledAsync();
            if (count > 0) OutputBox.AppendText($"同梱のマスコット(モデルとモーション{count}個)を入れました。出すには「マスコット」のページでオンにしてください。{Environment.NewLine}");
        }
        catch (Exception ex) when (ex is CoreException or IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        {
            OutputBox.AppendText($"同梱のマスコットを入れられませんでした: {ex.Message}{Environment.NewLine}");
        }
    }

    // 取り返しのつかない操作の前に「はい / いいえ」で確かめる。既定は「いいえ」（Enter の押し間違いで進まないように）
    private bool Confirm(string title, string text) =>
        MessageBox.Show(Window.GetWindow(this), text, title, MessageBoxButton.YesNo, MessageBoxImage.Warning, MessageBoxResult.No)
        == MessageBoxResult.Yes;
}
