# OhMyDisplay - restore the WebView2 SDK build-time dependency.
# Downloads the Microsoft.Web.WebView2 NuGet package and extracts just the headers and the
# static loader into third_party\webview2\. This is optional: without it the build falls
# back to a native text report. Safe to re-run (skips when already present).
param(
    [string]$Version = "latest"
)

$ErrorActionPreference = "Stop"

$root = Split-Path $PSScriptRoot -Parent
$dir = Join-Path $root "third_party\webview2"
$header = Join-Path $dir "include\WebView2.h"

if (Test-Path $header) {
    Write-Host "WebView2 SDK already present at $dir" -ForegroundColor Green
    return
}

if ($Version -eq "latest") {
    $idx = Invoke-RestMethod "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/index.json"
    $Version = $idx.versions | Where-Object { $_ -notmatch '-' } | Select-Object -Last 1
}
Write-Host "Fetching WebView2 SDK $Version ..." -ForegroundColor Cyan

New-Item -ItemType Directory -Force -Path "$dir\include", "$dir\lib\x64" | Out-Null
$nupkg = Join-Path $env:TEMP "webview2.$Version.zip"
$extract = Join-Path $env:TEMP "webview2_extract_$Version"

Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg" -OutFile $nupkg
if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
Expand-Archive -Path $nupkg -DestinationPath $extract

Copy-Item "$extract\build\native\include\WebView2.h" "$dir\include\"
Copy-Item "$extract\build\native\include\WebView2EnvironmentOptions.h" "$dir\include\"
Copy-Item "$extract\build\native\x64\WebView2LoaderStatic.lib" "$dir\lib\x64\"
"WebView2 SDK $Version (static loader) - fetched from nuget.org" | Out-File "$dir\VERSION.txt" -Encoding ascii

Remove-Item $nupkg -Force -ErrorAction SilentlyContinue
Remove-Item -Recurse -Force $extract -ErrorAction SilentlyContinue
Write-Host "WebView2 SDK $Version installed to $dir" -ForegroundColor Green
