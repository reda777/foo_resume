param (
    [string]$TargetName = "foo_resume",
    [string]$TargetFileName = "foo_resume.dll",
    [string]$Platform = "x64",
    [string]$OutputPath = "" # This is $(OutDir) from VS
)

$ErrorActionPreference = 'Stop'
$OutputPath = $OutputPath.Trim().Trim('"')

# Define the package path (Sibling 'out' folder)
$PackagePath = "..\out\$TargetName"

Write-Host "Creating directory structure in $PackagePath..."
# Create the root (for x86) and the x64 subfolder
$null = New-Item -Path "..\out\" -Name "$TargetName\x64" -ItemType 'directory' -Force

# Copy the DLL to the correct spot
if ($Platform -eq 'x64') {
    # 64-bit goes into \x64 subfolder
    $Dest = "$PackagePath\x64"
    Write-Host "Copying x64 DLL to $Dest"
    Copy-Item "$OutputPath$TargetFileName" -Destination $Dest -Force
}
else {
    # 32-bit (Win32) goes into the root
    $Dest = "$PackagePath"
    Write-Host "Copying x86 DLL to $Dest"
    Copy-Item "$OutputPath$TargetFileName" -Destination $Dest -Force
}

# Create the Archive
Write-Host "Creating archive $TargetName.fb2k-component..."
Compress-Archive -Force -Path "..\out\$TargetName\*" -DestinationPath "..\out\$TargetName.fb2k-component"

Write-Host "Package complete at ..\out\$TargetName.fb2k-component" -ForegroundColor Green