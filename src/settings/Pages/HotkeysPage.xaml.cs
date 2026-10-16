// キーの割り当てのページ。決まりは CLI の bind と同じ（判定は C++ の config::IsValidHotkey / StealsNormalInput を Interop で呼ぶ）:
// ・保存できない入力（ホットキーにならない・2 つが同じキー）は止める
// ・Ctrl も Alt も無いキー（普段の入力を奪う）は、1 回目の保存で警告して止まり、もう一度押すと保存する（入力が変われば確認はやり直し）
// ・保存は 4 件まとめて置き換え（1 件ずつだと、キーの入れ替えの途中が「重なり」になる）→ サービスに読み直しを頼む
// ・入力欄にフォーカスがあるときだけ、割り当て済みのキーをフックで握りつぶして入力欄に入れる（KeyboardHook）
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using DisplayRescue.Settings.Controls;
using DisplayRescue.Settings.Interop;

namespace DisplayRescue.Settings.Pages;

public partial class HotkeysPage : UserControl
{
    private static readonly Topology[] AllTopologies = [Topology.Extend, Topology.Clone, Topology.Internal, Topology.External];

    private const double IconSize = 32;

    private readonly Dictionary<Topology, HotkeyBox> _boxes = [];
    private readonly List<System.Windows.Controls.Image> _icons = [];
    private IReadOnlyList<Binding>? _saved;    // 最後に読み込んだ・保存した割り当て。読めなかったら null（必ず保存する）
    private bool _riskConfirmed;               // 普段の入力を奪うキーの警告を出した後か
    private bool _loading;                     // 入力欄にまとめて値を入れている間（いちいち確かめない）
    private KeyboardHook? _hook;

    public HotkeysPage()
    {
        InitializeComponent();
        BuildRows();
        Reload();
        // ページが見えている間だけフックを付ける（別のページに移ると外れる）
        Loaded += (_, _) => _hook ??= new KeyboardHook(ShouldSwallow);
        Unloaded += (_, _) => { _hook?.Dispose(); _hook = null; };
        // アイコンは表示の倍率が分かってから選ぶ（モニタをまたいで倍率が変わったら選び直す）
        Loaded += (_, _) =>
        {
            foreach (var icon in _icons)
                PickIcon(icon, VisualTreeHelper.GetDpi(icon).DpiScaleX);
        };
    }

    // .ico には 16〜256 の画像が入っている。BitmapImage に渡すと最初の 16×16 を引き伸ばしてぼやけるので、
    // 実際に描くピクセル数（IconSize × 倍率）以上でいちばん小さい画像を選ぶ
    private static void PickIcon(System.Windows.Controls.Image icon, double scale)
    {
        var pixels = IconSize * scale;
        var frames = BitmapDecoder.Create((Uri)icon.Tag, BitmapCreateOptions.None, BitmapCacheOption.OnLoad).Frames;
        icon.Source = frames.Where(f => f.PixelWidth >= pixels).MinBy(f => f.PixelWidth)
                   ?? frames.MaxBy(f => f.PixelWidth);
    }

    // 保存していない変更があるか（窓を閉じるときに確かめる）
    public bool IsDirty => _saved is null || !SameBindings(_saved, CurrentBindings());

    public static string DisplayName(Topology topology) => topology switch
    {
        Topology.Extend => "拡張",
        Topology.Clone => "複製",
        Topology.Internal => "メインのみ",
        Topology.External => "サブのみ",
        _ => topology.ToString(),
    };

    // 保存してある割り当てを入力欄に出す
    public void Reload()
    {
        try
        {
            _saved = Core.LoadBindings();
            Fill(_saved);
            ShowMessage("");
        }
        catch (CoreException ex)
        {
            _saved = null;
            ShowMessage(ex.Message, warning: true);
        }
    }

