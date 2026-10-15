// キーの組み合わせを押して入れる入力欄（Win32 の msctls_hotkey32 の代わり）。
// 押したキーをそのまま出す（勝手に Ctrl+Alt を足さない）。修飾キーだけを押している間は「Ctrl+Alt+」と途中を出す。
// Tab / Shift+Tab は移動に使う、Backspace だけを押すと空（割り当てなし）。それ以外のキーは全部ここで受け止める
// （Alt+F4 や F10 も、窓を閉じたりメニューに行ったりせず、組み合わせとして入る）
using System.Windows.Controls;
using System.Windows.Input;
using DisplayRescue.Settings.Interop;

namespace DisplayRescue.Settings.Controls;

public sealed class HotkeyBox : TextBox
{
    private Hotkey _hotkey;

    public HotkeyBox()
    {
        // TextBox の見た目（Fluent テーマ）を使う。テーマのスタイルは「型がちょうど TextBox」のものにしか自動で当たらないので、
        // 派生したこのクラスには明示的に付ける（付けないと、ダークでも白い古い見た目になる）
        SetResourceReference(StyleProperty, typeof(TextBox));
        IsReadOnly = true;
        IsReadOnlyCaretVisible = false;
        IsUndoEnabled = false;
        ContextMenu = null;
        Cursor = Cursors.Arrow;
        UpdateText();
    }

    // 値が変わったとき（キーを押した・Backspace・外部から代入）
    public event EventHandler? HotkeyChanged;

    public Hotkey Hotkey
    {
        get => _hotkey;
        set
        {
            if (_hotkey == value) return;
            _hotkey = value;
            UpdateText();
            HotkeyChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    protected override void OnPreviewKeyDown(KeyEventArgs e)
    {
        // Alt を含むと Key は System になり、本当のキーは SystemKey に入る
        var key = e.Key == Key.System ? e.SystemKey : e.Key;
        var modifiers = ToMod(Keyboard.Modifiers);

        if (modifiers == 0 && key == Key.Tab)
        {
            base.OnPreviewKeyDown(e);   // フォーカスの移動は TextBox に任せる
            return;
        }
        e.Handled = true;
        if (modifiers == 0 && key == Key.Back)
        {
            Hotkey = default;
            return;
        }

        var vk = (uint)KeyInterop.VirtualKeyFromKey(key);
        if (vk == 0 || Core.IsModifierKey(vk))
        {
            // 修飾キーだけ: 途中経過を出す（決まるのは修飾キー以外が押されたとき）
            // 並びは C++ の FormatHotkey と同じ Ctrl → Alt → Shift（キーが無いと FormatHotkey は使えないので、ここで組み立てる）
            Text = modifiers == 0 ? Describe(_hotkey)
                : ((modifiers & Hotkey.ModControl) != 0 ? "Ctrl+" : "")
                + ((modifiers & Hotkey.ModAlt) != 0 ? "Alt+" : "")
                + ((modifiers & Hotkey.ModShift) != 0 ? "Shift+" : "");
            return;
        }
        Hotkey = new Hotkey(modifiers, vk);
        UpdateText();   // 同じ値でも、途中経過の表示を戻す
    }

    protected override void OnPreviewKeyUp(KeyEventArgs e)
    {
        // 修飾キーを離したら、途中経過をやめて今の値に戻す
        if (Keyboard.Modifiers == ModifierKeys.None) UpdateText();
        base.OnPreviewKeyUp(e);
    }

    protected override void OnLostKeyboardFocus(KeyboardFocusChangedEventArgs e)
    {
        UpdateText();
        base.OnLostKeyboardFocus(e);
    }

    private void UpdateText() => Text = Describe(_hotkey);

    private static string Describe(Hotkey hotkey) => hotkey.IsEmpty ? "なし" : Core.Format(hotkey);

    private static uint ToMod(ModifierKeys keys)
    {
        uint result = 0;
        if (keys.HasFlag(ModifierKeys.Control)) result |= Hotkey.ModControl;
        if (keys.HasFlag(ModifierKeys.Alt)) result |= Hotkey.ModAlt;
        if (keys.HasFlag(ModifierKeys.Shift)) result |= Hotkey.ModShift;
        return result;
    }
}
