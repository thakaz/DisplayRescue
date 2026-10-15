// NativeMethods（C++ の形のまま）を、C# らしい形に包む。画面のコードはここだけを使う。
// ・ポインタと配列 → fixed で一時的にアドレスを固定して渡す（GC に動かされないように）
// ・2 段階のバッファ → 大きさを聞いてから取り直す
// ・HRESULT → 例外（CoreException。メッセージは C++ の DescribeHresult の名前）
namespace DisplayRescue.Settings.Interop;

// 切り替え先。番号は C++ の ccd::Topology と同じ（並びを変えない）
public enum Topology
{
    Extend = 0,     // 拡張
    Clone = 1,      // 複製
    Internal = 2,   // メインのみ
    External = 3,   // サブのみ
}

// サービスの状態。番号は C++ の service::ServiceState と同じ
public enum ServiceState
{
    NotInstalled = 0,
    Stopped = 1,
    Running = 2,
    Pending = 3,
    Unknown = 4,
}

// キーの組み合わせ。Modifiers は RegisterHotKey の MOD_*（Alt 1 / Ctrl 2 / Shift 4）、Vk は仮想キー。Vk が 0 なら「割り当てなし」
public readonly record struct Hotkey(uint Modifiers, uint Vk)
{
    public const uint ModAlt = 0x1;
    public const uint ModControl = 0x2;
    public const uint ModShift = 0x4;

    public bool IsEmpty => Vk == 0;
}

public sealed record Binding(Topology Topology, Hotkey Hotkey);

// マスコットの設定。中身は C++ の config::MascotSettings と同じ（範囲は C++ が保存のときに収める）
public sealed record MascotSettings(
    bool Enabled,
    bool ShowOnDesktop,
    int PositionX,
    int PositionY,
    int ScalePercent,
    int YawDegrees,
    int PitchDegrees,
    int MotionIntervalSeconds,
    int FadeMilliseconds,
    int IdleFadeMilliseconds,
    bool ShowShortcuts,
    bool KeepResident);

// C++ から返った失敗。メッセージは「何をしようとしたか: ERROR_ACCESS_DENIED」の形。HResult に元の値
public sealed class CoreException : Exception
{
    public CoreException(int hresult, string what) : base($"{what}: {Core.DescribeHresult(hresult)}")
    {
        HResult = hresult;
    }
}

public static unsafe class Core
{
    // 割り当ては操作ごとに 1 件なので、多くても 4 件
    private const int MaxBindings = 4;

    public static Version Version
    {
        get
        {
            NativeMethods.GetVersion(out var major, out var minor, out var patch);
            return new Version(major, minor, patch);
        }
    }

    // インストールされていなければ null
    public static Version? InstalledVersion =>
        NativeMethods.GetInstalledVersion(out var major, out var minor, out var patch) != 0 ? new Version(major, minor, patch) : null;

    public static ServiceState QueryServiceState() => (ServiceState)NativeMethods.QueryServiceState();

    // インストール先のフォルダ（C:\Program Files\DisplayRescue）
    public static string InstallDirectory => ReadString(NativeMethods.GetInstallDirectory);

    // 送れたら true（サービスが無い・止まっていれば false）
    public static bool RequestServiceReload() => NativeMethods.RequestServiceReload() != 0;

    public static bool IsValid(Hotkey hotkey) => NativeMethods.IsValidHotkey(hotkey.Modifiers, hotkey.Vk) != 0;

    public static bool StealsNormalInput(Hotkey hotkey) => NativeMethods.StealsNormalInput(hotkey.Modifiers, hotkey.Vk) != 0;

    public static bool IsModifierKey(uint vk) => NativeMethods.IsModifierKey(vk) != 0;

    public static string Format(Hotkey hotkey) =>
        ReadString((buffer, capacity) => NativeMethods.FormatHotkey(hotkey.Modifiers, hotkey.Vk, buffer, capacity));

    public static string DescribeHresult(int hr) =>
        ReadString((buffer, capacity) => NativeMethods.DescribeHresult(hr, buffer, capacity));

    public static IReadOnlyList<Binding> LoadBindings() => ReadBindings(NativeMethods.LoadBindings, "設定を読み込めませんでした");

    public static IReadOnlyList<Binding> DefaultBindings() => ReadBindings(NativeMethods.DefaultBindings, "既定の割り当てを読めませんでした");

