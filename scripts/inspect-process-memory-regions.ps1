[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [int]$ProcessId,
    [int]$Top = 40,
    [switch]$SamplePrivateData,
    [double]$SampleMinimumPrivateWorkingSetMiB = 0,
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

if (-not ("SnowShot.MemoryProbe" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace SnowShot {
    public static class MemoryProbe {
        const uint PROCESS_QUERY_INFORMATION = 0x0400;
        const uint PROCESS_VM_READ = 0x0010;
        const uint THREAD_QUERY_INFORMATION = 0x0040;
        const uint MEM_COMMIT = 0x1000;
        const uint MEM_PRIVATE = 0x20000;
        const uint MEM_MAPPED = 0x40000;
        const uint MEM_IMAGE = 0x1000000;
        const uint TH32CS_SNAPHEAPLIST = 0x00000001;
        static readonly IntPtr INVALID_HANDLE_VALUE = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential)]
        struct MEMORY_BASIC_INFORMATION {
            public IntPtr BaseAddress;
            public IntPtr AllocationBase;
            public uint AllocationProtect;
            public ushort PartitionId;
            public UIntPtr RegionSize;
            public uint State;
            public uint Protect;
            public uint Type;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct PSAPI_WORKING_SET_EX_INFORMATION {
            public IntPtr VirtualAddress;
            public UIntPtr VirtualAttributes;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct HEAPLIST32 {
            public UIntPtr dwSize;
            public uint th32ProcessID;
            public UIntPtr th32HeapID;
            public uint dwFlags;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct PROCESS_MEMORY_COUNTERS_EX {
            public uint cb;
            public uint PageFaultCount;
            public UIntPtr PeakWorkingSetSize;
            public UIntPtr WorkingSetSize;
            public UIntPtr QuotaPeakPagedPoolUsage;
            public UIntPtr QuotaPagedPoolUsage;
            public UIntPtr QuotaPeakNonPagedPoolUsage;
            public UIntPtr QuotaNonPagedPoolUsage;
            public UIntPtr PagefileUsage;
            public UIntPtr PeakPagefileUsage;
            public UIntPtr PrivateUsage;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct CLIENT_ID {
            public IntPtr UniqueProcess;
            public IntPtr UniqueThread;
        }

        [StructLayout(LayoutKind.Sequential)]
        struct THREAD_BASIC_INFORMATION {
            public int ExitStatus;
            public IntPtr TebBaseAddress;
            public CLIENT_ID ClientId;
            public UIntPtr AffinityMask;
            public int Priority;
            public int BasePriority;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int processId);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenThread(uint access, bool inherit, uint threadId);
        [DllImport("ntdll.dll")]
        static extern int NtQueryInformationThread(IntPtr thread, int infoClass,
                                                    out THREAD_BASIC_INFORMATION info,
                                                    int length, IntPtr returnLength);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
                                             [Out] byte[] buffer, UIntPtr size,
                                             out UIntPtr bytesRead);
        [DllImport("dbghelp.dll", SetLastError = true)]
        static extern bool MiniDumpWriteDump(IntPtr process, uint processId,
                                             IntPtr fileHandle, uint dumpType,
                                             IntPtr exceptionParam,
                                             IntPtr userStreamParam,
                                             IntPtr callbackParam);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern UIntPtr VirtualQueryEx(IntPtr process, IntPtr address,
                                              out MEMORY_BASIC_INFORMATION info,
                                              UIntPtr length);
        [DllImport("psapi.dll", CharSet = CharSet.Unicode)]
        static extern uint GetMappedFileName(IntPtr process, IntPtr address,
                                             StringBuilder fileName, uint size);
        [DllImport("psapi.dll", SetLastError = true)]
        static extern bool QueryWorkingSetEx(IntPtr process,
                                             [In, Out] PSAPI_WORKING_SET_EX_INFORMATION[] info,
                                             uint size);
        [DllImport("psapi.dll", SetLastError = true)]
        static extern bool GetProcessMemoryInfo(IntPtr process,
                                                out PROCESS_MEMORY_COUNTERS_EX counters,
                                                uint size);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr CreateToolhelp32Snapshot(uint flags, uint processId);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool Heap32ListFirst(IntPtr snapshot, ref HEAPLIST32 entry);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool Heap32ListNext(IntPtr snapshot, ref HEAPLIST32 entry);

        public sealed class Region {
            public ulong BaseAddress { get; set; }
            public ulong AllocationBase { get; set; }
            public ulong Size { get; set; }
            public ulong WorkingSet { get; set; }
            public ulong PrivateWorkingSet { get; set; }
            public uint Protection { get; set; }
            public string Type { get; set; }
            public string Details { get; set; }
            public bool HeapBase { get; set; }
        }

        public sealed class Summary {
            public ulong PrivateBytes { get; set; }
            public ulong WorkingSet { get; set; }
            public ulong PeakWorkingSet { get; set; }
            public int HeapCount { get; set; }
            public ThreadStack[] ThreadStacks { get; set; }
            public Region[] Regions { get; set; }
        }

        public sealed class ThreadStack {
            public int ThreadId { get; set; }
            public ulong TebAddress { get; set; }
            public ulong StackBase { get; set; }
            public ulong StackLimit { get; set; }
            public ulong AllocationBase { get; set; }
        }

        public sealed class DataStats {
            public int BytesRead { get; set; }
            public double ZeroPercent { get; set; }
            public double FFPercent { get; set; }
            public double EntropyBits { get; set; }
            public double[] OffsetZeroPercent { get; set; }
            public double[] OffsetFFPercent { get; set; }
            public string FirstBytes { get; set; }
        }

        static ulong Value(UIntPtr value) { return value.ToUInt64(); }
        static ulong Value(IntPtr value) { return unchecked((ulong)value.ToInt64()); }

        static string TypeName(uint type) {
            if (type == MEM_PRIVATE) return "Private";
            if (type == MEM_MAPPED) return "Mapped";
            if (type == MEM_IMAGE) return "Image";
            return "Unknown";
        }

        static HashSet<ulong> HeapBases(int processId) {
            var result = new HashSet<ulong>();
            IntPtr snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPHEAPLIST,
                                                       unchecked((uint)processId));
            if (snapshot == INVALID_HANDLE_VALUE) return result;
            try {
                var entry = new HEAPLIST32();
                entry.dwSize = (UIntPtr)Marshal.SizeOf<HEAPLIST32>();
                if (!Heap32ListFirst(snapshot, ref entry)) return result;
                do {
                    result.Add(Value(entry.th32HeapID));
                    entry.dwSize = (UIntPtr)Marshal.SizeOf<HEAPLIST32>();
                } while (Heap32ListNext(snapshot, ref entry));
            } finally {
                CloseHandle(snapshot);
            }
            return result;
        }

        static void WorkingSet(IntPtr process, Region region) {
            const ulong pageSize = 4096;
            int pageCount = checked((int)((region.Size + pageSize - 1) / pageSize));
            if (pageCount == 0) return;
            var pages = new PSAPI_WORKING_SET_EX_INFORMATION[pageCount];
            for (int i = 0; i < pageCount; ++i) {
                pages[i].VirtualAddress = new IntPtr(
                    unchecked((long)(region.BaseAddress + (ulong)i * pageSize)));
            }
            uint bytes = checked((uint)(Marshal.SizeOf<PSAPI_WORKING_SET_EX_INFORMATION>() *
                                        pageCount));
            if (!QueryWorkingSetEx(process, pages, bytes)) return;
            foreach (var page in pages) {
                ulong attributes = page.VirtualAttributes.ToUInt64();
                if ((attributes & 1UL) == 0) continue;
                region.WorkingSet += pageSize;
                bool shared = (attributes & (1UL << 15)) != 0;
                if (!shared) region.PrivateWorkingSet += pageSize;
            }
        }

        static ThreadStack[] ThreadStacks(IntPtr process, int processId) {
            var result = new List<ThreadStack>();
            using (var target = Process.GetProcessById(processId)) {
                foreach (ProcessThread processThread in target.Threads) {
                    IntPtr thread = OpenThread(THREAD_QUERY_INFORMATION, false,
                                               unchecked((uint)processThread.Id));
                    if (thread == IntPtr.Zero) continue;
                    try {
                        THREAD_BASIC_INFORMATION basic;
                        int status = NtQueryInformationThread(thread, 0, out basic,
                            Marshal.SizeOf<THREAD_BASIC_INFORMATION>(), IntPtr.Zero);
                        if (status != 0 || basic.TebBaseAddress == IntPtr.Zero) continue;
                        var tib = new byte[24];
                        UIntPtr bytesRead;
                        if (!ReadProcessMemory(process, basic.TebBaseAddress, tib,
                                               (UIntPtr)tib.Length, out bytesRead) ||
                            bytesRead.ToUInt64() != (ulong)tib.Length) continue;
                        ulong stackBase = BitConverter.ToUInt64(tib, 8);
                        ulong stackLimit = BitConverter.ToUInt64(tib, 16);
                        MEMORY_BASIC_INFORMATION memory;
                        UIntPtr queried = VirtualQueryEx(process,
                            new IntPtr(unchecked((long)(stackBase - 1))), out memory,
                            (UIntPtr)Marshal.SizeOf<MEMORY_BASIC_INFORMATION>());
                        result.Add(new ThreadStack {
                            ThreadId = processThread.Id,
                            TebAddress = Value(basic.TebBaseAddress),
                            StackBase = stackBase,
                            StackLimit = stackLimit,
                            AllocationBase = queried == UIntPtr.Zero
                                ? 0 : Value(memory.AllocationBase)
                        });
                    } finally {
                        CloseHandle(thread);
                    }
                }
            }
            return result.ToArray();
        }

        public static DataStats AnalyzeMemory(int processId, ulong address, int requestedBytes) {
            IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                         false, processId);
            if (process == IntPtr.Zero) return null;
            try {
                int count = Math.Max(0, Math.Min(requestedBytes, 1024 * 1024));
                var data = new byte[count];
                UIntPtr bytesRead;
                if (!ReadProcessMemory(process, new IntPtr(unchecked((long)address)), data,
                                       (UIntPtr)data.Length, out bytesRead)) return null;
                int actual = checked((int)bytesRead.ToUInt64());
                if (actual == 0) return null;
                var frequencies = new long[256];
                var offsetCounts = new long[4];
                var offsetZero = new long[4];
                var offsetFF = new long[4];
                for (int i = 0; i < actual; ++i) {
                    byte value = data[i];
                    frequencies[value]++;
                    int offset = i & 3;
                    offsetCounts[offset]++;
                    if (value == 0) offsetZero[offset]++;
                    if (value == 255) offsetFF[offset]++;
                }
                double entropy = 0;
                for (int i = 0; i < frequencies.Length; ++i) {
                    if (frequencies[i] == 0) continue;
                    double probability = (double)frequencies[i] / actual;
                    entropy -= probability * (Math.Log(probability) / Math.Log(2));
                }
                int prefix = Math.Min(32, actual);
                var first = new StringBuilder(prefix * 3);
                for (int i = 0; i < prefix; ++i) {
                    if (i != 0) first.Append(' ');
                    first.Append(data[i].ToString("X2"));
                }
                var zeroPercent = new double[4];
                var ffPercent = new double[4];
                for (int i = 0; i < 4; ++i) {
                    zeroPercent[i] = 100.0 * offsetZero[i] / offsetCounts[i];
                    ffPercent[i] = 100.0 * offsetFF[i] / offsetCounts[i];
                }
                return new DataStats {
                    BytesRead = actual,
                    ZeroPercent = 100.0 * frequencies[0] / actual,
                    FFPercent = 100.0 * frequencies[255] / actual,
                    EntropyBits = entropy,
                    OffsetZeroPercent = zeroPercent,
                    OffsetFFPercent = ffPercent,
                    FirstBytes = first.ToString()
                };
            } finally {
                CloseHandle(process);
            }
        }

        public static void WriteFullDump(int processId, string path) {
            const uint MiniDumpWithFullMemory = 0x00000002;
            IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                         false, processId);
            if (process == IntPtr.Zero) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess failed");
            }
            try {
                using (var stream = new System.IO.FileStream(path,
                    System.IO.FileMode.Create, System.IO.FileAccess.Write,
                    System.IO.FileShare.None)) {
                    if (!MiniDumpWriteDump(process, unchecked((uint)processId),
                                           stream.SafeFileHandle.DangerousGetHandle(),
                                           MiniDumpWithFullMemory, IntPtr.Zero,
                                           IntPtr.Zero, IntPtr.Zero)) {
                        throw new Win32Exception(Marshal.GetLastWin32Error(),
                                                 "MiniDumpWriteDump failed");
                    }
                }
            } finally {
                CloseHandle(process);
            }
        }

        public static Summary Capture(int processId) {
            IntPtr process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                         false, processId);
            if (process == IntPtr.Zero) {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess failed");
            }
            try {
                var counters = new PROCESS_MEMORY_COUNTERS_EX();
                counters.cb = (uint)Marshal.SizeOf<PROCESS_MEMORY_COUNTERS_EX>();
                if (!GetProcessMemoryInfo(process, out counters, counters.cb)) {
                    throw new Win32Exception(Marshal.GetLastWin32Error(),
                                             "GetProcessMemoryInfo failed");
                }
                var heapBases = HeapBases(processId);
                var regions = new List<Region>();
                ulong address = 0;
                int infoSize = Marshal.SizeOf<MEMORY_BASIC_INFORMATION>();
                while (address < 0x0000800000000000UL) {
                    MEMORY_BASIC_INFORMATION info;
                    UIntPtr queried = VirtualQueryEx(process,
                        new IntPtr(unchecked((long)address)), out info, (UIntPtr)infoSize);
                    if (queried == UIntPtr.Zero) break;
                    ulong baseAddress = Value(info.BaseAddress);
                    ulong size = Value(info.RegionSize);
                    if (size == 0 || baseAddress + size <= address) break;
                    if (info.State == MEM_COMMIT) {
                        var name = new StringBuilder(32768);
                        uint chars = GetMappedFileName(process, info.BaseAddress,
                                                       name, (uint)name.Capacity);
                        var region = new Region {
                            BaseAddress = baseAddress,
                            AllocationBase = Value(info.AllocationBase),
                            Size = size,
                            Protection = info.Protect,
                            Type = TypeName(info.Type),
                            Details = chars == 0 ? "" : name.ToString(),
                            HeapBase = heapBases.Contains(baseAddress) ||
                                       heapBases.Contains(Value(info.AllocationBase))
                        };
                        WorkingSet(process, region);
                        regions.Add(region);
                    }
                    address = baseAddress + size;
                }
                return new Summary {
                    PrivateBytes = Value(counters.PrivateUsage),
                    WorkingSet = Value(counters.WorkingSetSize),
                    PeakWorkingSet = Value(counters.PeakWorkingSetSize),
                    HeapCount = heapBases.Count,
                    ThreadStacks = ThreadStacks(process, processId),
                    Regions = regions.ToArray()
                };
            } finally {
                CloseHandle(process);
            }
        }
    }
}
'@
}

