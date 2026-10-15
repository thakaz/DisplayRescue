// マスコットのページ。キーの割り当てのページ（HotkeysPage）と同じ形: 画面の値を集める → 「保存」で C++ に渡す → 130 で読み直し。
// 違いは、モデルとモーションのファイルは「選んだらすぐ入れる・外したらすぐ消す」こと（保存ボタンを待たない）。
// コピーの前に置き場所を整える（Core.PrepareMascotDirectory。SYSTEM のマスコットが読むので、管理者と SYSTEM しか書けない場所にする）
// モデルとモーションは、そのままコピーせず、マスコットが読む形式に変換して入れる（Services/MascotAssets.cs）
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using DisplayRescue.Settings.Interop;
using DisplayRescue.Settings.Services;
using Microsoft.Win32;

namespace DisplayRescue.Settings.Pages;

public partial class MascotPage : UserControl
{
    private MascotSettings? _saved;     // 最後に読み込んだ・保存した設定。読めなかったら null
    // 位置・大きさ・向き。画面の部品では持たず、ここに持つ（調整は別の窓でするため）
    private (int X, int Y, int Scale, int Yaw, int Pitch) _placement = DefaultPlacement;
    private static readonly (int X, int Y, int Scale, int Yaw, int Pitch) DefaultPlacement = (870, 660, 45, 0, 0);
    private bool _loading;              // 画面にまとめて値を入れている間（いちいち確かめない）

    public MascotPage()
    {
        InitializeComponent();
        Reload();
    }

    // 保存していない変更があるか（窓を閉じるときに確かめる）
    public bool IsDirty => _saved is not null && Collect() != _saved;

    // 見せるたびに呼ぶ（インストールしたかどうか、ファイルが変わったかを調べ直す）
    public void Refresh()
    {
        var installed = Core.QueryServiceState() != ServiceState.NotInstalled;
        NotInstalledText.Visibility = installed ? Visibility.Collapsed : Visibility.Visible;
        Body.IsEnabled = Buttons.IsEnabled = installed;
        ShowFiles();
        if (installed) UpgradeOnce();
    }

    public void Reload()
    {
        try
        {
            _saved = Core.LoadMascotSettings();
            Fill(_saved);
            ShowMessage("");
        }
        catch (CoreException ex)
        {
            _saved = null;
            ShowMessage(ex.Message, warning: true);
        }
        Refresh();
    }

    private void Fill(MascotSettings s)
    {
        _loading = true;
        EnabledBox.IsChecked = s.Enabled;
        DesktopBox.IsChecked = s.ShowOnDesktop;
        ShortcutsBox.IsChecked = s.ShowShortcuts;
        ResidentBox.IsChecked = s.KeepResident;
        IntervalSlider.Value = s.MotionIntervalSeconds;
        FadeSlider.Value = s.FadeMilliseconds;
        IdleFadeSlider.Value = s.IdleFadeMilliseconds;
        _placement = (s.PositionX, s.PositionY, s.ScalePercent, s.YawDegrees, s.PitchDegrees);
        ShowPlacement();
        ShowFiles();
        _loading = false;
        UpdateSliderTexts();
    }

    // 画面の値を集める
    private MascotSettings Collect() => new(
        EnabledBox.IsChecked == true,
        DesktopBox.IsChecked == true,
        _placement.X,
        _placement.Y,
        _placement.Scale,
        _placement.Yaw,
        _placement.Pitch,
        (int)IntervalSlider.Value,
        (int)FadeSlider.Value,
        (int)IdleFadeSlider.Value,
        ShortcutsBox.IsChecked == true,
        ResidentBox.IsChecked == true);

    // モデル・待機モーションの状態と、モーションの一覧を出す
    private void ShowFiles()
    {
        // 名前は取っておいた元のファイル（mascot-sources）のもの。日時は変換した日時（入れた日時）
        var model = new FileInfo(Core.MascotModelPath);
        var modelSource = MascotAssets.SourceModel;
        ModelText.Text = !model.Exists ? "まだ入っていません。"
            : $"{(modelSource is null ? "(元の名前は不明)" : Path.GetFileName(modelSource))}({model.LastWriteTime:yyyy-MM-dd HH:mm}に入れました)";

        // 待機モーションは mascot-idle.motion（モーションのフォルダの外）
        var idle = new FileInfo(Core.MascotIdleMotionPath);
        var idleSource = MascotAssets.SourceIdle;
        IdleText.Text = !idle.Exists ? "なし" : idleSource is null ? $"あり({idle.LastWriteTime:yyyy-MM-dd HH:mm})" : Path.GetFileName(idleSource);

        MotionList.ItemsSource = MascotAssets.ListMotions();
    }

