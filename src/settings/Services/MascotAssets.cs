// モデルとモーションの取り込み。
// ログイン画面の SYSTEM のマスコットに他人の作ったファイル（glTF・JSON・画像）を解析させないため、取り込むときに
// bin\ の DisplayRescueMascot.exe --convert-model / --convert-motion で、マスコットが読む形式（mascot/AssetFile.h）に変換する。
//   1. 変換は一時フォルダに書かせる（変換に失敗したら、入っているものは何も変わらない）
//   2. できたものを ProgramData の置き場所に「コピー」してから置き換える（移動だと一時フォルダの ACL を持ち込むので、コピーでフォルダの ACL を引き継がせる）
//   3. 元のファイル（.vrm / .vrma）は mascot-sources に、選んだときの名前のまま取っておく（設定アプリが名前を出すため）。
//      モデルと待機モーションは 1 つだけなので、それぞれ model\・idle\ のフォルダに 1 つ置く。形式の版が変わったら、ここから変換し直す（UpgradeAsync）
// 起動のしかたは ServiceSetup（CLI を起動して終わりを待つ）と同じ
using System.Diagnostics;
using System.IO;
using DisplayRescue.Settings.Interop;

namespace DisplayRescue.Settings.Services;

public static class MascotAssets
{
    private static string MascotExe => AppFiles.MascotExe;
    private static string SourceMotions => Path.Combine(Core.MascotSourceDirectory, "motions");
    private static string SourceModelDirectory => Path.Combine(Core.MascotSourceDirectory, "model");
    private static string SourceIdleDirectory => Path.Combine(Core.MascotSourceDirectory, "idle");

    // 取っておいた元のモデル（mascot-sources\model\ の .vrm か .glb）・待機モーション（idle\ の .vrma）。無ければ null
    public static string? SourceModel => FirstFile(SourceModelDirectory, "*.vrm", "*.glb");
    public static string? SourceIdle => FirstFile(SourceIdleDirectory, "*.vrma");

    private static string? FirstFile(string folder, params string[] patterns) => !Directory.Exists(folder) ? null
        : patterns.SelectMany(pattern => Directory.EnumerateFiles(folder, pattern)).Order(StringComparer.OrdinalIgnoreCase).FirstOrDefault();

    public static async Task ImportModelAsync(string file)
    {
        Core.PrepareMascotDirectory();
        await ConvertAsync("--convert-model", file, Core.MascotModelPath);
        KeepSource(file, SourceModelDirectory);
    }

    public static async Task ImportIdleAsync(string file)
    {
        Core.PrepareMascotDirectory();
        await ConvertAsync("--convert-motion", file, Core.MascotIdleMotionPath);
        KeepSource(file, SourceIdleDirectory);
    }

    public static void RemoveIdle()
    {
        File.Delete(Core.MascotIdleMotionPath);   // 無くても例外にならない
        if (Directory.Exists(SourceIdleDirectory)) Directory.Delete(SourceIdleDirectory, recursive: true);
    }

    // 1 つだけ置くフォルダ（model\・idle\）を空にしてから、選んだファイルを名前のままコピーする
    private static void KeepSource(string file, string folder)
    {
        if (Directory.Exists(folder)) Directory.Delete(folder, recursive: true);
        Directory.CreateDirectory(folder);
        File.Copy(file, Path.Combine(folder, Path.GetFileName(file)));
    }

    public static async Task ImportMotionAsync(string file)
    {
        Core.PrepareMascotDirectory();
        var name = Path.GetFileNameWithoutExtension(file);
        await ConvertAsync("--convert-motion", file, Path.Combine(Core.MascotMotionDirectory, name + ".motion"));
        Directory.CreateDirectory(SourceMotions);   // 初めて足すときは無い
        File.Copy(file, Path.Combine(SourceMotions, name + ".vrma"), overwrite: true);
    }

    // zip に入れた既定のモデルとモーション（bin\mascot\。AppFiles）
    private static string BundledDirectory => AppFiles.BundledMascot;

    // マスコットのモデル・待機モーション・モーションがどれもまだ入っていなければ、同梱のものを取り込む。
    // インストールのあとに呼ぶ。利用者が自分のものを入れていれば何もしない（1 つでも入っていれば触らない）。
    // 戻り値は取り込んだ数（同梱のものが無い・何かが入っていて取り込まなかったときは 0）
    public static async Task<int> ImportBundledAsync()
    {
        if (!Directory.Exists(BundledDirectory)) return 0;
        var empty = SourceModel is null && !File.Exists(Core.MascotModelPath)
            && SourceIdle is null && !File.Exists(Core.MascotIdleMotionPath)
            && ListMotions().Count == 0
            && !(Directory.Exists(SourceMotions) && Directory.EnumerateFiles(SourceMotions, "*.vrma", SearchOption.AllDirectories).Any());
        if (!empty) return 0;

        var count = 0;
        if (FirstFile(Path.Combine(BundledDirectory, "model"), "*.vrm", "*.glb") is { } model)
        {
            await ImportModelAsync(model);
            count++;
        }
        if (FirstFile(Path.Combine(BundledDirectory, "idle"), "*.vrma") is { } idle)
        {
            await ImportIdleAsync(idle);
            count++;
        }
        var motions = Path.Combine(BundledDirectory, "motions");
        if (Directory.Exists(motions))
        {
            foreach (var motion in Directory.EnumerateFiles(motions, "*.vrma").Order(StringComparer.OrdinalIgnoreCase))
            {
                await ImportMotionAsync(motion);
                count++;
            }
        }
        return count;
    }

