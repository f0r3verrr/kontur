$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$manager = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager,Windows.Media.Control,ContentType=WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1
} | Select-Object -First 1
$request = $asTask.MakeGenericMethod($manager).Invoke($null, @($manager::RequestAsync()))
if (!$request.Wait(5000)) { exit 2 }
$sessions = $request.Result.GetSessions() | Where-Object {
    $_.SourceAppUserModelId -in @('Яндекс Музыка.exe', 'YandexMusic.exe') -and
    $_.GetPlaybackInfo().PlaybackStatus -eq 'Playing'
}
foreach ($session in $sessions) {
    $pause = $asTask.MakeGenericMethod([bool]).Invoke($null, @($session.TryPauseAsync()))
    if (!$pause.Wait(5000) -or !$pause.Result) { exit 3 }
}
