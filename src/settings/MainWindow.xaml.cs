// ページの切り替えと、閉じるときの確認、Interop の DLL の版の表示。
// ページは最初に 1 つずつ作って使い回す（切り替えても入力中の内容が消えないように）
using System.ComponentModel;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using DisplayRescue.Settings.Interop;
using DisplayRescue.Settings.Pages;

namespace DisplayRescue.Settings;

public partial class MainWindow : Window
{
    private readonly GeneralPage _general = new();
    private readonly HotkeysPage _hotkeys = new();
    private readonly MascotPage _mascot = new();

    public MainWindow()
    {
        InitializeComponent();
        // アンインストールで設定が消えるので、ほかのページに読み直してもらう
        _general.SettingsRemoved += (_, _) => { _hotkeys.Reload(); _mascot.Reload(); };
        // 最初のページを選ぶ（OnPageChanged が呼ばれて右に出る）。XAML の IsSelected で選ぶと、
        // 右の部品がまだ作られていないうちに通知が来て落ちるので、作り終わったここで選ぶ
        Pages.SelectedIndex = 0;
        VersionText.Text = DescribeVersions();
    }

    private void OnPageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (Pages.SelectedItem is not ListBoxItem item) return;
        PageHost.Content = item.Tag switch
        {
            "Hotkeys" => _hotkeys,
            "Mascot" => _mascot,
            _ => _general,
        };
        // 状態は変わっているかもしれないので、見せるたびに調べ直す
        if (PageHost.Content == _general) _general.Refresh();
        if (PageHost.Content == _mascot) _mascot.Refresh();
    }

    // 保存していない変更があれば確かめる（キーの割り当て → マスコットの順）
    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (!AskSave(_hotkeys.IsDirty, "キーの割り当て", _hotkeys.Save, 1)) e.Cancel = true;
        else if (!AskSave(_mascot.IsDirty, "マスコット", _mascot.Save, 2)) e.Cancel = true;
    }

    // 閉じてよければ true。page は止めたときに開くページの番号（何が残っているか見えるように）
    private bool AskSave(bool dirty, string what, Func<bool> save, int page)
    {
        if (!dirty) return true;
        var answer = MessageBox.Show(this, $"{what}の変更を保存していません。保存しますか？", "DisplayRescueの設定",
            MessageBoxButton.YesNoCancel, MessageBoxImage.Question, MessageBoxResult.Cancel);
        if (answer == MessageBoxResult.No || (answer == MessageBoxResult.Yes && save())) return true;
        Pages.SelectedIndex = page;
        return false;
    }

    // 「設定アプリ1.1.0 / Interop 1.1.0」のように並べる。DLL が無い・読めないときは、そう書く
    private static string DescribeVersions()
    {
        var app = Assembly.GetExecutingAssembly().GetName().Version;
        var appText = app is null ? "?" : $"{app.Major}.{app.Minor}.{app.Build}";
        try
        {
            var interop = Core.Version;
            var mismatch = app is not null && (interop.Major, interop.Minor, interop.Build) != (app.Major, app.Minor, app.Build)
                ? "(バージョンが違います。同じzipのDisplayRescueInterop.dllを置いてください)" : "";
            return $"設定アプリ{appText} / Interop {interop}{mismatch}";
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            return $"設定アプリ{appText} / InteropのDLLを読めません: {e.GetType().Name}";
        }
    }
}