    // 取り込み（変換）の間は画面を触れなくし、メッセージを出す。終わったら一覧を出し直す
    private async Task RunImportAsync(string working, Func<Task<string>> import, string failure)
    {
        Body.IsEnabled = Buttons.IsEnabled = false;
        ShowMessage(working);
        try
        {
            ShowMessage(await import());
        }
        // Win32Exception: 変換の exe を起動できなかった（async void から外に出すと設定アプリが落ちるので、ここで受け止める）
        catch (Exception ex) when (ex is CoreException or IOException or UnauthorizedAccessException or System.ComponentModel.Win32Exception)
        {
            ShowMessage($"{failure}: {ex.Message}", warning: true);
        }
        finally
        {
            Body.IsEnabled = Buttons.IsEnabled = true;
            ShowFiles();
        }
    }

    // 形式の版が変わったときの変換し直し。ページを開いて最初に見せたとき 1 回だけ（インストールしてあるとき）
    private bool _upgraded;
    private async void UpgradeOnce()
    {
        if (_upgraded) return;
        _upgraded = true;
        await RunImportAsync("マスコットのファイルを確かめています...", async () =>
        {
            var count = await MascotAssets.UpgradeAsync();
            if (count == 0) return "";
            Core.RequestServiceReload();
            return $"マスコットのファイルを今の形式に変換しました({count}個)。";
        }, "マスコットのファイルを変換できませんでした");
    }

    private void OnChanged(object sender, RoutedEventArgs e)
    {
        if (!_loading && IsLoaded) ShowMessage("「保存」で保存します。");
    }

