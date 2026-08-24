[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$MapPath,
    [Parameter(Mandatory = $true)]
    [string]$LoadAddress,
    [Parameter(Mandatory = $true)]
    [string[]]$Address,
    [string]$PreferredAddress = "0x140000000",
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"

$map = [IO.Path]::GetFullPath($MapPath)
if (-not (Test-Path -LiteralPath $map -PathType Leaf)) {
    throw "Linker map does not exist: $map"
}

function Convert-HexAddress([string]$Value) {
    $hex = $Value.Trim()
    if ($hex.StartsWith("0x", [StringComparison]::OrdinalIgnoreCase)) {
        $hex = $hex.Substring(2)
    }
    return [Convert]::ToUInt64($hex, 16)
}

if (-not ("SnowShot.LinkerMapResolver" -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;

namespace SnowShot {
    public static class LinkerMapResolver {
        static readonly Regex SymbolPattern = new Regex(
            @"^\s*[0-9a-fA-F]+:[0-9a-fA-F]+\s+(\S+)\s+([0-9a-fA-F]{16})\s+(.*)$",
            RegexOptions.Compiled | RegexOptions.CultureInvariant);

        public sealed class Resolution {
            public string Address { get; set; }
            public string PreferredAddress { get; set; }
            public string SymbolAddress { get; set; }
            public ulong SymbolOffset { get; set; }
            public string[] Symbols { get; set; }
            public string Provenance { get; set; }
        }

        sealed class Target {
            public ulong RuntimeAddress;
            public ulong PreferredAddress;
            public int OriginalIndex;
        }

        public static Resolution[] Resolve(string mapPath, ulong loadAddress,
                                            ulong preferredBase, ulong[] addresses) {
            var targets = addresses.Select((address, index) => new Target {
                RuntimeAddress = address,
                PreferredAddress = checked(preferredBase + address - loadAddress),
                OriginalIndex = index
            }).OrderBy(target => target.PreferredAddress).ToArray();
            var results = new Resolution[addresses.Length];
            int targetIndex = 0;
            ulong previousAddress = 0;
            var previousSymbols = new List<string>();
            string previousProvenance = null;
            bool inPublics = false;

            Action<Target> resolve = target => {
                results[target.OriginalIndex] = new Resolution {
                    Address = "0x" + target.RuntimeAddress.ToString("x16"),
                    PreferredAddress = "0x" + target.PreferredAddress.ToString("x16"),
                    SymbolAddress = previousAddress == 0
                        ? null : "0x" + previousAddress.ToString("x16"),
                    SymbolOffset = previousAddress == 0
                        ? target.PreferredAddress : target.PreferredAddress - previousAddress,
                    Symbols = previousSymbols.Take(16).ToArray(),
                    Provenance = previousProvenance
                };
            };

            using (var reader = new StreamReader(mapPath, Encoding.UTF8, true, 1 << 20)) {
                string line;
                while ((line = reader.ReadLine()) != null) {
                    if (!inPublics) {
                        inPublics = line.Contains("Publics by Value");
                        continue;
                    }
                    if (line.TrimStart().StartsWith("entry point at ",
                                                    StringComparison.OrdinalIgnoreCase)) {
                        break;
                    }
                    Match match = SymbolPattern.Match(line);
                    if (!match.Success) {
                        continue;
                    }
                    ulong symbolAddress = UInt64.Parse(
                        match.Groups[2].Value, NumberStyles.AllowHexSpecifier,
                        CultureInfo.InvariantCulture);
                    while (targetIndex < targets.Length &&
                           targets[targetIndex].PreferredAddress < symbolAddress) {
                        resolve(targets[targetIndex++]);
                    }
                    if (symbolAddress != previousAddress) {
                        previousAddress = symbolAddress;
                        previousSymbols.Clear();
                        previousProvenance = match.Groups[3].Value.Trim();
                    }
                    if (previousSymbols.Count < 16) {
                        previousSymbols.Add(match.Groups[1].Value);
                    }
                }
            }
            while (targetIndex < targets.Length) {
                resolve(targets[targetIndex++]);
            }
            return results;
        }
    }
}
'@
}

$load = Convert-HexAddress $LoadAddress
$preferred = Convert-HexAddress $PreferredAddress
$addresses = [uint64[]]@($Address | ForEach-Object { Convert-HexAddress $_ })
$result = [SnowShot.LinkerMapResolver]::Resolve($map, $load, $preferred, $addresses)
$json = $result | ConvertTo-Json -Depth 4

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
