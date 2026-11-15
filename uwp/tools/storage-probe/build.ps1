# Run in an x64 VS 2022 developer shell with UWP C++ tools and SDK 10.0.19041.0.
$ErrorActionPreference = 'Stop'
function Exec([scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "Command failed with exit code ${LASTEXITCODE}: $Command" }
}
Push-Location $PSScriptRoot
try {
    Exec { cmake -S . -B build -G 'Visual Studio 17 2022' -A x64 }
    $cert = New-SelfSignedCertificate -Type Custom -Subject 'CN=RingRacersStorageProbe' `
        -KeyUsage DigitalSignature -FriendlyName 'Storage Probe UWP CI' `
        -CertStoreLocation 'Cert:\CurrentUser\My' `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
    $password = ConvertTo-SecureString -String 'ci' -Force -AsPlainText
    Export-PfxCertificate -Cert $cert -FilePath build\ci.pfx -Password $password | Out-Null
    New-Item -ItemType Directory -Force out | Out-Null
    Export-Certificate -Cert $cert -FilePath out\storage-probe.cer | Out-Null
    Exec {
        msbuild build\storage-probe.vcxproj /p:Configuration=Release /p:Platform=x64 `
            /p:GenerateAppxPackageOnBuild=true /p:UapAppxPackageBuildMode=SideloadOnly /p:AppxBundle=Never `
            /p:AppxPackageSigningEnabled=true /p:PackageCertificateKeyFile="$PWD\build\ci.pfx" /p:PackageCertificatePassword=ci `
            /p:AppxPackageDir="$PWD\build\AppPackages\"
    }
    $package = @(Get-ChildItem build\AppPackages -Recurse -Filter 'storage-probe*.msix')
    if ($package.Count -ne 1) { throw 'Expected one storage-probe MSIX' }
    Copy-Item $package[0].FullName out
    $archive = [System.IO.Compression.ZipFile]::OpenRead($package[0].FullName)
    try {
        foreach ($name in @('AppxSignature.p7x', 'storage-probe.exe', 'AppxManifest.xml')) {
            if ($null -eq $archive.GetEntry($name)) { throw "Package is missing $name" }
        }
    } finally { $archive.Dispose() }
} finally { Pop-Location }