    private void OnSliderChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        UpdateSliderTexts();
        OnChanged(sender, e);
    }

    private void UpdateSliderTexts()
    {
        // XAML の読み込み中（まだ全部の部品が無い）にも呼ばれるので、無ければ何もしない
        if (IntervalText is null || FadeText is null || IdleFadeText is null) return;
        IntervalText.Text = $"{(int)IntervalSlider.Value}秒";
        FadeText.Text = $"{FadeSlider.Value / 1000:0.0}秒";
        IdleFadeText.Text = $"{IdleFadeSlider.Value / 1000:0.0}秒";
    }

    // 選んだファイルは、マスコットが読む形式に変換して入れる（MascotAssets。数秒かかる）
    private async void OnChooseModel(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "モデルを選ぶ", Filter = "VRMのモデル (*.vrm)|*.vrm|glTFのモデル (*.glb)|*.glb" };
        if (dialog.ShowDialog(Window.GetWindow(this)) != true) return;
        var name = Path.GetFileName(dialog.FileName);
        await RunImportAsync($"{name}を変換しています...", async () =>
        {
            await MascotAssets.ImportModelAsync(dialog.FileName);
            return $"モデルを入れました: {name}。出ているマスコットに反映するには「保存」を押してください。";
        }, "モデルを入れられませんでした");
    }

    private async void OnChooseIdle(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "待機モーションを選ぶ", Filter = "VRMアニメーション (*.vrma)|*.vrma" };
        if (dialog.ShowDialog(Window.GetWindow(this)) != true) return;
        var name = Path.GetFileName(dialog.FileName);
        await RunImportAsync($"{name}を変換しています...", async () =>
        {
            await MascotAssets.ImportIdleAsync(dialog.FileName);
            return $"待機モーションを入れました: {name}。出ているマスコットに反映するには「保存」を押してください。";
        }, "待機モーションを入れられませんでした");
    }

    private void OnRemoveIdle(object sender, RoutedEventArgs e)
    {
        try
        {
            MascotAssets.RemoveIdle();
            ShowFiles();
            ShowMessage("待機モーションを外しました。出ているマスコットに反映するには「保存」を押してください。");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            ShowMessage($"待機モーションを外せませんでした: {ex.Message}", warning: true);
        }
    }

    private async void OnAddMotions(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFileDialog { Title = "モーションを追加", Filter = "VRMアニメーション (*.vrma)|*.vrma", Multiselect = true };
        if (dialog.ShowDialog(Window.GetWindow(this)) != true) return;
        var files = dialog.FileNames;
        await RunImportAsync($"モーションを{files.Length}本変換しています...", async () =>
        {
            foreach (var file in files) await MascotAssets.ImportMotionAsync(file);
            return $"モーションを{files.Length}本入れました。出ているマスコットに反映するには「保存」を押してください。";
        }, "モーションを入れられませんでした");
    }

    private void OnRemoveMotions(object sender, RoutedEventArgs e)
    {
        var selected = MotionList.SelectedItems.Cast<string>().ToList();
        if (selected.Count == 0)
        {
            ShowMessage("削除するモーションを一覧で選んでください。");
            return;
        }
        try
        {
            foreach (var name in selected) MascotAssets.RemoveMotion(name);
            ShowFiles();
            ShowMessage($"モーションを{selected.Count}本外しました。出ているマスコットに反映するには「保存」を押してください。");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            ShowMessage($"モーションを外せませんでした: {ex.Message}", warning: true);
        }
    }

    private void OnSave(object sender, RoutedEventArgs e) => Save();

    // 保存してサービスに反映する（マスコットを再起動する。読み込みに数秒かかる）。保存できたら true
    public bool Save()
    {
        var settings = Collect();
        try
        {
            Core.SaveMascotSettings(settings);
            _saved = Core.LoadMascotSettings();   // C++ が範囲に収めた値で持ち直す
        }
        catch (CoreException ex)
        {
            ShowMessage(ex.Message, warning: true);
            return false;
        }

        var hasModel = File.Exists(Core.MascotModelPath);
        var reloaded = Core.RequestServiceReload();
        ShowMessage(!settings.Enabled ? "保存しました。マスコットは出しません。"
            : !hasModel ? "保存しました。モデルを入れると出ます。"
            : reloaded ? "保存しました。数秒でマスコットが再起動し、設定が反映されます。"
            : "保存しました。サービスが動き出すと有効になります。");
        return true;
    }

    private void OnRevert(object sender, RoutedEventArgs e)
    {
        Reload();
        ShowMessage("保存してある設定に戻しました。");
    }

    // 「位置 左から87%・上から66% / 背の高さ 画面の高さの45% / 向き 左右0度・前後0度」
    private void ShowPlacement()
    {
        PlacementText.Text = $"位置 左から{_placement.X / 10}%・上から{_placement.Y / 10}% / 背の高さ 画面の高さの{_placement.Scale}% / "
            + $"向き 左右{_placement.Yaw}度・前後{_placement.Pitch}度";
    }

    private void OnResetPlacement(object sender, RoutedEventArgs e)
    {
        _placement = DefaultPlacement;
        ShowPlacement();
        ShowMessage("既定の配置にしました。「保存」で保存します。");
    }

    // マスコットの exe を --edit で起動し、終わるのを待つ。調整用の窓が Enter で設定に配置を書いて 0 で終わる。
    // 書かれた配置だけを取り込み、ほかの項目は画面の値のまま（まだ保存していない変更を消さないように）。そのあとサービスに再起動を頼む
    private async void OnEditPlacement(object sender, RoutedEventArgs e)
    {
        if (!File.Exists(Core.MascotModelPath))
        {
            ShowMessage("先にモデルを入れてください。", warning: true);
            return;
        }
        var exe = AppFiles.MascotExe;
        if (!File.Exists(exe))
        {
            ShowMessage("DisplayRescueMascot.exeが見つかりません。zipを展開したフォルダから開いてください。", warning: true);
            return;
        }
        StopPreview();   // 再生している途中なら止める（2 体重ならないように）
        Body.IsEnabled = Buttons.IsEnabled = false;
        ShowMessage("画面にマスコットが出ます(読み込みに数秒)。左ドラッグで移動、ホイールで大きさ、右ドラッグで回す、Rで回転を戻す。Enterで保存、Escでやめます。");
        try
        {
            // 調整は画面の今の配置から始める（「既定の配置」を押しただけで、まだ保存していなくても）
            var p = _placement;
            using var process = Process.Start(new ProcessStartInfo(exe, $"--edit {p.X} {p.Y} {p.Scale} {p.Yaw} {p.Pitch}") { UseShellExecute = false })!;
            await process.WaitForExitAsync();
            if (process.ExitCode == 0)
            {
                var fresh = Core.LoadMascotSettings();
                _placement = (fresh.PositionX, fresh.PositionY, fresh.ScalePercent, fresh.YawDegrees, fresh.PitchDegrees);
                // 保存済みのものも配置だけ新しくする（ほかの項目の「保存していない変更」はそのまま残る）
                if (_saved is not null)
                {
                    _saved = _saved with { PositionX = fresh.PositionX, PositionY = fresh.PositionY, ScalePercent = fresh.ScalePercent,
                        YawDegrees = fresh.YawDegrees, PitchDegrees = fresh.PitchDegrees };
                }
                ShowPlacement();
                ShowMessage(Core.RequestServiceReload()
                    ? "配置を保存しました。数秒でマスコットが再起動し、設定が反映されます。"
                    : "配置を保存しました。");
            }
            else
            {
                ShowMessage(process.ExitCode == 1223 ? "配置の調整をやめました。" : $"配置を調整できませんでした(終了コード{process.ExitCode})。",
                    warning: process.ExitCode != 1223);   // 1223 = ERROR_CANCELLED（Esc）
            }
        }
        catch (Exception ex) when (ex is CoreException or System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            ShowMessage($"配置を調整できませんでした: {ex.Message}", warning: true);
        }
        finally
        {
            Body.IsEnabled = Buttons.IsEnabled = true;
        }
    }

    // マスコットの exe を --preview で起動し、そのモーションをデスクトップで 1 回再生する。
    // 配置と切り替え時間は画面上の現在値を使う（保存前でも試せる）。ウィンドウはクリックを透過するため、停止時はプロセスを終了する。
    // 同時に再生するのは 1 つだけ（次を再生すると前のプレビューは停止する）。設定アプリを閉じた場合も停止する
    private Process? _preview;

    private void OnMotionDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (MotionList.SelectedItem is string) OnPreviewMotion(sender, e);
    }

    private void OnPreviewMotion(object sender, RoutedEventArgs e)
    {
        if (MotionList.SelectedItem is not string name)
        {
            ShowMessage("再生するモーションを一覧で選んでください。");
            return;
        }
        StartPreview(Path.Combine(Core.MascotMotionDirectory, name + ".motion"), name);
    }

    private void OnPreviewIdle(object sender, RoutedEventArgs e)
    {
        if (!File.Exists(Core.MascotIdleMotionPath))
        {
            ShowMessage("待機モーションが入っていません。");
            return;
        }
        StartPreview(Core.MascotIdleMotionPath, "待機モーション");
    }

    private void OnStopPreview(object sender, RoutedEventArgs e)
    {
        StopPreview();
        ShowMessage("");
    }

    private async void StartPreview(string motion, string name)
    {
        if (!File.Exists(Core.MascotModelPath))
        {
            ShowMessage("先にモデルを入れてください。", warning: true);
            return;
        }
        var exe = AppFiles.MascotExe;
        if (!File.Exists(exe))
        {
            ShowMessage("DisplayRescueMascot.exeが見つかりません。zipを展開したフォルダから開いてください。", warning: true);
            return;
        }
        StopPreview();
        var p = _placement;
        var startInfo = new ProcessStartInfo(exe) { UseShellExecute = false };
        foreach (var arg in new[] { "--preview", motion, $"{p.X}", $"{p.Y}", $"{p.Scale}", $"{p.Yaw}", $"{p.Pitch}",
                     $"{(int)FadeSlider.Value}", $"{(int)IdleFadeSlider.Value}" })
        {
            startInfo.ArgumentList.Add(arg);
        }
        Process process;
        try
        {
            process = Process.Start(startInfo)!;
        }
        catch (System.ComponentModel.Win32Exception ex)
        {
            ShowMessage($"再生できませんでした: {ex.Message}", warning: true);
            return;
        }
        _preview = process;
        StopPreviewButton.IsEnabled = true;
        EnsureStopOnExit();
        ShowMessage($"{name}を再生します(読み込みに数秒)。今の配置の位置に出ます。");

        await process.WaitForExitAsync();
        if (_preview != process) return;   // 次のを再生した・止めた（そちらがメッセージを出す）
        var exitCode = process.ExitCode;
        process.Dispose();
        _preview = null;
        StopPreviewButton.IsEnabled = false;
        ShowMessage(exitCode == 0 ? "" : $"再生できませんでした(終了コード{exitCode})。", warning: exitCode != 0);
    }

    private void StopPreview()
    {
        var process = _preview;
        _preview = null;
        StopPreviewButton.IsEnabled = false;
        if (process is null) return;
        try
        {
            if (!process.HasExited) process.Kill();
        }
        catch (InvalidOperationException)
        {
            // もう終わっていた
        }
        process.Dispose();
    }

    // 設定アプリを閉じたら、再生している途中のものも止める（1 回だけ登録する）
    private bool _stopOnExitHooked;
    private void EnsureStopOnExit()
    {
        if (_stopOnExitHooked) return;
        _stopOnExitHooked = true;
        Dispatcher.ShutdownStarted += (_, _) => StopPreview();
    }

    private void ShowMessage(string text, bool warning = false)
    {
        MessageText.Text = text;
        if (warning) MessageText.Foreground = GeneralPage.WarningBrush;
        else MessageText.ClearValue(TextBlock.ForegroundProperty);
    }
}
