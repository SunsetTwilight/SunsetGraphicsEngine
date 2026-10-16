# ============================================================
#  tools/check-sample.ps1
#
#  ビルド済みの GfxExample を自動テストモードで実行し、D3D12 のバリデーションメッセージが
#  0 件であることを確認する。GFX_ENABLE_VALIDATION=ON でビルドした bin/ を指定すること。
#
#  使い方:
#    powershell -ExecutionPolicy Bypass -File tools/check-sample.ps1 -BinDir build/windows-debug/bin
#
#  終了コード: すべて成功なら 0、いずれかが失敗したら 1
# ============================================================
param(
	[Parameter(Mandatory = $true)][string]$BinDir,
	[int]$Frames = 600,
	[int]$TimeoutSec = 240
)

$ErrorActionPreference = 'Stop'
$exe = Join-Path $BinDir 'GfxExample.exe'
if (-not (Test-Path $exe)) { Write-Error "GfxExample.exe が見つかりません: $exe" }

function Invoke-Check([string]$name, [hashtable]$envVars)
{
	foreach ($k in $envVars.Keys) { Set-Item -Path "Env:$k" -Value $envVars[$k] }
	$out = Join-Path $env:TEMP "gfx_check_$name.out"
	$err = Join-Path $env:TEMP "gfx_check_$name.err"
	try
	{
		$p = Start-Process -FilePath $exe -WorkingDirectory $BinDir -NoNewWindow -PassThru `
			-RedirectStandardOutput $out -RedirectStandardError $err
		# ExitCode を後で読めるよう、終了前にハンドルを取得しておく (Start-Process -PassThru の既知の問題)
		$null = $p.Handle
		if (-not $p.WaitForExit($TimeoutSec * 1000))
		{
			$p.Kill()
			Write-Host "[NG] $name : タイムアウト ($TimeoutSec 秒)"
			return $false
		}
		$p.WaitForExit()
	}
	finally
	{
		foreach ($k in $envVars.Keys) { Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue }
	}

	$d3d12 = @(Select-String -Path $err -Pattern '\[D3D12 ' -ErrorAction SilentlyContinue).Count
	$ok    = ($p.ExitCode -eq 0) -and ($d3d12 -eq 0)
	$mark  = if ($ok) { 'OK' } else { 'NG' }
	Write-Host "[$mark] $name : exit=$($p.ExitCode) d3d12=$d3d12"
	if (-not $ok) { Get-Content $err -TotalCount 10 | ForEach-Object { Write-Host "      $_" } }
	return $ok
}

$results = @(
	(Invoke-Check 'frames' @{ GFX_EXAMPLE_FRAMES = "$Frames" }),
	(Invoke-Check 'resize' @{ GFX_EXAMPLE_FRAMES = '500'; GFX_EXAMPLE_RESIZE_TEST = '1' })
)
if ($results -contains $false) { exit 1 }
exit 0
