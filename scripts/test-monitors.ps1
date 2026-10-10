Set-StrictMode -Version Latest

# Monitor discovery and selection for tests that need the visible desktop.
# Tests read the chosen display from EASNAP_TEST_MONITOR (\\.\DISPLAYn).

function Get-EasnapTestMonitors {
    Add-Type -AssemblyName System.Windows.Forms
    if (-not ('Easnap.DisplayDevices' -as [type])) {
        Add-Type -Namespace Easnap -Name DisplayDevices -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DISPLAY_DEVICE {
    public int cb;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
    public int StateFlags;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
}
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern bool EnumDisplayDevices(string device, uint index, ref DISPLAY_DEVICE info, uint flags);
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DEVMODE {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
    public short dmSpecVersion; public short dmDriverVersion; public short dmSize; public short dmDriverExtra;
    public int dmFields; public int dmPositionX; public int dmPositionY; public int dmDisplayOrientation;
    public int dmDisplayFixedOutput; public short dmColor; public short dmDuplex; public short dmYResolution;
    public short dmTTOption; public short dmCollate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
    public short dmLogPixels; public int dmBitsPerPel; public int dmPelsWidth; public int dmPelsHeight;
    public int dmDisplayFlags; public int dmDisplayFrequency;
    public int dmICMMethod; public int dmICMIntent; public int dmMediaType; public int dmDitherType;
    public int dmReserved1; public int dmReserved2; public int dmPanningWidth; public int dmPanningHeight;
}
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern bool EnumDisplaySettings(string device, int mode, ref DEVMODE settings);
[DllImport("shcore.dll")]
public static extern int GetDpiForMonitor(IntPtr monitor, int type, out uint dpiX, out uint dpiY);
[DllImport("user32.dll")]
public static extern IntPtr MonitorFromPoint(System.Drawing.Point point, uint flags);
'@ -ReferencedAssemblies System.Drawing
    }
    # EDID names (ASUS, SAMSUNG, ...) keyed by the hardware id that both WMI
    # and EnumDisplayDevices report, e.g. DISPLAY\SAM0F9C\... / MONITOR\SAM0F9C\...
    $friendly = @{}
    try {
        foreach ($entry in Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction Stop) {
            $hardware = ($entry.InstanceName -split '\\')[1]
            $name = -join ($entry.UserFriendlyName | Where-Object { $_ } | ForEach-Object { [char]$_ })
            $maker = -join ($entry.ManufacturerName | Where-Object { $_ } | ForEach-Object { [char]$_ })
            if ($hardware) { $friendly[$hardware] = if ($name) { $name } else { $maker } }
        }
    } catch { }  # Names are a convenience; selection by number or device still works.
    $index = 0
    foreach ($screen in [System.Windows.Forms.Screen]::AllScreens | Sort-Object { -not $_.Primary }, DeviceName) {
        $index++
        $device = New-Object Easnap.DisplayDevices+DISPLAY_DEVICE
        $device.cb = [Runtime.InteropServices.Marshal]::SizeOf($device)
        $label = ''
        if ([Easnap.DisplayDevices]::EnumDisplayDevices($screen.DeviceName, 0, [ref]$device, 0)) {
            $hardware = ($device.DeviceID -split '\\')[1]
            $label = if ($hardware -and $friendly.ContainsKey($hardware)) { $friendly[$hardware] } else { $device.DeviceString }
        }
        $mode = New-Object Easnap.DisplayDevices+DEVMODE
        $mode.dmSize = [int16][Runtime.InteropServices.Marshal]::SizeOf($mode)
        $hasMode = [Easnap.DisplayDevices]::EnumDisplaySettings($screen.DeviceName, -1, [ref]$mode)
        $refresh = if ($hasMode) { $mode.dmDisplayFrequency } else { 0 }
        $dpi = [uint32]96; $dpiY = [uint32]96
        $centre = New-Object System.Drawing.Point(($screen.Bounds.Left + $screen.Bounds.Width / 2), ($screen.Bounds.Top + $screen.Bounds.Height / 2))
        [void][Easnap.DisplayDevices]::GetDpiForMonitor([Easnap.DisplayDevices]::MonitorFromPoint($centre, 2), 0, [ref]$dpi, [ref]$dpiY)
        # A DPI-unaware PowerShell host sees scaled bounds and 96 DPI. The display
        # mode holds the physical size, and their ratio is the real scale.
        $width = if ($hasMode) { $mode.dmPelsWidth } else { $screen.Bounds.Width }
        $height = if ($hasMode) { $mode.dmPelsHeight } else { $screen.Bounds.Height }
        $scale = [Math]::Max($dpi * 100 / 96, $width * 100 / [Math]::Max(1, $screen.Bounds.Width))
        [PSCustomObject]@{
            Index = $index; Device = $screen.DeviceName; Name = $label; Primary = $screen.Primary
            Width = $width; Height = $height
            RefreshHz = $refresh; ScalePercent = [int][Math]::Round($scale)
        }
    }
}

function Format-EasnapTestMonitor($Monitor) {
    '{0}. {1,-18} {2}  {3}x{4} @ {5} Hz, {6}%{7}' -f $Monitor.Index, $Monitor.Name, $Monitor.Device,
        $Monitor.Width, $Monitor.Height, $Monitor.RefreshHz, $Monitor.ScalePercent,
        $(if ($Monitor.Primary) { ', primary' } else { '' })
}

# Resolves -Monitor: a list number, a \\.\DISPLAYn device, part of a monitor
# name (for example SAMSUNG), 'Primary', or 'Ask'. Ambiguous or missing
# selections are errors; a test must never silently run on another display.
function Resolve-EasnapTestMonitor([object[]]$Monitors, [string]$Selection, [bool]$CanPrompt) {
    if (-not $Monitors.Count) { throw 'No active display was found.' }
    if (-not $Selection -or $Selection -eq 'Ask') {
        if ($Monitors.Count -eq 1) { return $Monitors[0] }
        if (-not $CanPrompt) {
            throw ("Several displays are active and this run cannot prompt. Pass -Monitor <number|name|device>:`n" +
                (($Monitors | ForEach-Object { '  ' + (Format-EasnapTestMonitor $_) }) -join "`n"))
        }
        Write-Host 'Desktop tests need one display. Choose where they run:' -ForegroundColor Cyan
        $Monitors | ForEach-Object { Write-Host ('  ' + (Format-EasnapTestMonitor $_)) }
        $Selection = Read-Host 'Display number or name'
    }
    if ($Selection -eq 'Primary') { return @($Monitors | Where-Object Primary)[0] }
    $number = 0
    if ([int]::TryParse($Selection, [ref]$number)) {
        $match = @($Monitors | Where-Object Index -eq $number)
    } else {
        $match = @($Monitors | Where-Object { $_.Device -ieq $Selection })
        if (-not $match.Count) { $match = @($Monitors | Where-Object { $_.Name -like "*$Selection*" }) }
    }
    if ($match.Count -ne 1) {
        throw ("'$Selection' matches $($match.Count) displays. Available:`n" +
            (($Monitors | ForEach-Object { '  ' + (Format-EasnapTestMonitor $_) }) -join "`n"))
    }
    return $match[0]
}