    private void BuildRows()
    {
        // 1 行 1 枚のカード: アイコン・名前・入力欄・解除
        foreach (var topology in AllTopologies)
        {
            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(240) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            Rows.Children.Add(new Border { Style = (Style)FindResource("Card"), Child = grid });

            var icon = new System.Windows.Controls.Image
            {
                Width = IconSize, Height = IconSize, Margin = new Thickness(0, 0, 16, 0), VerticalAlignment = VerticalAlignment.Center,
                Tag = new Uri($"pack://application:,,,/Assets/{topology.ToString().ToLowerInvariant()}.ico"),
            };
            icon.DpiChanged += (_, e) => PickIcon(icon, e.NewDpi.DpiScaleX);   // 倍率の違うモニタに移ったら選び直す
            _icons.Add(icon);
            var name = new TextBlock { Text = DisplayName(topology), VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(0, 0, 16, 0), MinWidth = 80 };
            var box = new HotkeyBox { Margin = new Thickness(0, 0, 8, 0), VerticalAlignment = VerticalAlignment.Center, VerticalContentAlignment = VerticalAlignment.Center };
            box.HotkeyChanged += (_, _) => OnHotkeyChanged(topology);
            var clear = new Button { Content = "解除", Padding = new Thickness(12, 4, 12, 4), VerticalAlignment = VerticalAlignment.Center };
            clear.Click += (_, _) => box.Hotkey = default;

            foreach (var (element, column) in new (UIElement, int)[] { (icon, 0), (name, 1), (box, 2), (clear, 3) })
            {
                Grid.SetColumn(element, column);
                grid.Children.Add(element);
            }
            _boxes[topology] = box;
        }
    }

    private void Fill(IReadOnlyList<Binding> bindings)
    {
        _loading = true;
        foreach (var topology in AllTopologies)
        {
            _boxes[topology].Hotkey = bindings.FirstOrDefault(b => b.Topology == topology)?.Hotkey ?? default;
        }
        _loading = false;
        _riskConfirmed = false;
    }

    private List<Binding> CurrentBindings() =>
        AllTopologies.Where(t => !_boxes[t].Hotkey.IsEmpty).Select(t => new Binding(t, _boxes[t].Hotkey)).ToList();

    private static bool SameBindings(IReadOnlyList<Binding> a, IReadOnlyList<Binding> b) =>
        a.Count == b.Count && a.All(b.Contains);

    // 入力欄を全部見る。problem: 保存できない入力の最初の 1 つ。risky: 保存はできるが確認が要るもの
    private (List<Binding> Bindings, (Topology Topology, string Message)? Problem, List<Binding> Risky) Collect()
    {
        var bindings = new List<Binding>();
        var risky = new List<Binding>();
        (Topology, string)? problem = null;
        foreach (var topology in AllTopologies)
        {
            var hotkey = _boxes[topology].Hotkey;
            if (hotkey.IsEmpty) continue;
            if (!Core.IsValid(hotkey))
            {
                problem ??= (topology, $"「{DisplayName(topology)}」のキーはホットキーにできません。");
                continue;
            }
            if (bindings.FirstOrDefault(b => b.Hotkey == hotkey) is { } same)
            {
                problem ??= (topology, $"「{DisplayName(same.Topology)}」と「{DisplayName(topology)}」が同じキー({Core.Format(hotkey)})です。");
                continue;
            }
            bindings.Add(new Binding(topology, hotkey));
            if (Core.StealsNormalInput(hotkey)) risky.Add(new Binding(topology, hotkey));
        }
        return (bindings, problem, risky);
    }

    // 入力が変わったら、すぐ確かめてメッセージに出す
    private void OnHotkeyChanged(Topology changed)
    {
        if (_loading) return;
        _riskConfirmed = false;
        var (_, problem, _) = Collect();
        if (problem is { } p)
        {
            ShowMessage(p.Message, warning: true);
            return;
        }
        var hotkey = _boxes[changed].Hotkey;
        if (hotkey.IsEmpty)
        {
            ShowMessage($"「{DisplayName(changed)}」は割り当てなしになります。");
        }
        else if (Core.StealsNormalInput(hotkey))
        {
            ShowMessage($"{Core.Format(hotkey)}にはCtrlもAltも付いていないため、このキーはWindows全体で使えなくなります。保存するときに確認します。", warning: true);
        }
        else
        {
            ShowMessage($"「{DisplayName(changed)}」を{Core.Format(hotkey)}にします。「保存」で保存します。");
        }
    }

