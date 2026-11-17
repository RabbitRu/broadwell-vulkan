param([int]$Frames = 600, [int[]]$CaptureAfterMs = @(2500, 4000), [string]$OutputDirectory, [int]$DeadlineSeconds = 120,
    [string[]]$ExtraArguments = @(), [int]$ExerciseSeconds = 0, [int]$ExerciseIntervalMs = 1500,
    [switch]$Validate, [string]$LayerPath = 'C:\VulkanSDK-local\Bin',
    [ValidateSet('redirect', 'gdi')][string]$Present = 'redirect')
# -Present gdi: force the old CPU-copy/GDI presentation (HASVK_WDDM_PRESENT=gdi).
# The display is kept on during the run: while it sleeps every present is
# "occluded" and the ICD falls back to GDI.
# -Validate: run under VK_LAYER_KHRONOS_validation; messages go to validation.log.
# -ExerciseSeconds N: run for N seconds with periodic resize/minimize/restore and
# finish with WM_CLOSE (pass a large -Frames so vkcube does not stop by itself).
# Launches the unmodified vkcube through the local ICD and saves captures of its
# client area (PrintWindow, so the window does not have to be on top).
# VK_DRIVER_FILES is set for the child process only. The run is unattended: any
# dialog owned by vkcube is recorded, the process is terminated and the run fails.
$ErrorActionPreference='Stop'
$taskRoot=Split-Path $PSScriptRoot -Parent
$taskPackage=Join-Path $taskRoot 'build\icd'
if (-not $OutputDirectory) { $OutputDirectory=Join-Path $taskRoot 'build\vkcube-capture' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.Text; using System.Drawing; using System.Runtime.InteropServices;
public static class CubeWindow {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int hgt, uint flags);
  [DllImport("user32.dll")] static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("kernel32.dll")] static extern uint SetThreadExecutionState(uint flags);
  public static void KeepDisplayOn(bool on){ SetThreadExecutionState(on ? 0x80000003u /* CONTINUOUS|SYSTEM|DISPLAY */ : 0x80000000u); }
  // Moved to the screen origin: GDI presentation only paints the on-screen part of a window.
  public static void Resize(IntPtr h, int w, int hgt){ SetWindowPos(h, IntPtr.Zero, 0, 0, w, hgt, 0x14 /* NOZORDER|NOACTIVATE */); }
  public static void Show(IntPtr h, int cmd){ ShowWindow(h, cmd); }
  public static void Close(IntPtr h){ PostMessage(h, 0x10 /* WM_CLOSE */, IntPtr.Zero, IntPtr.Zero); }
  static string Text(IntPtr h){ var s=new StringBuilder(4096); GetWindowText(h,s,4096); return s.ToString(); }
  static string Class(IntPtr h){ var s=new StringBuilder(256); GetClassName(h,s,256); return s.ToString(); }
  // Text of a dialog (#32770) owned by the process, or null.
  public static string Dialog(uint pid){
    string found=null;
    EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h,out p);
      if(p==pid && Class(h)=="#32770"){ var b=new StringBuilder(Text(h));
        EnumChildWindows(h,(c,l2)=>{ b.Append(" | ").Append(Text(c)); return true; },IntPtr.Zero); found=b.ToString(); }
      return true; },IntPtr.Zero);
    return found; }
  public static IntPtr TopWindow(uint pid){
    IntPtr found=IntPtr.Zero;
    EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h,out p);
      if(p==pid && IsWindowVisible(h) && Class(h)!="#32770" && Text(h).Length>0) found=h; return true; },IntPtr.Zero);
    return found; }
  public static Bitmap Capture(IntPtr h){
    RECT r; GetClientRect(h,out r); if(r.R<=0||r.B<=0) return null;
    var b=new Bitmap(r.R,r.B); using(var g=Graphics.FromImage(b)){ IntPtr dc=g.GetHdc();
      bool ok=PrintWindow(h,dc,3 /* PW_CLIENTONLY | PW_RENDERFULLCONTENT */); g.ReleaseHdc(dc); if(!ok){ b.Dispose(); return null; } }
    return b; }
}
'@
$taskStart=[Diagnostics.ProcessStartInfo]::new()
$taskStart.FileName=Join-Path $taskRoot 'build\vkcube\vkcube.exe'
$taskStart.Arguments=(@('--c', "$Frames", '--suppress_popups') + $ExtraArguments) -join ' '
$taskStart.WorkingDirectory=$taskPackage; $taskStart.UseShellExecute=$false
$taskStart.RedirectStandardError=$true
$taskStart.EnvironmentVariables['VK_DRIVER_FILES']=Join-Path $taskPackage 'intel_hasvk_wddm_icd.json'
$taskStart.EnvironmentVariables.Remove('VK_ICD_FILENAMES'); $taskStart.EnvironmentVariables.Remove('VK_ADD_DRIVER_FILES')
$taskStart.EnvironmentVariables['VK_LOADER_LAYERS_DISABLE']='~implicit~'
$taskStart.EnvironmentVariables['PATH']=$taskPackage+';'+$env:PATH
if ($Present -eq 'gdi') { $taskStart.EnvironmentVariables['HASVK_WDDM_PRESENT']='gdi' } else { $taskStart.EnvironmentVariables.Remove('HASVK_WDDM_PRESENT') }
$taskStart.EnvironmentVariables['HASVK_WDDM_PRESENT_STATS']='1'
$taskStart.EnvironmentVariables['HASVK_WDDM_ASSERTS_TO_STDERR']='1'   # no assert message boxes from the ICD
[CubeWindow]::KeepDisplayOn($true); Start-Sleep -Milliseconds 1000
if ($Validate) {
    if (-not (Test-Path -LiteralPath (Join-Path $LayerPath 'VkLayer_khronos_validation.json'))) { throw "Validation layer not found in $LayerPath" }
    $taskValidationLog=Join-Path (Resolve-Path -LiteralPath $OutputDirectory).Path 'validation.log'
    if (Test-Path -LiteralPath $taskValidationLog) { Remove-Item -LiteralPath $taskValidationLog -Confirm:$false }
    $taskStart.EnvironmentVariables['VK_LAYER_PATH']=$LayerPath
    $taskStart.EnvironmentVariables['VK_INSTANCE_LAYERS']='VK_LAYER_KHRONOS_validation'
    $taskStart.EnvironmentVariables['VK_KHRONOS_VALIDATION_DEBUG_ACTION']='VK_DBG_LAYER_ACTION_LOG_MSG'
    $taskStart.EnvironmentVariables['VK_KHRONOS_VALIDATION_LOG_FILENAME']=$taskValidationLog
}
$taskProcess=[Diagnostics.Process]::Start($taskStart)
$taskId=[uint32]$taskProcess.Id
$taskErrors=$taskProcess.StandardError.ReadToEndAsync()
$taskWatch=[Diagnostics.Stopwatch]::StartNew()
$taskCaptures=@()
function Wait-Cube([long]$UntilMs) {
    while ($taskWatch.ElapsedMilliseconds -lt $UntilMs -and -not $taskProcess.HasExited) {
        $dialog=[CubeWindow]::Dialog($taskId)
        if ($dialog) { $taskProcess.Kill(); $taskProcess.WaitForExit(); throw "vkcube opened a dialog (closed automatically): $dialog" }
        Start-Sleep -Milliseconds 100
    }
}
function Save-Capture([string]$Name, [string]$Action, [bool]$Keep) {
    $bitmap=[CubeWindow]::Capture([CubeWindow]::TopWindow($taskId))
    if (-not $bitmap) { throw "vkcube window capture failed at $Name" }
    $path=$null
    if ($Keep) { $path=Join-Path $OutputDirectory ($Name+'.png'); $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png) }
    $colors=@{}
    $stepX=[Math]::Max(1,[int]($bitmap.Width/64)); $stepY=[Math]::Max(1,[int]($bitmap.Height/64))
    for ($y=0; $y -lt $bitmap.Height; $y+=$stepY) { for ($x=0; $x -lt $bitmap.Width; $x+=$stepX) { $colors[$bitmap.GetPixel($x,$y).ToArgb()]=1 } }
    # Pixels the swapchain never painted keep the window class background (white).
    $unpainted=0; foreach ($y in @(0, ($bitmap.Height-1))) { for ($x=0; $x -lt $bitmap.Width; $x+=$stepX) { if (($bitmap.GetPixel($x,$y).ToArgb() -band 0xffffff) -eq 0xffffff) { ++$unpainted } } }
    $entry=[ordered]@{name=$Name; action=$Action; file=$path; width=$bitmap.Width; height=$bitmap.Height; elapsedMs=$taskWatch.ElapsedMilliseconds; distinctSampledColors=$colors.Count; unpaintedEdgeSamples=$unpainted}
    $bitmap.Dispose()
    $entry
}
try {
    if ($ExerciseSeconds -gt 0) {
        # Resize/minimize/restore cycle without activating the window, then WM_CLOSE.
        Wait-Cube 2500
        $taskSizes=@(@(500,500),@(800,600),@(320,240),@(1024,768),@(640,640),@(200,150))
        $taskStep=0; $taskResize=0
        while ($taskWatch.Elapsed.TotalSeconds -lt $ExerciseSeconds -and -not $taskProcess.HasExited) {
            $taskWindow=[CubeWindow]::TopWindow($taskId)
            if ($taskWindow -eq [IntPtr]::Zero) { throw 'vkcube window not found' }
            $size=$taskSizes[$taskResize % $taskSizes.Count]
            if ($taskStep % 3 -eq 2) {
                [CubeWindow]::Show($taskWindow, 7); Wait-Cube ($taskWatch.ElapsedMilliseconds+1500)   # SW_SHOWMINNOACTIVE
                if ($taskProcess.HasExited) { break }
                [CubeWindow]::Show($taskWindow, 4); $taskAction='minimize/restore'                    # SW_SHOWNOACTIVATE
            } else { [CubeWindow]::Resize($taskWindow, $size[0], $size[1]); $taskAction="resize $($size[0])x$($size[1])"; ++$taskResize }
            Wait-Cube ($taskWatch.ElapsedMilliseconds+1200)
            if ($taskProcess.HasExited) { break }
            $taskCaptures += Save-Capture ("step-{0:d4}" -f $taskStep) $taskAction ($taskStep -lt 12 -or $taskStep % 25 -eq 0)
            ++$taskStep
            Wait-Cube ($taskWatch.ElapsedMilliseconds+$ExerciseIntervalMs)
        }
        if ($taskProcess.HasExited) { throw "vkcube exited early with code $($taskProcess.ExitCode) after $([int]$taskWatch.Elapsed.TotalSeconds) s" }
        [CubeWindow]::Close([CubeWindow]::TopWindow($taskId))
        Wait-Cube ($taskWatch.ElapsedMilliseconds+15000)
    } else {
        foreach ($delay in $CaptureAfterMs) {
            Wait-Cube $delay
            if ($taskProcess.HasExited) { break }
            $taskCaptures += Save-Capture ("frame-{0}ms" -f $delay) 'capture' $true
        }
        Wait-Cube ($DeadlineSeconds*1000)
    }
    if (-not $taskProcess.HasExited) { $taskProcess.Kill(); $taskProcess.WaitForExit(); throw 'vkcube did not exit before the deadline' }
    $taskBlank=@($taskCaptures | Where-Object { $_.distinctSampledColors -lt 20 -or $_.unpaintedEdgeSamples })
    # "HasVK/WDDM present stats: R redirected, G gdi" (HASVK_WDDM_PRESENT_STATS, at swapchain destruction).
    $taskStats=@([regex]::Matches($taskErrors.Result, 'present stats: (\d+) redirected, (\d+) gdi'))
    $taskRedirected=($taskStats | ForEach-Object { [int]$_.Groups[1].Value } | Measure-Object -Sum).Sum
    $taskGdi=($taskStats | ForEach-Object { [int]$_.Groups[2].Value } | Measure-Object -Sum).Sum
    $taskCpu=$null; try { $taskCpu=[math]::Round($taskProcess.TotalProcessorTime.TotalSeconds,2) } catch {}
    $taskResult=[ordered]@{frames=$Frames; exerciseSeconds=$ExerciseSeconds; present=$Present; exitCode=$taskProcess.ExitCode; seconds=[math]::Round($taskWatch.Elapsed.TotalSeconds,2);
        cpuSeconds=$taskCpu;
        redirectedPresents=$taskRedirected; gdiPresents=$taskGdi;
        captureCount=$taskCaptures.Count; blankCaptures=$taskBlank.Count; captures=$taskCaptures}
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'report.json'), ($taskResult | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
    if ($taskBlank.Count) { throw "vkcube produced $($taskBlank.Count) blank captures" }
    $taskResult | ConvertTo-Json -Depth 4
    if ($taskProcess.ExitCode) { throw "vkcube failed: $($taskProcess.ExitCode)" }
} finally {
    if (-not $taskProcess.HasExited) { $taskProcess.Kill(); $taskProcess.WaitForExit() }
    [CubeWindow]::KeepDisplayOn($false)
    [IO.File]::WriteAllText((Join-Path $OutputDirectory 'stderr.log'), $taskErrors.Result, [Text.UTF8Encoding]::new($false))
    $taskProcess.Dispose()
}
