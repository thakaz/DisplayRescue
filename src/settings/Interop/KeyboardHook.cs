// 低レベルのキーボードフック（WH_KEYBOARD_LL）。キーの入力欄のために使う:
// 入力欄で割り当て済みのキーを押すと、ホットキー監視プロセス（RegisterHotKey）が先に処理して画面が切り替わり、入力欄には届かない。
// RegisterHotKey の判定はこのフックの後なので、ここで入力を破棄すればホットキー監視プロセスは反応しない。
// 握りつぶすかどうかは呼ぶ側（shouldSwallow）が決める。フックはプロセスと一緒に必ず外れる（待ち受けが止まったままにはならない）
using System.Runtime.InteropServices;

namespace DisplayRescue.Settings.Interop;

public sealed partial class KeyboardHook : IDisposable
{
    private const int WhKeyboardLl = 13;
    private const int HcAction = 0;
    private const nint WmKeyDown = 0x0100;
    private const nint WmSysKeyDown = 0x0104;

    private delegate nint HookProc(int code, nint wParam, nint lParam);

    // フックの関数。GC に回収されないよう、フックを外すまでフィールドで持っておく（回収されると Windows が消えた関数を呼んで落ちる）
    private readonly HookProc _proc;
    private readonly Func<uint, bool> _shouldSwallow;
    private nint _hook;

    // shouldSwallow: 押された仮想キーを受け取り、握りつぶすなら true。フックを付けたスレッド（UI）で呼ばれる。手早く済ませること
    // （遅いと Windows にフックを外される）
    public KeyboardHook(Func<uint, bool> shouldSwallow)
    {
        _shouldSwallow = shouldSwallow;
        _proc = Callback;
        // 低レベルのフックは DLL を要らないが、モジュールのハンドルは渡す決まり（null = この exe）
        var module = GetModuleHandleW(null);
        _hook = SetWindowsHookExW(WhKeyboardLl, Marshal.GetFunctionPointerForDelegate(_proc), module, 0);
        // 付けられなくても設定はできる（割り当て済みのキーを入力欄で押すと画面が切り替わるだけ）ので、失敗は無視する
    }

    public void Dispose()
    {
        if (_hook != 0)
        {
            UnhookWindowsHookEx(_hook);
            _hook = 0;
        }
    }

    private nint Callback(int code, nint wParam, nint lParam)
    {
        if (code == HcAction && (wParam == WmKeyDown || wParam == WmSysKeyDown))
        {
            var vk = (uint)Marshal.ReadInt32(lParam);  // KBDLLHOOKSTRUCT の最初が vkCode
            if (_shouldSwallow(vk)) return 1;            // 0 以外 = このキーはここで終わり
        }
        return CallNextHookEx(0, code, wParam, lParam);
    }

    // 今押されている修飾キー（MOD_*）。フックの中では WPF の Keyboard.Modifiers がまだ更新されていないので、OS に聞く
    public static uint CurrentModifiers()
    {
        uint modifiers = 0;
        if ((GetAsyncKeyState(0x11) & 0x8000) != 0) modifiers |= Hotkey.ModControl;  // VK_CONTROL
        if ((GetAsyncKeyState(0x12) & 0x8000) != 0) modifiers |= Hotkey.ModAlt;      // VK_MENU
        if ((GetAsyncKeyState(0x10) & 0x8000) != 0) modifiers |= Hotkey.ModShift;    // VK_SHIFT
        return modifiers;
    }

    [LibraryImport("user32.dll", SetLastError = true)]
    private static partial nint SetWindowsHookExW(int idHook, nint lpfn, nint hmod, uint dwThreadId);

    [LibraryImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool UnhookWindowsHookEx(nint hhk);

    [LibraryImport("user32.dll")]
    private static partial nint CallNextHookEx(nint hhk, int nCode, nint wParam, nint lParam);

    [LibraryImport("user32.dll")]
    private static partial short GetAsyncKeyState(int vKey);

    [LibraryImport("kernel32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial nint GetModuleHandleW(string? lpModuleName);
}
