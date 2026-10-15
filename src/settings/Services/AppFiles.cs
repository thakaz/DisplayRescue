// 設定アプリと一緒に配るファイルの場所。
// 配る zip では、利用者が開くものを DisplayRescue.exe 1 つにするため、ほかのもの（サービス・マスコット・Interop の DLL・
// 同梱のマスコット）は隣の bin\ に置く。手元のビルドは x64\<構成>\ に全部並ぶので、bin\ が無ければ exe と同じフォルダを見る
using System.IO;

namespace DisplayRescue.Settings.Services;

public static class AppFiles
{
    // bin\ か、exe と同じフォルダ
    public static string Folder { get; } = FindFolder();

    // インストールでは、CLI が「自分と同じフォルダ」のサービス本体とマスコットを Program Files にコピーする
    public static string ServiceExe => Path.Combine(Folder, "DisplayRescueService.exe");
    public static string MascotExe => Path.Combine(Folder, "DisplayRescueMascot.exe");
    public static string InteropDll => Path.Combine(Folder, "DisplayRescueInterop.dll");

    // 同梱のマスコット（assets/mascot と同じ分け方: model\・idle\・motions\）
    public static string BundledMascot => Path.Combine(Folder, "mascot");

    private static string FindFolder()
    {
        var bin = Path.Combine(AppContext.BaseDirectory, "bin");
        return Directory.Exists(bin) ? bin : AppContext.BaseDirectory;
    }
}
