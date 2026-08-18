# mini-cli.ps1 — a tiny interactive client for miniredis.
#
# Lets you type commands (SET, GET, TTL, ...) and see decoded replies, without
# needing the real redis-cli installed. Usage:
#
#     ./tools/mini-cli.ps1              # connects to 127.0.0.1:6379
#     ./tools/mini-cli.ps1 -Port 6400   # or a port of your choice
#
# Type 'quit' (or Ctrl+C) to exit.

param([int]$Port = 6379)

try {
    $client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
} catch {
    Write-Host "Could not connect to 127.0.0.1:$Port. Is the server running?" -ForegroundColor Red
    exit 1
}
$stream = $client.GetStream()
Write-Host "Connected to miniredis on port $Port. Type a command, or 'quit'." -ForegroundColor Green

# Encodes a command as a RESP array of bulk strings.
function Encode-Command([string[]]$parts) {
    $s = "*$($parts.Count)`r`n"
    foreach ($p in $parts) { $s += "`$$($p.Length)`r`n$p`r`n" }
    return $s
}

# Decodes a single RESP reply into something human-readable.
function Decode-Reply([string]$raw) {
    if ($raw.Length -eq 0) { return "(no reply)" }
    $type = $raw[0]
    $body = $raw.Substring(1).TrimEnd("`r", "`n")
    switch ($type) {
        '+' { return $body }
        '-' { return "(error) $body" }
        ':' { return "(integer) $body" }
        '$' {
            if ($body -eq "-1") { return "(nil)" }
            $lines = ($raw -split "`r`n")
            if ($lines.Count -ge 2) { return "`"$($lines[1])`"" }
            return $body
        }
        default { return $raw }
    }
}

while ($true) {
    Write-Host -NoNewline "> "
    $line = [Console]::ReadLine()
    if ($null -eq $line) { break }
    $line = $line.Trim()
    if ($line -eq "") { continue }
    if ($line -eq "quit" -or $line -eq "exit") { break }

    $parts = $line -split '\s+'
    $bytes = [System.Text.Encoding]::UTF8.GetBytes((Encode-Command $parts))
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush()

    Start-Sleep -Milliseconds 80
    $buf = New-Object byte[] 8192
    $n = $stream.Read($buf, 0, $buf.Length)
    $raw = [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
    Write-Host (Decode-Reply $raw)
}

$client.Close()
Write-Host "bye."
