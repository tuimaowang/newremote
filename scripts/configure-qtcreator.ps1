param([string]$QtRoot = 'C:\Qt\6.11.1\msvc2022_64')
$ErrorActionPreference = 'Stop'
if (Get-Process qtcreator -ErrorAction SilentlyContinue) { throw 'Close Qt Creator before updating its settings.' }
$root = Split-Path -Parent $PSScriptRoot
$settings = Join-Path $env:APPDATA 'QtProject\qtcreator'
$projectPath = Join-Path $root '.qtcreator\CMakeLists.txt.user'
$profilesPath = Join-Path $settings 'profiles.xml'
$versionsPath = Join-Path $settings 'qtversion.xml'
$qmake = Join-Path $QtRoot 'bin\qmake.exe'
if (-not (Test-Path -LiteralPath $qmake)) { throw "Missing qmake: $qmake" }
[xml]$project = Get-Content -LiteralPath $projectPath
[xml]$profiles = Get-Content -LiteralPath $profilesPath
[xml]$versions = Get-Content -LiteralPath $versionsPath

function Set-Value($Parent, [string]$Key, [string]$Text, [string]$Type = 'QString') {
    $node = $Parent.SelectSingleNode("value[@key='$Key']")
    if (-not $node) {
        $node = $Parent.OwnerDocument.CreateElement('value')
        $node.SetAttribute('key', $Key)
        [void]$Parent.AppendChild($node)
    }
    $node.SetAttribute('type', $Type)
    $node.InnerText = $Text
}

function Add-DataMap($Document, [string]$Name) {
    $data = $Document.CreateElement('data')
    $variable = $Document.CreateElement('variable')
    $variable.InnerText = $Name
    [void]$data.AppendChild($variable)
    $map = $Document.CreateElement('valuemap')
    $map.SetAttribute('type', 'QVariantMap')
    [void]$data.AppendChild($map)
    [void]$Document.DocumentElement.AppendChild($data)
    return $map
}