$snapshot = [SnowShot.MemoryProbe]::Capture($ProcessId)
$threadStacksByAllocation = @{}
foreach ($stack in $snapshot.ThreadStacks) {
    $key = [uint64]$stack.AllocationBase
    if (-not $threadStacksByAllocation.ContainsKey($key)) {
        $threadStacksByAllocation[$key] = [Collections.Generic.List[int]]::new()
    }
    $threadStacksByAllocation[$key].Add($stack.ThreadId)
}
$groups = $snapshot.Regions |
    Group-Object AllocationBase |
    ForEach-Object {
        $regions = @($_.Group)
        $sample = $null
        $privateWorkingSetBytes = ($regions | Measure-Object PrivateWorkingSet -Sum).Sum
        if ($SamplePrivateData -and $regions[0].Type -eq "Private" -and
            $privateWorkingSetBytes -ge $SampleMinimumPrivateWorkingSetMiB * 1MB) {
            $largestRegion = $regions | Sort-Object Size -Descending | Select-Object -First 1
            $sample = [SnowShot.MemoryProbe]::AnalyzeMemory(
                $ProcessId,
                [uint64]$largestRegion.BaseAddress,
                [Math]::Min([int64]1MB, [int64]$largestRegion.Size)
            )
        }
        [pscustomobject]@{
            AllocationBase = "0x{0:X16}" -f [uint64]$regions[0].AllocationBase
            Type = ($regions.Type | Sort-Object -Unique) -join "/"
            CommittedMiB = [Math]::Round(
                (($regions | Measure-Object Size -Sum).Sum / 1MB), 3)
            WorkingSetMiB = [Math]::Round(
                (($regions | Measure-Object WorkingSet -Sum).Sum / 1MB), 3)
            PrivateWorkingSetMiB = [Math]::Round(
                ($privateWorkingSetBytes / 1MB), 3)
            Regions = $regions.Count
            HeapBase = $regions.HeapBase -contains $true
            ThreadIds = if ($threadStacksByAllocation.ContainsKey(
                    [uint64]$regions[0].AllocationBase)) {
                ($threadStacksByAllocation[[uint64]$regions[0].AllocationBase] -join ",")
            } else { "" }
            Protection = ($regions.Protection |
                ForEach-Object { "0x{0:X}" -f $_ } | Sort-Object -Unique) -join "/"
            Details = ($regions.Details | Where-Object { $_ } | Sort-Object -Unique) -join ";"
            DataSample = if ($null -ne $sample) {
                [pscustomobject]@{
                    BytesRead = $sample.BytesRead
                    ZeroPercent = [Math]::Round($sample.ZeroPercent, 2)
                    FFPercent = [Math]::Round($sample.FFPercent, 2)
                    EntropyBits = [Math]::Round($sample.EntropyBits, 3)
                    OffsetZeroPercent = @($sample.OffsetZeroPercent |
                        ForEach-Object { [Math]::Round($_, 2) })
                    OffsetFFPercent = @($sample.OffsetFFPercent |
                        ForEach-Object { [Math]::Round($_, 2) })
                    FirstBytes = $sample.FirstBytes
                }
            } else { $null }
        }
    } |
    Sort-Object CommittedMiB -Descending

