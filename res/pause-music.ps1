$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
if ($mode -in @('like', 'unlike')) {
    Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
    Add-Type -ReferencedAssemblies Accessibility,UIAutomationClient,UIAutomationTypes -TypeDefinition @"
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Accessibility;
using System.Windows.Automation;

public static class KonturLike {
    const int Blocked = 0x1, Selected = 0x18, Hidden = 0x18000;
    const int Like = 1, Previous = 2, Next = 3;
    delegate bool EnumWindow(IntPtr window, IntPtr unused);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumWindow callback, IntPtr unused);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassName(IntPtr window, StringBuilder name, int length);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr window, int mode);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("oleacc.dll")] static extern int AccessibleObjectFromWindow(IntPtr window, uint id, ref Guid iid, out IAccessible accessible);

    sealed class Control {
        public IAccessible Element;
        public int Kind, X, Y;
        public int[] Path;
    }

    static int Kind(string name) {
        switch ((name ?? "").ToLowerInvariant()) {
            case "нравится": case "like": case "убрать лайк": case "remove like": return Like;
            case "предыдущая песня": case "предыдущий трек": case "previous song": case "previous track": return Previous;
            case "следующая песня": case "следующий трек": case "next song": case "next track": return Next;
            default: return 0;
        }
    }

    static void Walk(IAccessible element, List<int> path, List<Control> controls, ref int budget) {
        if (budget-- <= 0 || path.Count > 40) return;
        try {
            var role = element.get_accRole(0);
            if (role is int && ((int)role == 43 || (int)role == 44)) {
                int kind = Kind(element.get_accName(0));
                int state = (int)element.get_accState(0);
                if (kind != 0 && (state & Hidden) == 0 && (kind != Like || (state & Blocked) == 0)) {
                    int x, y, w, h;
                    element.accLocation(out x, out y, out w, out h, 0);
                    if (w > 0 && h > 0)
                        controls.Add(new Control { Element = element, Kind = kind, X = x + w / 2,
                                                   Y = y + h / 2, Path = path.ToArray() });
                }
            }
            int children = element.accChildCount;
            for (int i = 1; i <= children && budget > 0; ++i) {
                IAccessible child = element.get_accChild(i) as IAccessible;
                if (child == null) continue;
                path.Add(i);
                Walk(child, path, controls, ref budget);
                path.RemoveAt(path.Count - 1);
            }
        } catch (COMException) {}
    }

    static int CommonDepth(int[] a, int[] b) {
        int i = 0;
        while (i < a.Length && i < b.Length && a[i] == b[i]) ++i;
        return i;
    }

    static Control FindHeart(IntPtr window) {
        Control heart = null;
        EnumChildWindows(window, delegate(IntPtr child, IntPtr unused) {
            var name = new StringBuilder(64);
            GetClassName(child, name, name.Capacity);
            if (name.ToString() != "Chrome_RenderWidgetHostHWND") return true;
            var iid = typeof(IAccessible).GUID;
            IAccessible root;
            if (AccessibleObjectFromWindow(child, unchecked((uint)-4), ref iid, out root) != 0 || root == null) return true;
            var controls = new List<Control>();
            int budget = 4000;
            Walk(root, new List<int>(), controls, ref budget);
            int bestDepth = 0;
            long bestDistance = long.MaxValue;
            foreach (var candidate in controls) {
                if (candidate.Kind != Like) continue;
                foreach (var next in controls) {
                    if (next.Kind != Next) continue;
                    foreach (var previous in controls) {
                        if (previous.Kind != Previous || Math.Abs(previous.Y - next.Y) > 26) continue;
                        int depth = Math.Min(CommonDepth(candidate.Path, next.Path), CommonDepth(candidate.Path, previous.Path));
                        if (depth < Math.Max(3, CommonDepth(previous.Path, next.Path) - 4)) continue;
                        long dy = candidate.Y - next.Y, dx = candidate.X - (next.X + previous.X) / 2;
                        long distance = dx * dx + dy * dy;
                        if (depth < bestDepth || (depth == bestDepth && distance >= bestDistance)) continue;
                        heart = candidate;
                        bestDepth = depth;
                        bestDistance = distance;
                    }
                }
            }
            return heart == null;
        }, IntPtr.Zero);
        return heart;
    }

    static bool IsLiked(Control heart) { return ((int)heart.Element.get_accState(0) & Selected) != 0; }

    public static int SetLiked(IntPtr window, bool liked) {
        bool minimized = IsIconic(window);
        IntPtr oldForeground = GetForegroundWindow();
        bool focused = false;
        if (minimized) ShowWindowAsync(window, 4);
        try {
            Control heart = null;
            for (int i = 0; i < 5 && heart == null; ++i) {
                if (i == 2 && oldForeground != window) {
                    focused = SetForegroundWindow(window);
                    try {
                        // Request the native provider so Chromium can initialize its accessibility tree.
                        AutomationElement.FromHandle(window).FindAll(TreeScope.Descendants, Condition.TrueCondition);
                    } catch (InvalidOperationException) {}
                      catch (COMException) {}
                }
                if (i != 0 || minimized) Thread.Sleep(180);
                heart = FindHeart(window);
            }
            if (heart == null) return 5;
            if (IsLiked(heart) == liked) return 0;
            string action = (heart.Element.get_accDefaultAction(0) ?? "").Trim().ToLowerInvariant();
            bool check = action == "поставить галочку" || action == "установить флажок" || action == "check";
            if (liked ? !check : check || action.Length == 0) return 5;
            heart.Element.accDoDefaultAction(0);
            for (int i = 0; i < 6; ++i) {
                Thread.Sleep(100);
                if (IsLiked(heart) == liked) return 0;
            }
            return 6;
        } catch (COMException) {
            return 5;
        } finally {
            if (minimized) ShowWindowAsync(window, 7);
            if (focused && oldForeground != IntPtr.Zero && GetForegroundWindow() == window)
                SetForegroundWindow(oldForeground);
        }
    }
}
"@
    foreach ($player in (Get-Process -Name 'Яндекс Музыка','YandexMusic' -ErrorAction SilentlyContinue)) {
        if ($player.MainWindowHandle -eq [IntPtr]::Zero) { continue }
        $result = [KonturLike]::SetLiked($player.MainWindowHandle, ($mode -eq 'like'))
        if ($result -ne 5) { exit $result }
    }
    exit 5
}
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$manager = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager,Windows.Media.Control,ContentType=WindowsRuntime]
$metadata = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties,Windows.Media.Control,ContentType=WindowsRuntime]
$asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1
} | Select-Object -First 1
function Wait-Media($operation, [type]$type) {
    $task = $asTask.MakeGenericMethod($type).Invoke($null, @($operation))
    if (!$task.Wait(5000)) { throw 'Media command timed out' }
    $task.Result
}
$names = @('Яндекс Музыка.exe', 'YandexMusic.exe')
$service = Wait-Media ($manager::RequestAsync()) $manager
$sessions = @($service.GetSessions() | Where-Object {
    $id = $_.SourceAppUserModelId
    $id -in $names -or $id -eq 'ru.yandex.desktop.music' -or
        $id -match '(?i)(?:^|[.!\\])YandexMusic(?:$|[.!])'
})
$session = $sessions | Sort-Object { $_.GetPlaybackInfo().PlaybackStatus -eq 'Playing' } -Descending | Select-Object -First 1
if (!$session) {
    if ($mode -eq 'pause') { exit 0 }
    exit 4
}
if ($mode -eq 'next') {
    if (!(Wait-Media ($session.TrySkipNextAsync()) ([bool]))) { exit 3 }
} elseif ($mode -eq 'previous') {
    if (!(Wait-Media ($session.TrySkipPreviousAsync()) ([bool]))) { exit 3 }
} elseif ($mode -eq 'play') {
    if ($session.GetPlaybackInfo().PlaybackStatus -ne 'Playing' -and
        !(Wait-Media ($session.TryPlayAsync()) ([bool]))) { exit 3 }
} elseif ($mode -eq 'track') {
    $track = Wait-Media ($session.TryGetMediaPropertiesAsync()) $metadata
    if (!$track.Title) { exit 4 }
    $label = @($track.Artist, $track.Title) | Where-Object { $_ }
    [Console]::Write((($label -join ' — ') -replace '\s+', ' ').Trim())
} elseif ($session.GetPlaybackInfo().PlaybackStatus -eq 'Playing' -and !(Wait-Media ($session.TryPauseAsync()) ([bool]))) {
    exit 3
}
