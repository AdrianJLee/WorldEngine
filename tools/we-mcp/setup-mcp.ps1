[CmdletBinding()]
param(
    [string]$ProjectDir = "",
    [string]$TargetClient = "all", # all, cursor, claude, codex, windsurf, cline
    [int]$Port = 8089,
    [string]$Role = "producer"
)

$ErrorActionPreference = "Stop"

# 1. 解析 we-mcp.exe 绝对路径
$exeCandidates = @(
    (Join-Path $PSScriptRoot "bin\we-mcp.exe"),
    (Join-Path $PSScriptRoot "bin\WeMcp.exe"),
    (Join-Path $PSScriptRoot "..\..\build\x64-Debug\Launcher\Debug\we-mcp.exe"),
    (Join-Path $PSScriptRoot "..\..\build\x64-Debug\Launcher\Debug\WeMcp.exe")
)

$mcpExe = $null
foreach ($cand in $exeCandidates) {
    if (Test-Path $cand) {
        $mcpExe = (Resolve-Path $cand).Path
        break
    }
}

if (-not $mcpExe) {
    Write-Error "Cannot locate we-mcp.exe! Please compile WeMcp first."
    exit 1
}

# 2. 解析项目根路径
if ([string]::IsNullOrWhiteSpace($ProjectDir)) {
    $engineRoot = (Resolve-Path "$PSScriptRoot\..\..").Path
    $ProjectDir = $engineRoot
} else {
    if (Test-Path $ProjectDir) {
        $ProjectDir = (Resolve-Path $ProjectDir).Path
    }
}

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host " WorldEngine MCP Client Auto-Configurator" -ForegroundColor Green
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  MCP Binary  : $mcpExe"
Write-Host "  Project Dir : $ProjectDir"
Write-Host "  Port        : $Port"
Write-Host "  Role        : $Role"
Write-Host "----------------------------------------------------------"

$serverConfig = @{
    command = $mcpExe
    args = @(
        "--project=$ProjectDir",
        "--port=$Port",
        "--role=$Role"
    )
}

function Update-McpConfigFile($filePath, $clientName) {
    try {
        $parentDir = Split-Path -Parent $filePath
        if (-not (Test-Path $parentDir)) {
            New-Item -ItemType Directory -Path $parentDir -Force | Out-Null
        }

        $json = @{ mcpServers = @{} }
        if (Test-Path $filePath) {
            $raw = Get-Content $filePath -Raw -Encoding UTF8
            if (-not [string]::IsNullOrWhiteSpace($raw)) {
                $parsed = $raw | ConvertFrom-Json -AsHashtable
                if ($parsed -and $parsed.ContainsKey("mcpServers")) {
                    $json = $parsed
                }
            }
        }

        if (-not $json.ContainsKey("mcpServers") -or $null -eq $json["mcpServers"]) {
            $json["mcpServers"] = @{}
        }

        $json["mcpServers"]["worldengine"] = $serverConfig

        $jsonContent = $json | ConvertTo-Json -Depth 10
        Set-Content -Path $filePath -Value $jsonContent -Encoding UTF8
        Write-Host "  [OK] $clientName configured -> $filePath" -ForegroundColor Green
    }
    catch {
        Write-Host "  [WARN] Failed to configure $clientName ($filePath): $_" -ForegroundColor Yellow
    }
}

# 3. 注入各客户端配置
$targets = @()

if ($TargetClient -eq "all" -or $TargetClient -eq "cursor") {
    $targets += @{ Name = "Cursor (Workspace)"; Path = (Join-Path $ProjectDir ".cursor\mcp.json") }
    $targets += @{ Name = "Cursor (Global)";    Path = (Join-Path $env:USERPROFILE ".cursor\mcp.json") }
}

if ($TargetClient -eq "all" -or $TargetClient -eq "claude") {
    $claudePath = Join-Path $env:APPDATA "Claude\claude_desktop_config.json"
    $targets += @{ Name = "Claude Desktop"; Path = $claudePath }
}

if ($TargetClient -eq "all" -or $TargetClient -eq "codex") {
    $codexPath = Join-Path $env:USERPROFILE ".codex\mcp.json"
    $targets += @{ Name = "Codex"; Path = $codexPath }
}

if ($TargetClient -eq "all" -or $TargetClient -eq "windsurf") {
    $windsurfPath = Join-Path $env:USERPROFILE ".codeium\windsurf\mcp_config.json"
    $targets += @{ Name = "Windsurf"; Path = $windsurfPath }
}

if ($TargetClient -eq "all" -or $TargetClient -eq "cline") {
    $clinePath = Join-Path $env:APPDATA "Code\User\globalStorage\saoudrizwan.claude-dev\settings\cline_mcp_settings.json"
    $targets += @{ Name = "VS Code (Cline)"; Path = $clinePath }
}

foreach ($t in $targets) {
    Update-McpConfigFile -filePath $t.Path -clientName $t.Name
}

Write-Host "----------------------------------------------------------"
Write-Host " Configuration complete! Restart your AI client to connect." -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