    // relativeName: モーションのフォルダからの相対パス（拡張子なし。ListMotions の名前）
    public static void RemoveMotion(string relativeName)
    {
        File.Delete(Path.Combine(Core.MascotMotionDirectory, relativeName + ".motion"));
        File.Delete(Path.Combine(SourceMotions, relativeName + ".vrma"));
    }

    // 変換したモーションの一覧（モーションのフォルダからの相対パス、拡張子なし。サブフォルダも）。マスコットも同じように探す
    public static List<string> ListMotions()
    {
        var folder = Core.MascotMotionDirectory;
        if (!Directory.Exists(folder)) return [];
        return Directory.EnumerateFiles(folder, "*.motion", SearchOption.AllDirectories)
            .Select(path => Path.ChangeExtension(Path.GetRelativePath(folder, path), null))
            .Order(StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    // 変換し直し。ページを開いたときに呼ぶ。
    // 変換したファイルが無い・形式の版が違う → mascot-sources の元のファイルから変換し直す
    // 戻り値は変換した数（何もしなければ 0）
    public static async Task<int> UpgradeAsync()
    {
        var work = NeedsConversion(SourceModel, Core.MascotModelPath)
            || NeedsConversion(SourceIdle, Core.MascotIdleMotionPath)
            || SourceMotionPairs().Any(pair => NeedsConversion(pair.Source, pair.Converted));
        if (!work) return 0;

        Core.PrepareMascotDirectory();
        var converted = 0;
        if (NeedsConversion(SourceModel, Core.MascotModelPath))
        {
            await ConvertAsync("--convert-model", SourceModel!, Core.MascotModelPath);
            converted++;
        }
        if (NeedsConversion(SourceIdle, Core.MascotIdleMotionPath))
        {
            await ConvertAsync("--convert-motion", SourceIdle!, Core.MascotIdleMotionPath);
            converted++;
        }
        foreach (var (source, destination) in SourceMotionPairs())
        {
            if (!NeedsConversion(source, destination)) continue;
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            await ConvertAsync("--convert-motion", source, destination);
            converted++;
        }
        return converted;
    }

    // 取っておいた元のモーションと、その変換先
    private static IEnumerable<(string Source, string Converted)> SourceMotionPairs()
    {
        if (!Directory.Exists(SourceMotions)) yield break;
        foreach (var source in Directory.EnumerateFiles(SourceMotions, "*.vrma", SearchOption.AllDirectories))
        {
            var relative = Path.ChangeExtension(Path.GetRelativePath(SourceMotions, source), ".motion");
            yield return (source, Path.Combine(Core.MascotMotionDirectory, relative));
        }
    }

    // 元があって、変換したものが無いか版が違う
    private static bool NeedsConversion(string? source, string converted) => source is not null && !IsCurrent(converted);

    // 先頭 12 バイト（印 8 バイト + 版 4 バイト）の版が、今の形式の版と同じか
    private static bool IsCurrent(string path)
    {
        try
        {
            using var stream = File.OpenRead(path);
            Span<byte> header = stackalloc byte[12];
            if (stream.Read(header) != 12) return false;
            return BitConverter.ToInt32(header[8..]) == Core.MascotAssetVersion;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    // 変換して destination に置く。失敗は例外（メッセージはエラーの名前）
    private static async Task ConvertAsync(string kind, string input, string destination)
    {
        if (!File.Exists(MascotExe)) throw new FileNotFoundException("DisplayRescueMascot.exeが見つかりません。zipを展開したフォルダから開いてください。", MascotExe);
        var temporary = Path.Combine(Path.GetTempPath(), $"DisplayRescue-{Guid.NewGuid():N}.asset");
        try
        {
            var start = new ProcessStartInfo(MascotExe) { UseShellExecute = false, CreateNoWindow = true };
            start.ArgumentList.Add(kind);
            start.ArgumentList.Add(input);
            start.ArgumentList.Add(temporary);
            using var process = Process.Start(start)!;
            await process.WaitForExitAsync();
            if (process.ExitCode != 0)
            {
                // 終了コードは Win32 のエラーコード（ExitCodeFromHresult）。HRESULT に戻して名前にする
                var code = process.ExitCode;
                var hr = code is > 0 and < 0x10000 ? unchecked((int)(0x80070000u | (uint)code)) : code;
                throw new IOException($"{Path.GetFileName(input)}を変換できませんでした: {Core.DescribeHresult(hr)}");
            }
            // コピーして（フォルダの ACL を引き継ぐ）、同じフォルダの中で置き換える
            var staging = destination + ".new";
            File.Copy(temporary, staging, overwrite: true);
            File.Move(staging, destination, overwrite: true);
        }
        finally
        {
            File.Delete(temporary);
            File.Delete(temporary + ".tmp");
        }
    }
}