$json = [pscustomobject]@{
    ProcessId = $ProcessId
    PrivateMiB = [Math]::Round($snapshot.PrivateBytes / 1MB, 3)
    WorkingSetMiB = [Math]::Round($snapshot.WorkingSet / 1MB, 3)
    PeakWorkingSetMiB = [Math]::Round($snapshot.PeakWorkingSet / 1MB, 3)
    HeapCount = $snapshot.HeapCount
    ThreadStacks = @($snapshot.ThreadStacks | ForEach-Object {
        [pscustomobject]@{
            ThreadId = $_.ThreadId
            AllocationBase = "0x{0:X16}" -f [uint64]$_.AllocationBase
            StackBase = "0x{0:X16}" -f [uint64]$_.StackBase
            StackLimit = "0x{0:X16}" -f [uint64]$_.StackLimit
        }
    })
    CommittedByType = @($snapshot.Regions |
        Group-Object Type |
        ForEach-Object {
            [pscustomobject]@{
                Type = $_.Name
                CommittedMiB = [Math]::Round(
                    (($_.Group | Measure-Object Size -Sum).Sum / 1MB), 3)
                WorkingSetMiB = [Math]::Round(
                    (($_.Group | Measure-Object WorkingSet -Sum).Sum / 1MB), 3)
                PrivateWorkingSetMiB = [Math]::Round(
                    (($_.Group | Measure-Object PrivateWorkingSet -Sum).Sum / 1MB), 3)
                RegionCount = $_.Count
            }
        })
    TopAllocations = @($groups | Select-Object -First $Top)
} | ConvertTo-Json -Depth 5

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $json
} else {
    $resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
    $outputDirectory = [IO.Path]::GetDirectoryName($resolvedOutput)
    if (-not [string]::IsNullOrEmpty($outputDirectory)) {
        [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    }
    [IO.File]::WriteAllText($resolvedOutput, $json, [Text.UTF8Encoding]::new($false))
}
