[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,
    [string]$ComparisonInputPath,
    [int]$BaselineInstance = 1,
    [int]$ComparisonInstance = 2,
    [int]$Top = 100,
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

$input = [IO.Path]::GetFullPath($InputPath)
if (-not (Test-Path -LiteralPath $input -PathType Leaf)) {
    throw "Heap snapshot export does not exist: $input"
}
$comparisonInput = if ([string]::IsNullOrWhiteSpace($ComparisonInputPath)) {
    $input
} else {
    [IO.Path]::GetFullPath($ComparisonInputPath)
}
if (-not (Test-Path -LiteralPath $comparisonInput -PathType Leaf)) {
    throw "Comparison heap snapshot export does not exist: $comparisonInput"
}
if ($BaselineInstance -lt 1 -or $ComparisonInstance -lt 1) {
    throw "Snapshot instance numbers must be positive."
}
if ($Top -lt 1) {
    throw "Top must be positive."
}

if (-not ("SnowShot.HeapSnapshotAnalyzer" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace SnowShot {
    public static class HeapSnapshotAnalyzer {
        static readonly Regex InstancePattern = new Regex(
            @"^PID \[[0-9]+\] Instance \[\s*([0-9]+)\]",
            RegexOptions.Compiled | RegexOptions.CultureInvariant);
        static readonly Regex StackPattern = new Regex(
            @"^\s*StackReference\s*:\s*[^,]+, identical references\s*=\s*([0-9]+)",
            RegexOptions.Compiled | RegexOptions.CultureInvariant);
        static readonly Regex AddressPattern = new Regex(
            @"^\s*(0x[0-9a-fA-F]+)\s*$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant);
        static readonly Regex TotalPattern = new Regex(
            @"^\s*Total Allocation of this stack:\s*([0-9]+)",
            RegexOptions.Compiled | RegexOptions.CultureInvariant);

        sealed class Totals {
            public long Bytes;
            public long Count;
        }

        public sealed class Result {
            public string InputPath { get; set; }
            public string ComparisonInputPath { get; set; }
            public int BaselineInstance { get; set; }
            public int ComparisonInstance { get; set; }
            public long BaselineBytes { get; set; }
            public long ComparisonBytes { get; set; }
            public long DeltaBytes { get; set; }
            public long BaselineCount { get; set; }
            public long ComparisonCount { get; set; }
            public long DeltaCount { get; set; }
            public int DistinctStacks { get; set; }
            public StackDelta[] TopDeltas { get; set; }
        }

        public sealed class StackDelta {
            public long BaselineBytes { get; set; }
            public long ComparisonBytes { get; set; }
            public long DeltaBytes { get; set; }
            public long BaselineCount { get; set; }
            public long ComparisonCount { get; set; }
            public long DeltaCount { get; set; }
            public string[] Stack { get; set; }
        }

        static Dictionary<int, Dictionary<string, Totals>> Parse(string inputPath) {
            var instances = new Dictionary<int, Dictionary<string, Totals>>();
            int currentInstance = 0;
            long currentCount = 0;
            var currentStack = new List<string>();

            using (var reader = new StreamReader(inputPath, Encoding.UTF8, true, 1 << 20)) {
                string line;
                while ((line = reader.ReadLine()) != null) {
                    Match match = InstancePattern.Match(line);
                    if (match.Success) {
                        currentInstance = Int32.Parse(match.Groups[1].Value,
                                                      CultureInfo.InvariantCulture);
                        currentCount = 0;
                        currentStack.Clear();
                        continue;
                    }

                    match = StackPattern.Match(line);
                    if (match.Success) {
                        currentCount = Int64.Parse(match.Groups[1].Value,
                                                   CultureInfo.InvariantCulture);
                        currentStack.Clear();
                        continue;
                    }

                    match = AddressPattern.Match(line);
                    if (match.Success && currentCount > 0) {
                        currentStack.Add(match.Groups[1].Value.ToLowerInvariant());
                        continue;
                    }

                    match = TotalPattern.Match(line);
                    if (!match.Success || currentInstance == 0 ||
                        currentCount == 0 || currentStack.Count == 0) {
                        continue;
                    }

                    long bytes = Int64.Parse(match.Groups[1].Value,
                                             CultureInfo.InvariantCulture);
                    string key = String.Join(";", currentStack);
                    Dictionary<string, Totals> stacks;
                    if (!instances.TryGetValue(currentInstance, out stacks)) {
                        stacks = new Dictionary<string, Totals>(StringComparer.Ordinal);
                        instances.Add(currentInstance, stacks);
                    }
                    Totals totals;
                    if (!stacks.TryGetValue(key, out totals)) {
                        totals = new Totals();
                        stacks.Add(key, totals);
                    }
                    totals.Bytes += bytes;
                    totals.Count += currentCount;
                    currentCount = 0;
                    currentStack.Clear();
                }
            }

            return instances;
        }

        public static Result Analyze(string inputPath, int baselineInstance,
                                     string comparisonInputPath, int comparisonInstance,
                                     int top) {
            Dictionary<string, Totals> baseline;
            Dictionary<string, Totals> comparison;
            var baselineInstances = Parse(inputPath);
            var comparisonInstances = String.Equals(
                Path.GetFullPath(inputPath), Path.GetFullPath(comparisonInputPath),
                StringComparison.OrdinalIgnoreCase)
                ? baselineInstances : Parse(comparisonInputPath);
            if (!baselineInstances.TryGetValue(baselineInstance, out baseline)) {
                throw new InvalidDataException("Baseline instance was not found: " +
                                               baselineInstance);
            }
            if (!comparisonInstances.TryGetValue(comparisonInstance, out comparison)) {
                throw new InvalidDataException("Comparison instance was not found: " +
                                               comparisonInstance);
            }

            var keys = new HashSet<string>(baseline.Keys, StringComparer.Ordinal);
            keys.UnionWith(comparison.Keys);
            var deltas = new List<StackDelta>(keys.Count);
            foreach (string key in keys) {
                Totals before;
                Totals after;
                baseline.TryGetValue(key, out before);
                comparison.TryGetValue(key, out after);
                long beforeBytes = before == null ? 0 : before.Bytes;
                long afterBytes = after == null ? 0 : after.Bytes;
                long beforeCount = before == null ? 0 : before.Count;
                long afterCount = after == null ? 0 : after.Count;
                deltas.Add(new StackDelta {
                    BaselineBytes = beforeBytes,
                    ComparisonBytes = afterBytes,
                    DeltaBytes = afterBytes - beforeBytes,
                    BaselineCount = beforeCount,
                    ComparisonCount = afterCount,
                    DeltaCount = afterCount - beforeCount,
                    Stack = key.Split(';')
                });
            }

            long baselineBytes = baseline.Values.Sum(value => value.Bytes);
            long comparisonBytes = comparison.Values.Sum(value => value.Bytes);
            long baselineCount = baseline.Values.Sum(value => value.Count);
            long comparisonCount = comparison.Values.Sum(value => value.Count);
            return new Result {
                InputPath = Path.GetFullPath(inputPath),
                ComparisonInputPath = Path.GetFullPath(comparisonInputPath),
                BaselineInstance = baselineInstance,
                ComparisonInstance = comparisonInstance,
                BaselineBytes = baselineBytes,
                ComparisonBytes = comparisonBytes,
                DeltaBytes = comparisonBytes - baselineBytes,
                BaselineCount = baselineCount,
                ComparisonCount = comparisonCount,
                DeltaCount = comparisonCount - baselineCount,
                DistinctStacks = keys.Count,
                TopDeltas = deltas
                    .OrderByDescending(delta => delta.DeltaBytes)
                    .ThenByDescending(delta => delta.DeltaCount)
                    .Take(top)
                    .ToArray()
            };
        }
    }
}
'@
}

$result = [SnowShot.HeapSnapshotAnalyzer]::Analyze(
    $input, $BaselineInstance, $comparisonInput, $ComparisonInstance, $Top)
$json = $result | ConvertTo-Json -Depth 5

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $json
} else {
    $output = [IO.Path]::GetFullPath($OutputPath)
    $outputDirectory = [IO.Path]::GetDirectoryName($output)
    if (-not [string]::IsNullOrEmpty($outputDirectory)) {
        [IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
    }
    [IO.File]::WriteAllText($output, $json, [Text.UTF8Encoding]::new($false))
}
