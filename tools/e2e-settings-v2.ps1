# UI smoke for the v2 settings (items 6 to 10): opens the General page, flips each new switch, reorders the rest line with
# the buttons, and checks settings.json after each step. Also checks every control has an accessible name.
# Needs a desktop, so it is not part of build.ps1 -Ci. It publishes to a temp folder and uses a temp STATLET_DATA_DIR, so it
# never touches your installed copy or %LOCALAPPDATA%\Statlet\settings.json.
#   .\tools\e2e-settings-v2.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
$root = Split-Path $PSScriptRoot
$dir = Join-Path ([IO.Path]::GetTempPath()) "statlet-e2e-v2-$PID"
$data = Join-Path $dir 'data'
New-Item -ItemType Directory -Force $data | Out-Null
dotnet publish "$root\settings\StatletSettings.csproj" -c Release -r win-x64 -o "$dir\settings" | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'publish failed' }
$exe = "$dir\settings\StatletSettings.exe"
$AE = [System.Windows.Automation.AutomationElement]
$CT = [System.Windows.Automation.ControlType]
$script:failed = 0
function Check($ok, $what) { if ($ok) { Write-Host "ok   $what" -ForegroundColor Green } else { Write-Host "FAIL $what" -ForegroundColor Red; $script:failed++ } }
function ByName($win, $name) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $name)
    $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $cond)
}
# The row title text and its switch share a name: pick the one that supports the pattern (toggle or invoke).
function ByNameWith($win, $name, $availableProp) {
    $both = New-Object System.Windows.Automation.AndCondition(
        (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $name)),
        (New-Object System.Windows.Automation.PropertyCondition($availableProp, $true)))
    $win.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $both)
}
function Json { Get-Content (Join-Path $data 'settings.json') -Raw | ConvertFrom-Json }
function WaitJson($test) { for ($i = 0; $i -lt 30; $i++) { Start-Sleep -Milliseconds 100; if (Test-Path (Join-Path $data 'settings.json')) { try { if (& $test (Json)) { return $true } } catch {} } }; $false }

$env:STATLET_DATA_DIR = $data
$proc = Start-Process $exe -PassThru
Remove-Item Env:STATLET_DATA_DIR
try {
    $win = $null
    for ($i = 0; $i -lt 60 -and -not $win; $i++) {
        Start-Sleep -Milliseconds 250
        $win = $AE::RootElement.FindFirst([System.Windows.Automation.TreeScope]::Children, (New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, 'Statlet Settings')))
    }
    if (-not $win) { throw 'the settings window did not open' }
    Start-Sleep 1

    # Every new control has an accessible name.
    $names = 'Show weekday', 'Bluetooth icon', 'Download icon', 'Rest line order', 'Move up', 'Move down',
             'Sound when a screenshot is taken', 'Sound when a download finishes', 'Sound when the focus timer finishes'
    foreach ($n in $names) { Check ($null -ne (ByName $win $n)) "control named '$n' exists" }

    # Each switch flips and lands in settings.json under the key the island reads.
    $switches = @(
        @('Bluetooth icon', 'btIcon', $false), @('Download icon', 'downloadIcon', $false),
        @('Sound when a screenshot is taken', 'soundScreenshot', $false), @('Sound when a download finishes', 'soundDownloadDone', $true),
        @('Sound when the focus timer finishes', 'soundFocusDone', $false), @('Show weekday', 'showWeekday', $false))
    foreach ($s in $switches) {
        $el = ByNameWith $win $s[0] $AE::IsTogglePatternAvailableProperty
        if (-not $el) { Check $false "'$($s[0])' is a switch"; continue }
        $el.GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
        $key = $s[1]; $want = $s[2]
        Check (WaitJson { param($j) $j.$key -eq $want }) "'$($s[0])' writes $key=$want"
    }

    # Reorder with the buttons: select 'CPU and memory bars' (last), move it up twice, then the status icon down once.
    $list = ByName $win 'Rest line order'
    $items = $list.FindAll([System.Windows.Automation.TreeScope]::Descendants, (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::ListItem)))
    Check ($items.Count -eq 3) 'the order list has three rows'
    function Pick($label) {
        $it = $items | Where-Object { $_.Current.Name -like "*$label*" } | Select-Object -First 1
        $it.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
        Start-Sleep -Milliseconds 200
    }
    function Press($name) { (ByNameWith $win $name $AE::IsInvokePatternAvailableProperty).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke(); Start-Sleep -Milliseconds 250 }
    Check ((Json).restOrder -eq 'status,clock,usage') 'order starts at the default'
    Pick 'CPU and memory'; Press 'Move up'; Press 'Move up'
    Check (WaitJson { param($j) $j.restOrder -eq 'usage,status,clock' }) 'two moves up: usage,status,clock'
    Pick 'Status icon'; Press 'Move down'
    Check (WaitJson { param($j) $j.restOrder -eq 'usage,clock,status' }) 'status down once: usage,clock,status'
    Check (-not (ByNameWith $win 'Move down' $AE::IsInvokePatternAvailableProperty).Current.IsEnabled) 'Move down is disabled on the last row'
} finally {
    Get-Process StatletSettings -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe } | Stop-Process -Force
    Start-Sleep -Milliseconds 300
    Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
}
if ($script:failed) { Write-Host "$script:failed check(s) failed" -ForegroundColor Red; exit 1 }
Write-Host 'settings v2 UI ok' -ForegroundColor Green