$qtPath = $qmake.Replace('\', '/')
$qtMap = $versions.SelectSingleNode("//valuemap[value[@key='QMakePath']='$qtPath']")
if (-not $qtMap) {
    $ids = @($versions.SelectNodes('//value[@key="Id"]') | ForEach-Object { [int]$_.InnerText })
    $qtId = [int]($ids | Measure-Object -Maximum).Maximum + 100
    $qtIndex = ($versions.SelectNodes('/qtcreator/data/variable') | Where-Object InnerText -Match '^QtVersion\.\d+$' | ForEach-Object { [int]($_.InnerText.Split('.')[1]) } | Measure-Object -Maximum).Maximum + 1
    $qtMap = Add-DataMap $versions "QtVersion.$qtIndex"
    Set-Value $qtMap 'Id' "$qtId" 'int'
    Set-Value $qtMap 'Name' 'FSRemote Qt 6.11.1 MSVC 2022 x64'
    Set-Value $qtMap 'QMakePath' $qtPath
    Set-Value $qtMap 'QtVersion.Type' 'Qt4ProjectManager.QtVersion.Desktop'
    Set-Value $qtMap 'isAutodetected' 'false' 'bool'
} else { $qtId = [int]$qtMap.SelectSingleNode('value[@key="Id"]').InnerText }

$kitId = 'fsremote.local.msvc2022.x64'
$kit = $profiles.SelectSingleNode("//valuemap[value[@key='PE.Profile.Id']='$kitId']")
if (-not $kit) {
    $source = $profiles.SelectSingleNode("//valuemap[value[@key='PE.Profile.Id']='qt.qt6.6111.win64_msvc2022_64_kit']")
    if (-not $source) { throw 'The installed MSVC 2022 x64 kit was not found.' }
    $index = ($profiles.SelectNodes('/qtcreator/data/variable') | Where-Object InnerText -Match '^Profile\.\d+$' | ForEach-Object { [int]($_.InnerText.Split('.')[1]) } | Measure-Object -Maximum).Maximum + 1
    $placeholder = Add-DataMap $profiles "Profile.$index"
    $kit = $source.CloneNode($true)
    [void]$placeholder.ParentNode.ReplaceChild($kit, $placeholder)
}
Set-Value $kit 'PE.Profile.Id' $kitId
Set-Value $kit 'PE.Profile.Name' 'FSRemote Qt 6.11.1 MSVC 2022 x64'
Set-Value $kit 'PE.Profile.AutoDetected' 'false' 'bool'
Set-Value $kit 'PE.Profile.SDK' 'false' 'bool'
foreach ($node in @($kit.SelectNodes('valuelist[@key="PE.Profile.StickyInfo"]/value'))) { [void]$node.ParentNode.RemoveChild($node) }
$kitData = $kit.SelectSingleNode('valuemap[@key="PE.Profile.Data"]')
Set-Value $kitData 'QtSupport.QtInformation' "$qtId" 'int'
$generator = $kitData.SelectSingleNode('valuemap[@key="CMake.GeneratorKitInformation"]')
Set-Value $generator 'Generator' 'Visual Studio 17 2022'
Set-Value $generator 'Platform' 'x64'
$count = $profiles.SelectSingleNode('/qtcreator/data[variable="Profile.Count"]/value')
if ($count) { $count.InnerText = [string]$profiles.SelectNodes('/qtcreator/data[starts-with(variable,"Profile.")]/valuemap').Count }

$target = $project.SelectSingleNode('/qtcreator/data[variable="ProjectExplorer.Project.Target.0"]/valuemap')
Set-Value $target 'ProjectExplorer.ProjectConfiguration.Id' $kitId
Set-Value $target 'ProjectExplorer.ProjectConfiguration.DisplayName' 'FSRemote Qt 6.11.1 MSVC 2022 x64'
Set-Value $target 'ProjectExplorer.ProjectConfiguration.DefaultDisplayName' 'FSRemote Qt 6.11.1 MSVC 2022 x64'
foreach ($node in @($project.SelectNodes('//value[@key="ProjectExplorer.Target.ActiveDeployConfiguration"]'))) { $node.InnerText = '0' }
foreach ($node in @($project.SelectNodes('//valuemap[@key="ProjectExplorer.Target.DeployConfiguration.1"]'))) { [void]$node.ParentNode.RemoveChild($node) }
foreach ($node in $project.SelectNodes('//value[@key="ProjectExplorer.Target.DeployConfigurationCount"]')) { $node.InnerText = '1' }
$buildDir = (Join-Path $root 'build\qtcreator-msvc').Replace('\', '/')
foreach ($build in $target.SelectNodes('valuemap[starts-with(@key,"ProjectExplorer.Target.BuildConfiguration.")]')) {
    $type = $build.SelectSingleNode('value[@key="CMake.Build.Type"]').InnerText
    Set-Value $build 'ProjectExplorer.BuildConfiguration.BuildDirectory' $buildDir
    Set-Value $build 'CMake.Initial.Parameters' "-DCMAKE_GENERATOR:STRING=Visual Studio 17 2022`n-DCMAKE_GENERATOR_PLATFORM:STRING=x64`n-DCMAKE_PREFIX_PATH:PATH=$($QtRoot.Replace('\','/'))"
    $step = $build.SelectSingleNode('valuemap[@key="ProjectExplorer.BuildConfiguration.BuildStepList.0"]/valuemap')
    $step.SelectSingleNode('valuelist[@key="CMakeProjectManager.MakeStep.BuildTargets"]/value').InnerText = 'ALL_BUILD'
}

# Replace empty custom executables with native CMake target configurations.
$owners = @($target) + @($target.SelectNodes('valuemap[starts-with(@key,"ProjectExplorer.Target.BuildConfiguration.")]'))
foreach ($owner in $owners) {
    $template = $owner.SelectSingleNode('valuemap[@key="ProjectExplorer.Target.RunConfiguration.0"]').CloneNode($true)
    foreach ($node in @($owner.SelectNodes('valuemap[starts-with(@key,"ProjectExplorer.Target.RunConfiguration.")]'))) { [void]$owner.RemoveChild($node) }
    $i = 0
    foreach ($name in @('FSRemoteMessages', 'FSRemoteMessageServer', 'message_tests')) {
        $run = $template.CloneNode($true)
        $run.SetAttribute('key', "ProjectExplorer.Target.RunConfiguration.$i")
        Set-Value $run 'ProjectExplorer.ProjectConfiguration.Id' 'CMakeProjectManager.CMakeRunConfiguration.'
        Set-Value $run 'ProjectExplorer.ProjectConfiguration.DisplayName' $name
        Set-Value $run 'ProjectExplorer.RunConfiguration.BuildKey' $name
        Set-Value $run 'RunConfiguration.UseLibrarySearchPath' 'true' 'bool'
        Set-Value $run 'RunConfiguration.UseTerminal' 'false' 'bool'
        $environment = $run.SelectSingleNode('valuelist[@key="PE.EnvironmentAspect.Changes"]')
        $environment.RemoveAll()
        $environment.SetAttribute('type', 'QVariantList')
        $environment.SetAttribute('key', 'PE.EnvironmentAspect.Changes')
        $path = $project.CreateElement('value')
        $path.SetAttribute('type', 'QString')
        $path.InnerText = "PATH=$($QtRoot.Replace('\','/'))/bin;`%{Env:PATH}"
        [void]$environment.AppendChild($path)
        [void]$owner.AppendChild($run)
        $i++
    }
    Set-Value $owner 'ProjectExplorer.Target.RunConfigurationCount' '3' 'qlonglong'
    Set-Value $owner 'ProjectExplorer.Target.ActiveRunConfiguration' '0' 'qlonglong'
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
foreach ($entry in @(@($versionsPath, $versions), @($profilesPath, $profiles), @($projectPath, $project))) {
    Copy-Item -LiteralPath $entry[0] -Destination "$($entry[0]).backup-$stamp"
    $entry[1].Save($entry[0])
}
Write-Host "Configured $projectPath"
Write-Host "Kit: $kitId; Qt: $qtPath; Debug/Release: $buildDir"
