// インストール・アンインストール。自分では行わず、bin\ の DisplayRescueService.exe install / uninstall を起動し（AppFiles）、
// 出力を 1 行ずつ受け取る（PowerToys がインストールを別のプログラムに任せるのと同じ考え）。
// 設定アプリは管理者で動いているので、起動した CLI も管理者のまま動く（権限は親から子へ引き継がれる）。
// CLI は「自分と同じフォルダ（bin\）の DisplayRescueService.exe と DisplayRescueMascot.exe」を Program Files にコピーするので、展開したフォルダのものが入る
using System.Diagnostics;
using System.IO;
using System.Text;

namespace DisplayRescue.Settings.Services;

public static class ServiceSetup
{
    public static string CliPath => AppFiles.ServiceExe;

    // command: "install" か "uninstall"。onLine は CLI の出力 1 行ごとに（UI のスレッドで）呼ばれる。戻り値は終了コード（0 が成功）
    public static async Task<int> RunAsync(string command, Action<string> onLine)
    {
        if (!File.Exists(CliPath)) throw new FileNotFoundException("DisplayRescueService.exeが見つかりません。zipを展開したフォルダから開いてください。", CliPath);

        var start = new ProcessStartInfo(CliPath, command)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            // CLI は標準出力を UTF-8 で書く（main.cpp の _O_U8TEXT）
            StandardOutputEncoding = Encoding.UTF8,
            StandardErrorEncoding = Encoding.UTF8,
            WorkingDirectory = AppFiles.Folder,
        };
        using var process = new Process { StartInfo = start };
        process.Start();

        // 出力は別のスレッドで読み、1 行ずつ呼ぶ側へ。await の後は呼んだ側（UI）のスレッドに戻るので、onLine は UI を触ってよい
        async Task Pump(StreamReader reader)
        {
            while (await reader.ReadLineAsync() is { } line)
            {
                if (line.Length > 0) onLine(line);
            }
        }
        await Task.WhenAll(Pump(process.StandardOutput), Pump(process.StandardError));
        await process.WaitForExitAsync();
        return process.ExitCode;
    }
}
