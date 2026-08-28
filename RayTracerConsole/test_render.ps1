$exe = "$PSScriptRoot\x64\Release\RayTracer.exe"
$pass = 0
$fail = 0

function Run-Test {
    param([string]$Label, [string[]]$Args, [string]$ExpectedOutput)
    Write-Host "`n=== $Label ===" -ForegroundColor Cyan
    $result = & $exe @Args 2>&1
    Write-Host $result
    if (Test-Path $ExpectedOutput) {
        $size = (Get-Item $ExpectedOutput).Length
        Write-Host "OK: $ExpectedOutput ($size bytes)" -ForegroundColor Green
        $script:pass++
    } else {
        Write-Host "FAIL: $ExpectedOutput が生成されませんでした" -ForegroundColor Red
        $script:fail++
    }
}

# 1. レガシーモード（コマンドライン引数）
Run-Test "Legacy: renderCornellBox (200x200, spp=10)" @("200","200","10") "cornell_box.png"

# 2. .cscene ファイル: Cornell Box 再現
Run-Test ".cscene: cornell_box.cscene" @("$PSScriptRoot\cornell_box.cscene") "cornell_box_scene.png"

# 3. .cscene ファイル: PBR (chrome球 + glass球)
Run-Test ".cscene: cornell_box_pbr.cscene" @("$PSScriptRoot\cornell_box_pbr.cscene") "cornell_box_pbr.png"

# 4. 不正ファイル → エラーメッセージ確認
Write-Host "`n=== Error handling: 存在しないファイル ===" -ForegroundColor Cyan
$err = & $exe "nonexistent.cscene" 2>&1
Write-Host $err
if ($LASTEXITCODE -ne 0) {
    Write-Host "OK: 非ゼロ終了コード ($LASTEXITCODE)" -ForegroundColor Green
    $pass++
} else {
    Write-Host "FAIL: エラー時にゼロ終了コードが返りました" -ForegroundColor Red
    $fail++
}

Write-Host "`n==============================" -ForegroundColor White
Write-Host "PASS: $pass  FAIL: $fail" -ForegroundColor $(if ($fail -eq 0) { "Green" } else { "Red" })
