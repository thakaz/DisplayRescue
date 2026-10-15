// 起動。窓を出す前に Interop の DLL を読めるか確かめる（読めないと、どのページも C++ を呼べずに落ちるため）
using System.Runtime.InteropServices;
using System.Windows;
using DisplayRescue.Settings.Interop;
using DisplayRescue.Settings.Services;

namespace DisplayRescue.Settings;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // Interop の DLL は exe の隣ではなく bin\ にあるので、名前で探させず、場所を決めて読む。
        // P/Invoke（NativeMethods）を初めて呼ぶ前に決めておく（読めなければ、呼んだところで DllNotFoundException）
        NativeLibrary.SetDllImportResolver(typeof(App).Assembly, (name, _, _) =>
            name == NativeMethods.Dll ? NativeLibrary.Load(AppFiles.InteropDll) : IntPtr.Zero);
        try
        {
            _ = Core.Version;
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException)
        {
            MessageBox.Show($"DisplayRescueInterop.dllを読めません({ex.GetType().Name})。zipを展開したフォルダから、ほかのファイルと一緒に開いてください。",
                "DisplayRescueの設定", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
            return;
        }
        new MainWindow().Show();
    }
}
