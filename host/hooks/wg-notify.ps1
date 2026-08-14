# wg-notify.ps1 — Windows fallback for installs without Git Bash.
#
# Prefer the POSIX `wg-notify` script: Claude Code uses Git Bash by default on
# Windows, and PowerShell start-up is 100-300 ms versus ~3 ms for bash. Use this
# only when Git Bash is genuinely unavailable.
#
# Usage:  wg-notify.ps1 SET <STATE> [reason]
#         wg-notify.ps1 DROP
#
# Same contract as the POSIX version (CLAUDE.md Rule 3): always exit 0, never write
# to stdout or stderr, never depend on the daemon being up.

param(
	[Parameter(Position = 0)][string]$Verb,
	[Parameter(Position = 1)][string]$State,
	[Parameter(Position = 2)][string]$Reason = 'unspecified'
)

$ErrorActionPreference = 'SilentlyContinue'

try {
	if (-not $Verb) { exit 0 }

	$targetHost = if ($env:WIGWAG_HOST) { $env:WIGWAG_HOST } else { '127.0.0.1' }
	$port = if ($env:WIGWAG_LISTEN_PORT) { [int]$env:WIGWAG_LISTEN_PORT } else { 9410 }

	# Read hook JSON from stdin and pull out session_id without a JSON parser, to
	# match the POSIX version's behaviour on malformed or truncated input.
	$stdin = [Console]::In.ReadToEnd()
	$sessionId = 'unknown'
	if ($stdin -match '"session_id"\s*:\s*"([^"]+)"') { $sessionId = $Matches[1] }

	switch ($Verb.ToUpperInvariant()) {
		'SET' {
			if (-not $State) { exit 0 }
			$msg = "SET $State $sessionId $Reason"
		}
		'DROP' { $msg = "DROP $sessionId" }
		default { exit 0 }
	}

	$client = New-Object System.Net.Sockets.UdpClient
	$bytes = [System.Text.Encoding]::UTF8.GetBytes($msg + "`n")
	[void]$client.Send($bytes, $bytes.Length, $targetHost, $port)
	$client.Close()
}
catch {
	# Swallow everything. A status light is never worth failing a hook over.
}

exit 0
