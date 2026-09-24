$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

$testDirectory = Join-Path ([IO.Path]::GetTempPath()) "snow-apps-legacy-issues-test-$([guid]::NewGuid().ToString('N'))"
$null = New-Item -ItemType Directory -Path $testDirectory
$mockGh = Join-Path $testDirectory 'gh-mock.ps1'
$logFile = Join-Path $testDirectory 'calls.jsonl'
$expectedReplyFile = Join-Path $testDirectory 'expected-reply.md'
$postedReplyFile = Join-Path $testDirectory 'posted-reply.md'
$expectedReply = @(
    '**简体中文**',
    '',
    '抱歉，这个项目已经有很长一段时间没有维护了。Snow Shot 现已使用 Qt 重写，性能更好，功能也更加丰富。目前我没有精力逐一查看并解决这些旧问题。如果您在新版本中遇到类似问题，请提交一个新 issue。',
    '',
    '**English**',
    '',
    "Sorry, this project has not been maintained for quite some time. Snow Shot has now been rewritten using Qt, with better performance and more features. I currently don’t have the capacity to review and address the old issues one by one. If you encounter a similar problem in the new version, please open a new issue."
) -join "`n"

$mockSource = @'
param([Parameter(ValueFromRemainingArguments)][string[]]$GhArguments)
$GhArguments = @($GhArguments)
Add-Content -LiteralPath $env:SNOW_TEST_GH_LOG -Value (ConvertTo-Json -InputObject @{ arguments = $GhArguments } -Compress)
$call = $GhArguments -join ' '
if ($call -like 'api --paginate repos/mg-chao/snow-apps/issues?state=open&per_page=100*') {
    '1022', '1021', '62', '61'
    exit 0
}
if ($call -like 'api repos/mg-chao/snow-apps/issues/61 *' -or
    $call -like 'api repos/mg-chao/snow-apps/issues/1021 *') {
    'open'
    exit 0
}
if ($call -like 'api repos/mg-chao/snow-apps/issues/62 *') {
    'closed'
    exit 0
}
if ($call -like 'api --paginate --slurp repos/mg-chao/snow-apps/issues/61/comments*') {
    '[[]]'
    exit 0
}
if ($call -like 'api --paginate --slurp repos/mg-chao/snow-apps/issues/1021/comments*') {
    $body = Get-Content -LiteralPath $env:SNOW_TEST_EXPECTED_REPLY -Raw -Encoding utf8
    '[[' + (ConvertTo-Json -InputObject @{ body = $body } -Compress) + ']]'
    exit 0
}
if ($GhArguments[0] -eq 'issue' -and $GhArguments[1] -eq 'comment') {
    Copy-Item -LiteralPath $GhArguments[6] -Destination $env:SNOW_TEST_POSTED_REPLY
    exit 0
}
if ($GhArguments[0] -eq 'issue' -and $GhArguments[1] -eq 'close') { exit 0 }
throw "Unexpected GitHub CLI call: $call"
'@

try {
    [IO.File]::WriteAllText($mockGh, $mockSource, [Text.UTF8Encoding]::new($false))
    [IO.File]::WriteAllText($expectedReplyFile, $expectedReply, [Text.UTF8Encoding]::new($false))
    $env:SNOW_TEST_GH_LOG = $logFile
    $env:SNOW_TEST_EXPECTED_REPLY = $expectedReplyFile
    $env:SNOW_TEST_POSTED_REPLY = $postedReplyFile
    $script = Join-Path $PSScriptRoot 'close-legacy-issues.ps1'

    $preview = & $script -GhExecutable $mockGh | Out-String
    Assert ($preview -match 'Found 3 open issues') 'The preview should include only issues through #1021.'
    Assert ($preview -match 'Preview only') 'The default run should be a preview.'
    Assert (-not (Test-Path -LiteralPath $postedReplyFile)) 'The preview posted a comment.'
    $previewCalls = @(Get-Content -LiteralPath $logFile | ConvertFrom-Json)
    Assert ($previewCalls.Count -eq 1) 'The preview should only list issues.'
    Assert ($previewCalls[0].arguments[-1] -match 'pull_request == null') `
        'The issue listing must exclude pull requests.'

    Clear-Content -LiteralPath $logFile
    $result = & $script -Apply -MutationDelaySeconds 0 -GhExecutable $mockGh | Out-String
    Assert ($result -match '1 new replies, 1 existing replies, 2 issues closed, 1 skipped') `
        'The apply run reported unexpected results.'
    $calls = @(Get-Content -LiteralPath $logFile | ForEach-Object { $_ | ConvertFrom-Json })
    $mutations = @($calls | Where-Object { $_.arguments[0] -eq 'issue' })
    Assert ($mutations.Count -eq 3) 'Expected one comment and two close operations.'
    Assert ($mutations[0].arguments[1] -eq 'comment' -and $mutations[0].arguments[2] -eq '61') 'Issue #61 should be commented on first.'
    Assert ($mutations[1].arguments[1] -eq 'close' -and $mutations[1].arguments[2] -eq '61') 'Issue #61 should be closed after commenting.'
    Assert ($mutations[2].arguments[1] -eq 'close' -and $mutations[2].arguments[2] -eq '1021') 'Issue #1021 should close without a duplicate comment.'
    $postedReply = Get-Content -LiteralPath $postedReplyFile -Raw -Encoding utf8
    Assert ($postedReply -ceq $expectedReply) 'The posted bilingual reply differs from the requested text.'
    Write-Output 'Legacy issue script test passed.'
} finally {
    Remove-Item Env:SNOW_TEST_GH_LOG, Env:SNOW_TEST_EXPECTED_REPLY, Env:SNOW_TEST_POSTED_REPLY -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $mockGh, $logFile, $expectedReplyFile, $postedReplyFile -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $testDirectory -Force -ErrorAction SilentlyContinue
}
