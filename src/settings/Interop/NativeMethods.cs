// DisplayRescueInterop.dll（C++）の関数を C# から呼ぶ宣言。C++ 側は src/interop/Interop.h（並びと型をそろえる）。
// LibraryImport は、C# からネイティブ DLL を呼び出すコードをコンパイル時に生成する。partial で宣言だけ記述する。
// ここは C++ の形（ポインタ・HRESULT・2 段階のバッファ）のまま。C# らしい形に包むのは Core.cs
using System.Runtime.InteropServices;

namespace DisplayRescue.Settings.Interop;

internal static unsafe partial class NativeMethods
{
    // 読む場所は App.OnStartup の SetDllImportResolver で決める（配る zip では bin\、手元のビルドでは exe と同じフォルダ。Services/AppFiles.cs）
    internal const string Dll = "DisplayRescueInterop.dll";

    // C++ の DR_Binding と同じ並び・大きさ（int・uint・uint）。中身が数だけなので、そのままメモリを渡せる（blittable）
    [StructLayout(LayoutKind.Sequential)]
    internal struct Binding
    {
        public int Topology;
        public uint Modifiers;
        public uint Vk;
    }

    // C# の out int は、C++ では int32_t* になる（書き込み先のアドレスを渡す）
    [LibraryImport(Dll, EntryPoint = "DisplayRescue_GetVersion")]
    internal static partial void GetVersion(out int major, out int minor, out int patch);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_GetInstalledVersion")]
    internal static partial int GetInstalledVersion(out int major, out int minor, out int patch);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_GetInstallDirectory")]
    internal static partial int GetInstallDirectory(char* buffer, int capacity);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_QueryServiceState")]
    internal static partial int QueryServiceState();

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_RequestServiceReload")]
    internal static partial int RequestServiceReload();

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_IsValidHotkey")]
    internal static partial int IsValidHotkey(uint modifiers, uint vk);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_StealsNormalInput")]
    internal static partial int StealsNormalInput(uint modifiers, uint vk);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_IsModifierKey")]
    internal static partial int IsModifierKey(uint vk);

    // 文字列は呼ぶ側のバッファ（char* = UTF-16）に書いてもらう。戻り値は終端を含めた必要な文字数
    [LibraryImport(Dll, EntryPoint = "DisplayRescue_FormatHotkey")]
    internal static partial int FormatHotkey(uint modifiers, uint vk, char* buffer, int capacity);

    // 配列は「先頭のアドレス + 個数」。戻り値は HRESULT
    [LibraryImport(Dll, EntryPoint = "DisplayRescue_LoadBindings")]
    internal static partial int LoadBindings(Binding* bindings, int capacity, out int count);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_DefaultBindings")]
    internal static partial int DefaultBindings(Binding* bindings, int capacity, out int count);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_ReplaceBindings")]
    internal static partial int ReplaceBindings(Binding* bindings, int count, out int conflict);

    // C++ の DR_MascotSettings と同じ並び・大きさ。中身が数だけなので blittable
    [StructLayout(LayoutKind.Sequential)]
    internal struct MascotSettings
    {
        public int Enabled;
        public int ShowOnDesktop;
        public int PositionX;
        public int PositionY;
        public int ScalePercent;
        public int YawDegrees;
        public int PitchDegrees;
        public int MotionIntervalSeconds;
        public int FadeMilliseconds;
        public int IdleFadeMilliseconds;
        public int ShowShortcuts;
        public int KeepResident;
    }

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_LoadMascotSettings")]
    internal static partial int LoadMascotSettings(MascotSettings* settings);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_SaveMascotSettings")]
    internal static partial int SaveMascotSettings(MascotSettings* settings);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_GetMascotModelPath")]
    internal static partial int GetMascotModelPath(char* buffer, int capacity);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_GetMascotMotionDirectory")]
    internal static partial int GetMascotMotionDirectory(char* buffer, int capacity);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_GetMascotIdleMotionPath")]
    internal static partial int GetMascotIdleMotionPath(char* buffer, int capacity);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_GetMascotSourceDirectory")]
    internal static partial int GetMascotSourceDirectory(char* buffer, int capacity);

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_GetMascotAssetVersion")]
    internal static partial int GetMascotAssetVersion();

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_PrepareMascotDirectory")]
    internal static partial int PrepareMascotDirectory();

    [LibraryImport(Dll, EntryPoint = "DisplayRescue_DescribeHresult")]
    internal static partial int DescribeHresult(int hr, char* buffer, int capacity);
}
