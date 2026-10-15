# Fetches what the tests build against into tests\external (not tracked, see .gitignore), at pinned commits:
#   injector          ThirteenAG/injector (with its safetyhook submodule and zydis): gameref.hpp, callbacks.h
#   hooking           ThirteenAG/Hooking.Patterns: callbacks.h
# The fixes bring their own copies of these. ImGui is not fetched here: it's the imgui submodule
# (tracking master).
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\fetch-test-deps.ps1

$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path -Parent $PSScriptRoot) 'tests\external'
New-Item -ItemType Directory -Force -Path $root | Out-Null

function Fetch($name, $url, $commit)
{
    $dir = Join-Path $root $name
    if (Test-Path (Join-Path $dir '.git'))
    {
        $head = (git -C $dir rev-parse HEAD).Trim()
        if ($head -eq $commit) { Write-Host "$name $commit"; return $dir }
    }
    if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    git -C $dir init -q
    git -C $dir remote add origin $url
    git -C $dir fetch -q --depth 1 origin $commit
    if ($LASTEXITCODE -ne 0) { throw "fetch of $name failed" }
    git -C $dir -c advice.detachedHead=false checkout -q FETCH_HEAD
    Write-Host "$name $commit"
    return $dir
}

$injector = Fetch 'injector' 'https://github.com/ThirteenAG/injector' '4af17c35774d08c85aeabb64a164bdc7c4ead5dc'
Fetch 'injector\safetyhook' 'https://github.com/cursey/safetyhook' '19223663fb8a573253ffb2e82da87cc354bf5c16' | Out-Null
Fetch 'hooking' 'https://github.com/ThirteenAG/Hooking.Patterns' '460a47bc813085b66c79768531caab1ed5ea96fc' | Out-Null