    private void OnSave(object sender, RoutedEventArgs e) => Save();

    // 保存してサービスに反映する。保存できたら（変更が無いときも）true
    public bool Save()
    {
        var (bindings, problem, risky) = Collect();
        if (problem is { } p)
        {
            ShowMessage(p.Message, warning: true);
            _boxes[p.Topology].Focus();
            return false;
        }
        if (_saved is not null && SameBindings(_saved, bindings))
        {
            ShowMessage("変更はありません。");
            return true;
        }
        // 止めはしないが、知らずに自分を締め出さないよう、1 度目は警告して止まる。もう一度押されたら保存する
        if (risky.Count > 0 && !_riskConfirmed)
        {
            var list = string.Join("、", risky.Select(b => $"「{DisplayName(b.Topology)}」の{Core.Format(b.Hotkey)}"));
            ShowMessage($"{list}はWindows全体で使えなくなり、ログイン画面のパスワードも打てなくなるおそれがあります。それでよければ、もう一度「保存」を押すと保存します。", warning: true);
            _riskConfirmed = true;
            return false;
        }

        try
        {
            // 重なりは Collect で確かめ済みなので、ふつうはここには来ない
            if (Core.ReplaceBindings(bindings) is { } conflict)
            {
                ShowMessage($"「{DisplayName(conflict)}」のキーがほかの操作と同じなので、保存しませんでした。", warning: true);
                return false;
            }
            _saved = bindings;
            _riskConfirmed = false;
        }
        catch (CoreException ex)
        {
            ShowMessage(ex.Message, warning: true);
            return false;
        }

        // ホットキー監視プロセスは起動時にしか割り当てを読み込まないため、サービスに再起動を依頼する
        ShowMessage(Core.RequestServiceReload()
            ? "保存しました。新しいキーで切り替えられます。"
            : "保存しました。サービスが動き出すと有効になります。");
        return true;
    }

    private void OnRevert(object sender, RoutedEventArgs e)
    {
        Reload();
        ShowMessage("保存してある割り当てに戻しました。");
    }

    private void OnResetDefaults(object sender, RoutedEventArgs e)
    {
        try
        {
            var defaults = Core.DefaultBindings();
            Fill(defaults);
            ShowMessage(_saved is not null && SameBindings(_saved, defaults)
                ? "既定の割り当てにしました(今の設定と同じです)。"
                : "既定の割り当てにしました。「保存」で保存します。");
        }
        catch (CoreException ex)
        {
            ShowMessage(ex.Message, warning: true);
        }
    }

    // フックから呼ばれる（UI のスレッド）。窓が前面・入力欄にフォーカス・割り当て済みの組み合わせなら、入力欄に入れて握りつぶす
    private bool ShouldSwallow(uint vk)
    {
        if (_saved is null || Core.IsModifierKey(vk)) return false;
        if (Window.GetWindow(this) is not { IsActive: true }) return false;
        if (_boxes.Values.FirstOrDefault(b => b.IsKeyboardFocused) is not { } box) return false;

        var pressed = new Hotkey(KeyboardHook.CurrentModifiers(), vk);
        if (!_saved.Any(b => b.Hotkey == pressed)) return false;
        // フックの中は手早く済ませる決まりなので、入力欄の更新は後回しにする
        Dispatcher.BeginInvoke(() => box.Hotkey = pressed);
        return true;
    }

    private void ShowMessage(string text, bool warning = false)
    {
        MessageText.Text = text;
        if (warning) MessageText.Foreground = GeneralPage.WarningBrush;
        else MessageText.ClearValue(TextBlock.ForegroundProperty);
    }
}
