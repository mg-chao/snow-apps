[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [int]$ProcessId,
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

if (-not ("SnowShot.ThreadProbe" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;

namespace SnowShot {
    public static class ThreadProbe {
        const uint THREAD_QUERY_INFORMATION = 0x0040;
        const uint THREAD_QUERY_LIMITED_INFORMATION = 0x0800;
        const int ThreadQuerySetWin32StartAddress = 9;

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenThread(uint access, bool inheritHandle, uint threadId);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr handle);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern int GetThreadDescription(IntPtr thread, out IntPtr description);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr LocalFree(IntPtr memory);

        [DllImport("ntdll.dll")]
        static extern int NtQueryInformationThread(IntPtr thread, int informationClass,
                                                    out IntPtr information,
                                                    int informationLength,
                                                    IntPtr returnLength);

        public sealed class Details {
            public string Description { get; set; }
            public ulong StartAddress { get; set; }
        }

        public static Details Inspect(uint threadId) {
            IntPtr thread = OpenThread(THREAD_QUERY_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
                                       false, threadId);
            if (thread == IntPtr.Zero) {
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            try {
                string description = null;
                IntPtr descriptionPointer;
                int descriptionResult = GetThreadDescription(thread, out descriptionPointer);
                if (descriptionResult >= 0 && descriptionPointer != IntPtr.Zero) {
                    try {
                        description = Marshal.PtrToStringUni(descriptionPointer);
                    } finally {
                        LocalFree(descriptionPointer);
                    }
                }

                IntPtr startAddress;
                int status = NtQueryInformationThread(
                    thread, ThreadQuerySetWin32StartAddress, out startAddress,
                    IntPtr.Size, IntPtr.Zero);
                return new Details {
                    Description = description,
                    StartAddress = status == 0
                        ? unchecked((ulong)startAddress.ToInt64())
                        : 0
                };
            } finally {
                CloseHandle(thread);
            }
        }
    }
}
'@
}

$process = Get-Process -Id $ProcessId -ErrorAction Stop
$modules = @($process.Modules | ForEach-Object {
    [pscustomobject]@{
        Name = $_.ModuleName
        Path = $_.FileName
        Base = [uint64]$_.BaseAddress.ToInt64()
        End = [uint64]$_.BaseAddress.ToInt64() + [uint64]$_.ModuleMemorySize
    }
})

$threads = foreach ($thread in $process.Threads) {
    try {
        $details = [SnowShot.ThreadProbe]::Inspect([uint32]$thread.Id)
        $module = $modules | Where-Object {
            $details.StartAddress -ge $_.Base -and $details.StartAddress -lt $_.End
        } | Select-Object -First 1
        $startTime = $null
        try {
            $startTime = $thread.StartTime.ToUniversalTime().ToString("o")
        } catch {
        }
        [pscustomobject]@{
            thread_id = $thread.Id
            description = $details.Description
            start_address = ('0x{0:x16}' -f $details.StartAddress)
            start_module = $module.Name
            start_module_offset = if ($null -eq $module) {
                $null
            } else {
                ('0x{0:x}' -f ($details.StartAddress - $module.Base))
            }
            start_time_utc = $startTime
            state = $thread.ThreadState.ToString()
            wait_reason = if ($thread.ThreadState -eq [System.Diagnostics.ThreadState]::Wait) {
                $thread.WaitReason.ToString()
            } else {
                $null
            }
        }
    } catch {
        [pscustomobject]@{
            thread_id = $thread.Id
            error = $_.Exception.Message
        }
    }
}

$json = [pscustomobject]@{
    process_id = $process.Id
    process_name = $process.ProcessName
    captured_at_utc = [DateTime]::UtcNow.ToString("o")
    thread_count = @($threads).Count
    threads = @($threads | Sort-Object thread_id)
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
