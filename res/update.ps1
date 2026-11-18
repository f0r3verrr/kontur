$ErrorActionPreference = 'Stop'
function Notice($message) {
    Add-Type -AssemblyName System.Windows.Forms
    [void][Windows.Forms.MessageBox]::Show($message, 'Контур')
}
$lock = [Threading.Mutex]::new($false, 'Local\KonturUpdate')
try { $locked = $lock.WaitOne(0) } catch [Threading.AbandonedMutexException] { $locked = $true }
if (!$locked) {
    if ($manual) { Notice 'Проверка обновлений уже идёт.' }
    $lock.Dispose()
    exit
}
$stage = "$app.update"
$backup = "$app.previous"
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $repo = 'https://api.github.com/repos/f0r3verrr/kontur'
    $release = Invoke-RestMethod -UseBasicParsing "$repo/releases/latest" -TimeoutSec 12
    if ($release.tag_name -notmatch '^v(\d+\.\d+\.\d+)$') { throw 'Unexpected release tag' }
    $next = [version]$Matches[1]
    $current = [version]([Diagnostics.FileVersionInfo]::GetVersionInfo($app).FileVersion)
    if ($next -le $current) {
        if ($manual) { Notice 'У тебя уже последняя версия Контура.' }
        return
    }
    $asset = $release.assets | Where-Object name -eq 'Kontur-update-win-x64.exe' | Select-Object -First 1
    $uri = "https://github.com/f0r3verrr/kontur/releases/download/$($release.tag_name)/Kontur-update-win-x64.exe"
    if (!$asset -or $asset.browser_download_url -ne $uri -or $asset.digest -notmatch '^sha256:[a-f0-9]{64}$') {
        throw 'Verified update asset unavailable'
    }
    Invoke-WebRequest -UseBasicParsing $uri -OutFile $stage -TimeoutSec 30
    $digest = (Get-FileHash -LiteralPath $stage -Algorithm SHA256).Hash
    if (("sha256:" + $digest) -ne $asset.digest -or
        [version]([Diagnostics.FileVersionInfo]::GetVersionInfo($stage).FileVersion) -ne $next) {
        throw 'Update verification failed'
    }
    $parent = Get-Process -Id $appPid -ErrorAction SilentlyContinue
    if (!$parent -or $parent.Path -ne $app) { return }
    & $app --quit
    if (!$parent.WaitForExit(12000)) { throw 'Kontur did not exit' }
    try {
        [IO.File]::Replace($stage, $app, $backup)
        if ($manual) { $process = Start-Process -FilePath $app -WindowStyle Hidden -PassThru }
        else { $process = Start-Process -FilePath $app -ArgumentList '--background' -WindowStyle Hidden -PassThru }
        if ($process.WaitForExit(800)) { throw 'Updated Kontur exited early' }
    } catch {
        if (Test-Path -LiteralPath $backup) {
            [IO.File]::Replace($backup, $app, "$stage.failed")
            Remove-Item -LiteralPath "$stage.failed" -ErrorAction SilentlyContinue
        }
        Start-Process -FilePath $app -ArgumentList '--background' -WindowStyle Hidden
        throw
    }
    Remove-Item -LiteralPath $backup -ErrorAction SilentlyContinue
} catch {
    if ($manual) { Notice 'Не удалось проверить или установить обновление. Попробуй ещё раз позже.' }
} finally {
    Remove-Item -LiteralPath $stage -ErrorAction SilentlyContinue
    $lock.ReleaseMutex()
    $lock.Dispose()
}
