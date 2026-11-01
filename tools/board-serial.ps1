param(
    [Parameter(Mandatory=$true)][string]$Command,
    [int]$Seconds = 4,
    [string]$Port = 'COM7'
)
$serial = [System.IO.Ports.SerialPort]::new($Port,115200,[System.IO.Ports.Parity]::None,8,[System.IO.Ports.StopBits]::One)
$serial.Handshake = 'None'
try {
    $serial.Open()
    $serial.Write($Command + "`r")
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    while ($timer.Elapsed.TotalSeconds -lt $Seconds) {
        $chunk = $serial.ReadExisting()
        if ($chunk) { [Console]::Write($chunk) }
        Start-Sleep -Milliseconds 100
    }
} finally {
    if ($serial.IsOpen) { $serial.Close() }
    $serial.Dispose()
}