    // まとめて置き換える。同じキーが 2 つの操作にあれば何も書かず、その操作を返す（書けたら null）
    public static Topology? ReplaceBindings(IReadOnlyList<Binding> bindings)
    {
        var native = bindings.Select(b => new NativeMethods.Binding
        {
            Topology = (int)b.Topology,
            Modifiers = b.Hotkey.Modifiers,
            Vk = b.Hotkey.Vk,
        }).ToArray();
        int hr, conflict;
        fixed (NativeMethods.Binding* pointer = native)
        {
            hr = NativeMethods.ReplaceBindings(pointer, native.Length, out conflict);
        }
        if (hr < 0) throw new CoreException(hr, "保存できませんでした");
        return conflict >= 0 ? (Topology)conflict : null;
    }

    public static MascotSettings LoadMascotSettings()
    {
        NativeMethods.MascotSettings native;
        var hr = NativeMethods.LoadMascotSettings(&native);
        if (hr < 0) throw new CoreException(hr, "マスコットの設定を読み込めませんでした");
        return new MascotSettings(
            native.Enabled != 0,
            native.ShowOnDesktop != 0,
            native.PositionX,
            native.PositionY,
            native.ScalePercent,
            native.YawDegrees,
            native.PitchDegrees,
            native.MotionIntervalSeconds,
            native.FadeMilliseconds,
            native.IdleFadeMilliseconds,
            native.ShowShortcuts != 0,
            native.KeepResident != 0);
    }

    public static void SaveMascotSettings(MascotSettings settings)
    {
        var native = new NativeMethods.MascotSettings
        {
            Enabled = settings.Enabled ? 1 : 0,
            ShowOnDesktop = settings.ShowOnDesktop ? 1 : 0,
            PositionX = settings.PositionX,
            PositionY = settings.PositionY,
            ScalePercent = settings.ScalePercent,
            YawDegrees = settings.YawDegrees,
            PitchDegrees = settings.PitchDegrees,
            MotionIntervalSeconds = settings.MotionIntervalSeconds,
            FadeMilliseconds = settings.FadeMilliseconds,
            IdleFadeMilliseconds = settings.IdleFadeMilliseconds,
            ShowShortcuts = settings.ShowShortcuts ? 1 : 0,
            KeepResident = settings.KeepResident ? 1 : 0,
        };
        var hr = NativeMethods.SaveMascotSettings(&native);
        if (hr < 0) throw new CoreException(hr, "マスコットの設定を保存できませんでした");
    }

    public static string MascotModelPath => ReadString(NativeMethods.GetMascotModelPath);

    public static string MascotMotionDirectory => ReadString(NativeMethods.GetMascotMotionDirectory);

    public static string MascotIdleMotionPath => ReadString(NativeMethods.GetMascotIdleMotionPath);

    // 取り込んだ元のファイルの置き場所と、変換した形式の版
    public static string MascotSourceDirectory => ReadString(NativeMethods.GetMascotSourceDirectory);

    public static int MascotAssetVersion => NativeMethods.GetMascotAssetVersion();

    // モデルやモーションをコピーする前に呼ぶ（置き場所を管理者と SYSTEM だけが書ける状態にし、モーションのフォルダを作る）
    public static void PrepareMascotDirectory()
    {
        var hr = NativeMethods.PrepareMascotDirectory();
        if (hr < 0) throw new CoreException(hr, "マスコットの置き場所を用意できませんでした");
    }

    private delegate int StringReader(char* buffer, int capacity);

    // 2 段階: 1 回目は大きさだけ聞き（バッファなし）、2 回目にその大きさで取る
    private static string ReadString(StringReader read)
    {
        var needed = read(null, 0);
        if (needed <= 1) return string.Empty;
        var buffer = new char[needed];
        fixed (char* pointer = buffer)
        {
            read(pointer, buffer.Length);
        }
        return new string(buffer, 0, needed - 1);   // 終端の 0 は除く
    }

    private delegate int BindingsReader(NativeMethods.Binding* bindings, int capacity, out int count);

    private static IReadOnlyList<Binding> ReadBindings(BindingsReader read, string what)
    {
        var native = new NativeMethods.Binding[MaxBindings];
        int hr, count;
        fixed (NativeMethods.Binding* pointer = native)
        {
            hr = read(pointer, native.Length, out count);
        }
        if (hr < 0) throw new CoreException(hr, what);
        return native.Take(count)
            .Select(b => new Binding((Topology)b.Topology, new Hotkey(b.Modifiers, b.Vk)))
            .ToList();
    }
}
