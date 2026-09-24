[CmdletBinding()]
param(
    [switch]$Apply,
    [ValidateRange(0, 60)][int]$MutationDelaySeconds = 8,
    [Parameter(DontShow)][string]$GhExecutable = 'gh'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repository = 'mg-chao/snow-apps'
$lastIssue = 1021
$reply = @(
    '**简体中文**',
    '',
    '抱歉，这个项目已经有很长一段时间没有维护了。Snow Shot 现已使用 Qt 重写，性能更好，功能也更加丰富。目前我没有精力逐一查看并解决这些旧问题。如果您在新版本中遇到类似问题，请提交一个新 issue。',
    '',
    '**English**',
    '',
    "Sorry, this project has not been maintained for quite some time. Snow Shot has now been rewritten using Qt, with better performance and more features. I currently don’t have the capacity to review and address the old issues one by one. If you encounter a similar problem in the new version, please open a new issue."
) -join "`n"

function Invoke-Gh([string[]]$GhArguments) {
    $output = @(& $GhExecutable @GhArguments)
    if ($LASTEXITCODE -ne 0) {
        throw "GitHub CLI failed (exit $LASTEXITCODE): gh $($GhArguments -join ' ')"
    }
    return $output
}

$mutationsStarted = 0
function Invoke-GhMutation([string[]]$GhArguments) {
    if ($script:mutationsStarted -gt 0 -and $MutationDelaySeconds -gt 0) {
        Start-Sleep -Seconds $MutationDelaySeconds
    }
    $script:mutationsStarted++
    return Invoke-Gh $GhArguments
}

# The REST issues endpoint also returns pull requests, so exclude them explicitly.
$issueLines = @(Invoke-Gh @('api', '--paginate',
    "repos/$repository/issues?state=open&per_page=100",
    '--jq', '.[] | select(.pull_request == null) | .number'))
$numbers = @($issueLines | ForEach-Object {
    if ($_ -notmatch '^\d+$') { throw "Unexpected issue number from GitHub: $_" }
    [int]$_
} | Where-Object { $_ -le $lastIssue } | Sort-Object -Unique)

Write-Output "Found $($numbers.Count) open issues in $repository through #$lastIssue."
if ($numbers.Count -eq 0) { return }
Write-Output "Issue numbers: $($numbers -join ', ')"
Write-Output "`nReply:`n$reply"
if (-not $Apply) {
    Write-Output "`nPreview only. Run this script with -Apply to post the reply and close these issues."
    return
}
Write-Output "`nApplying with at least $MutationDelaySeconds seconds between GitHub writes."

$bodyFile = Join-Path ([IO.Path]::GetTempPath()) "snow-apps-legacy-reply-$([guid]::NewGuid().ToString('N')).md"
[IO.File]::WriteAllText($bodyFile, $reply, [Text.UTF8Encoding]::new($false))
$commented = 0
$reused = 0
$closed = 0
$alreadyClosed = 0
try {
    foreach ($number in $numbers) {
        # An issue can be closed after the initial listing. Do not comment on it then.
        $state = (Invoke-Gh @('api', "repos/$repository/issues/$number", '--jq', '.state') | Out-String).Trim()
        if ($state -eq 'closed') {
            $alreadyClosed++
            Write-Output "#$number was already closed; skipped."
            continue
        }
        if ($state -ne 'open') { throw "Unexpected state for issue #${number}: $state" }

        # Checking the exact body makes a rerun safe after a partial failure.
        $commentJson = (Invoke-Gh @('api', '--paginate', '--slurp',
            "repos/$repository/issues/$number/comments?per_page=100") | Out-String)
        $pages = $commentJson | ConvertFrom-Json -Depth 100
        $hasReply = $false
        foreach ($page in $pages) {
            foreach ($comment in $page) {
                $existingBody = [string]$comment.body -replace "`r`n", "`n"
                if ($existingBody.TrimEnd("`r", "`n") -ceq $reply) {
                    $hasReply = $true
                    break
                }
            }
            if ($hasReply) { break }
        }

        if ($hasReply) {
            $reused++
            Write-Output "#$number already has the reply."
        } else {
            $null = Invoke-GhMutation @('issue', 'comment', "$number", '--repo', $repository,
                '--body-file', $bodyFile)
            $commented++
            Write-Output "#$number replied."
        }

        $null = Invoke-GhMutation @('issue', 'close', "$number", '--repo', $repository)
        $closed++
        Write-Output "#$number closed."
    }
} finally {
    Remove-Item -LiteralPath $bodyFile -ErrorAction SilentlyContinue
}
Write-Output "Done: $commented new replies, $reused existing replies, $closed issues closed, $alreadyClosed skipped."
