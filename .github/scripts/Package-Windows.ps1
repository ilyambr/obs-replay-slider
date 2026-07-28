[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string] $Target = 'x64',
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release', 'MinSizeRel')]
    [string] $Configuration = 'RelWithDebInfo'
)

$ErrorActionPreference = 'Stop'

if ( $DebugPreference -eq 'Continue' ) {
    $VerbosePreference = 'Continue'
    $InformationPreference = 'Continue'
}

if ( $env:CI -eq $null ) {
    throw "Package-Windows.ps1 requires CI environment"
}

if ( ! ( [System.Environment]::Is64BitOperatingSystem ) ) {
    throw "Packaging script requires a 64-bit system to build and run."
}

if ( $PSVersionTable.PSVersion -lt '7.2.0' ) {
    Write-Warning 'The packaging script requires PowerShell Core 7. Install or upgrade your PowerShell version: https://aka.ms/pscore6'
    exit 2
}

function Package {
    trap {
        Write-Error $_
        exit 2
    }

    $ScriptHome = $PSScriptRoot
    $ProjectRoot = Resolve-Path -Path "$PSScriptRoot/../.."
    $BuildSpecFile = "${ProjectRoot}/buildspec.json"

    $UtilityFunctions = Get-ChildItem -Path $PSScriptRoot/utils.pwsh/*.ps1 -Recurse

    foreach( $Utility in $UtilityFunctions ) {
        Write-Debug "Loading $($Utility.FullName)"
        . $Utility.FullName
    }

    $BuildSpec = Get-Content -Path ${BuildSpecFile} -Raw | ConvertFrom-Json
    $ProductName = $BuildSpec.name
    $ProductVersion = $BuildSpec.version

    $OutputName = "${ProductName}-${ProductVersion}-windows-${Target}"

    $RemoveArgs = @{
        ErrorAction = 'SilentlyContinue'
        Path = @(
            "${ProjectRoot}/release/${ProductName}-*-windows-*.zip"
        )
    }

    Remove-Item @RemoveArgs

    Log-Group "Archiving ${ProductName}..."
    # The CMake build produces the modern self-contained plugin layout
    # (${ProductName}\bin\64bit\${ProductName}.dll, ${ProductName}\data\*).
    # Remap it to the classic flat layout (obs-plugins/64bit, data/obs-plugins/<name>)
    # so the zip can be extracted directly into a "Program Files\obs-studio"-style
    # install, matching real OBS's own layout and every other installed plugin.
    $StagingDir = "${ProjectRoot}/release/${OutputName}-staging"
    Remove-Item -Path $StagingDir -Recurse -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Path "${StagingDir}/obs-plugins/64bit" -Force | Out-Null
    New-Item -ItemType Directory -Path "${StagingDir}/data/obs-plugins/${ProductName}" -Force | Out-Null

    $BuiltPluginDir = "${ProjectRoot}/release/${Configuration}/${ProductName}"
    Copy-Item -Path "${BuiltPluginDir}/bin/64bit/*" -Destination "${StagingDir}/obs-plugins/64bit" -Recurse
    Copy-Item -Path "${BuiltPluginDir}/data/*" -Destination "${StagingDir}/data/obs-plugins/${ProductName}" -Recurse

    $CompressArgs = @{
        Path = (Get-ChildItem -Path $StagingDir)
        CompressionLevel = 'Optimal'
        DestinationPath = "${ProjectRoot}/release/${OutputName}.zip"
        Verbose = ($Env:CI -ne $null)
    }
    Compress-Archive -Force @CompressArgs
    Remove-Item -Path $StagingDir -Recurse -ErrorAction SilentlyContinue
    Log-Group

    $IsccPath = 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'
    if ( ( Test-Path -Path $IsccPath ) -and ( Test-Path -Path "${ProjectRoot}/installer.iss" ) ) {
        Log-Group "Building installer for ${ProductName}..."
        & $IsccPath /Qp "/DMyAppVersion=${ProductVersion}" "/DConfigDir=${Configuration}" "${ProjectRoot}/installer.iss"
        if ( $LASTEXITCODE -ne 0 ) {
            throw "ISCC.exe failed with exit code ${LASTEXITCODE}"
        }
        Log-Group
    } else {
        Write-Warning "Inno Setup or installer.iss not found -- skipping installer creation."
    }
}

Package
