// inFamous2Launcher -- a small Windows front end for this repository's own build and run scripts.
//
// First run: asks for YOUR game files (disc, title update 1.04, the decrypted EBOOT.elf and the decrypted
// firmware libsre.prx), checks them, checks the build prerequisites (offering a winget install for each
// missing one, only after you confirm), then runs tools\build.ps1 one step at a time -- setup, check, analyze,
// lift, libsre, jobs, build -- and starts the game the way tools\run.ps1 does once the build succeeds.
// Later runs: Play when the build is up to date with the sources, the toolkit pin and patch, and your files.
//
// It contains no game code and downloads nothing by itself (setup.ps1 clones the ps3recomp toolkit from
// GitHub; winget installs only what you confirm). Build it with tools\build_launcher.cmd (the .NET Framework's
// own csc.exe, C# 5). Every path is relative to the folder the launcher sits in.
//
// Command line (all optional; for shortcuts and scripted tests):
//   --disc <dir> --update <dir> --eboot <EBOOT.elf> --libsre <libsre.prx | RPCS3 folder>   prefill the files dialog
//   --detect-out <file>     write the prerequisite / game-file / build-state report to <file> and exit
//   --winget-dry-run        Install buttons log the winget command instead of running it
//   --pretend-missing <ids> show these prerequisites as missing (git,python,cmake,ninja,vs,llvm): UI tests
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

[assembly: System.Reflection.AssemblyTitle("inFamous 2 recomp launcher")]
[assembly: System.Reflection.AssemblyProduct("infamous2-recomp")]
[assembly: System.Reflection.AssemblyDescription("Builds and starts the inFamous 2 static recompilation from your own game files")]
[assembly: System.Reflection.AssemblyCopyright("bhuiyah and infamous2-recomp contributors, MIT")]
[assembly: System.Reflection.AssemblyVersion("1.0.0.0")]

namespace Infamous2Launcher
{
    // ------------------------------------------------------------------ options
    class Options
    {
        public string Disc, Update, Eboot, Libsre, DetectOut;
        public bool WingetDryRun;
        public HashSet<string> PretendMissing = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        public static Options Parse(string[] a)
        {
            var o = new Options();
            for (int i = 0; i < a.Length; i++)
            {
                string k = a[i].ToLowerInvariant();
                Func<string> next = () => (i + 1 < a.Length) ? a[++i] : "";
                if (k == "--disc") o.Disc = next();
                else if (k == "--update") o.Update = next();
                else if (k == "--eboot") o.Eboot = next();
                else if (k == "--libsre") o.Libsre = next();
                else if (k == "--detect-out") o.DetectOut = next();
                else if (k == "--winget-dry-run") o.WingetDryRun = true;
                else if (k == "--pretend-missing")
                    foreach (var s in next().Split(',')) if (s.Trim().Length > 0) o.PretendMissing.Add(s.Trim());
            }
            if (Environment.GetEnvironmentVariable("INF2L_WINGET_DRYRUN") == "1") o.WingetDryRun = true;
            return o;
        }
    }

    // ------------------------------------------------------------------ helpers
    static class Util
    {
        public static string Sha1File(string path)
        {
            using (var s = File.OpenRead(path)) using (var h = SHA1.Create())
                return Hex(h.ComputeHash(s));
        }
        public static string Sha1(string text)
        {
            using (var h = SHA1.Create()) return Hex(h.ComputeHash(Encoding.UTF8.GetBytes(text)));
        }
        static string Hex(byte[] b)
        {
            var sb = new StringBuilder(b.Length * 2);
            foreach (var x in b) sb.Append(x.ToString("x2"));
            return sb.ToString();
        }
        // path, size and write time: cheap identity for big files (the EBOOT, the PRX)
        public static string Identity(string path)
        {
            if (path == null || !File.Exists(path)) return "missing:" + (path ?? "");
            var fi = new FileInfo(path);
            return fi.FullName.ToLowerInvariant() + "|" + fi.Length + "|" + fi.LastWriteTimeUtc.Ticks;
        }
        // Runs a short command and returns (exit code, stdout+stderr); -1 when it cannot start or times out.
        public static int Run(string exe, string args, out string output, int timeoutMs = 20000, string cwd = null)
        {
            output = "";
            try
            {
                var psi = new ProcessStartInfo(exe, args)
                {
                    UseShellExecute = false, CreateNoWindow = true,
                    RedirectStandardOutput = true, RedirectStandardError = true
                };
                if (cwd != null) psi.WorkingDirectory = cwd;
                using (var p = Process.Start(psi))
                {
                    var err = p.StandardError.ReadToEndAsync();
                    string o = p.StandardOutput.ReadToEnd();
                    if (!p.WaitForExit(timeoutMs)) { try { p.Kill(); } catch { } return -1; }
                    output = o + err.Result;
                    return p.ExitCode;
                }
            }
            catch { return -1; }
        }
        public static string FindOnPath(string file, string pathVar)
        {
            foreach (var d in (pathVar ?? "").Split(';'))
            {
                if (d.Trim().Length == 0) continue;
                try
                {
                    var p = Path.Combine(Environment.ExpandEnvironmentVariables(d.Trim().Trim('"')), file);
                    if (File.Exists(p)) return p;
                }
                catch { }
            }
            return null;
        }
        // PATH as a new process would see it (after an install), plus this process's own.
        public static string FreshPath()
        {
            string m = (string)Registry.GetValue(@"HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Environment", "Path", "");
            string u = (string)Registry.GetValue(@"HKEY_CURRENT_USER\Environment", "Path", "");
            var parts = new List<string>();
            foreach (var s in new[] { m, u, Environment.GetEnvironmentVariable("PATH") })
                foreach (var d in (s ?? "").Split(';'))
                {
                    var e = Environment.ExpandEnvironmentVariables(d.Trim());
                    if (e.Length > 0 && !parts.Contains(e, StringComparer.OrdinalIgnoreCase)) parts.Add(e);
                }
            return string.Join(";", parts);
        }
        public static string Pf { get { return Environment.GetEnvironmentVariable("ProgramFiles") ?? @"C:\Program Files"; } }
        public static string Pf86 { get { return Environment.GetEnvironmentVariable("ProgramFiles(x86)") ?? @"C:\Program Files (x86)"; } }
        public static string LocalAppData { get { return Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData); } }
        public static void KillTree(int pid)
        {
            string o;
            Run(Path.Combine(Environment.SystemDirectory, "taskkill.exe"), "/PID " + pid + " /T /F", out o, 15000);
        }
        public static string Elapsed(TimeSpan t)
        {
            return t.TotalHours >= 1 ? string.Format("{0}:{1:00}:{2:00}", (int)t.TotalHours, t.Minutes, t.Seconds)
                                     : string.Format("{0}:{1:00}", t.Minutes, t.Seconds);
        }
    }

    // ------------------------------------------------------------------ PS3 file checks (the same rules as tools\check_inputs.py)
    static class Ps3Files
    {
        public static readonly Dictionary<string, string> KnownEboots = new Dictionary<string, string>
        {
            { "f1c20717d6532dfb27e292a5db6cdf5a3218760b", "BCUS98125 disc EBOOT (1.00)" },
            { "7814ff1a87d7d3d10bd0e9627dc05ac353ed4162", "BCUS98125 update 1.04 EBOOT" },
        };
        public static readonly Dictionary<string, string> KnownLibsre = new Dictionary<string, string>
        {
            { "c2f5d2dbf1c58eadcc3367dc34b28f80c4251a67", "libsre.prx the port was developed with" },
        };

        public static Dictionary<string, string> ParseSfo(string path)
        {
            var r = new Dictionary<string, string>();
            byte[] d = File.ReadAllBytes(path);
            if (d.Length < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') throw new InvalidDataException("not a PARAM.SFO");
            int keyTab = BitConverter.ToInt32(d, 8), dataTab = BitConverter.ToInt32(d, 12), count = BitConverter.ToInt32(d, 16);
            for (int i = 0; i < count; i++)
            {
                int e = 20 + i * 16;
                int kOff = BitConverter.ToUInt16(d, e), fmt = BitConverter.ToUInt16(d, e + 2);
                int len = BitConverter.ToInt32(d, e + 4), dOff = BitConverter.ToInt32(d, e + 12);
                int ks = keyTab + kOff, ke = ks;
                while (ke < d.Length && d[ke] != 0) ke++;
                string key = Encoding.ASCII.GetString(d, ks, ke - ks);
                if (fmt == 0x0404) r[key] = BitConverter.ToUInt32(d, dataTab + dOff).ToString();
                else r[key] = Encoding.UTF8.GetString(d, dataTab + dOff, len).TrimEnd('\0');
            }
            return r;
        }
        // "ok" | "self" | "bad"
        public static string ElfKind(string path, out string why)
        {
            byte[] h = new byte[64];
            using (var s = File.OpenRead(path)) s.Read(h, 0, 64);
            if (h[0] == 'S' && h[1] == 'C' && h[2] == 'E' && h[3] == 0)
            { why = "still encrypted (a SELF): decrypt it in RPCS3 (Utilities > Decrypt PS3 Binaries)"; return "self"; }
            if (h[0] != 0x7F || h[1] != 'E' || h[2] != 'L' || h[3] != 'F' || h[4] != 2 || h[5] != 2)
            { why = "not a decrypted PS3 executable"; return "bad"; }
            why = "decrypted"; return "ok";
        }
        public static bool IsLibsre(string path, out string why)
        {
            string k = ElfKind(path, out why);
            if (k != "ok") return false;
            byte[] d = File.ReadAllBytes(path);
            if (d[16] != 0xFF || d[17] != 0xA4) { why = "an ELF, but not a PRX module"; return false; }
            if (Encoding.ASCII.GetString(d).IndexOf("cellSre_Library", StringComparison.Ordinal) < 0)
            { why = "a PRX, but not libsre (cellSre_Library)"; return false; }
            why = "decrypted libsre"; return true;
        }
        public static string FindEboot(string usrdir)
        {
            foreach (var n in new[] { "EBOOT.elf", "EBOOT.ELF" })
                if (File.Exists(Path.Combine(usrdir, n))) return Path.Combine(usrdir, n);
            return null;
        }
    }

    // ------------------------------------------------------------------ the player's files in game\
    class GameFiles
    {
        public readonly string Root;
        public GameFiles(string root) { Root = root; }
        public string Game { get { return Path.Combine(Root, "game"); } }
        public string DiscGame { get { return Path.Combine(Game, @"disc\PS3_GAME"); } }
        public string TitleId
        {
            get
            {
                try { string v; return ParseSfo(Path.Combine(DiscGame, "PARAM.SFO")).TryGetValue("TITLE_ID", out v) ? v : "BCUS98125"; }
                catch { return "BCUS98125"; }
            }
        }
        static Dictionary<string, string> ParseSfo(string p) { return Ps3Files.ParseSfo(p); }
        public string UpdateDir { get { return Path.Combine(Game, @"hdd0\game\" + TitleId); } }
        public string LibsrePath { get { return Path.Combine(Game, @"firmware\libsre.prx"); } }
        // the EBOOT check_inputs.py selects: the update's when there is one
        public string TargetEboot
        {
            get
            {
                string u = Directory.Exists(UpdateDir) ? Ps3Files.FindEboot(Path.Combine(UpdateDir, "USRDIR")) : null;
                return u ?? Ps3Files.FindEboot(Path.Combine(DiscGame, "USRDIR"));
            }
        }
        public bool Present
        {
            get
            {
                return File.Exists(Path.Combine(DiscGame, "PARAM.SFO")) && TargetEboot != null &&
                       (File.Exists(LibsrePath) || File.Exists(Path.Combine(Root, @"generated\prx\libsre.linked.bin")));
            }
        }
        public string Identity()
        {
            return string.Join("\n", new[] {
                Util.Identity(Path.Combine(DiscGame, "PARAM.SFO")), Util.Identity(Path.Combine(UpdateDir, "PARAM.SFO")),
                Util.Identity(TargetEboot), Util.Identity(LibsrePath) });
        }

        public class Finding { public int Level; public string Text; public Finding(int l, string t) { Level = l; Text = t; } }   // 0 ok, 1 warn, 2 error

        // Checks a selection: disc folder (holding PS3_GAME, or PS3_GAME itself), the update folder, an EBOOT.elf
        // given separately (or null), and libsre.prx (or an RPCS3 folder holding the decrypted one).
        public static List<Finding> Check(string disc, string update, string eboot, string libsre,
                                          out string discGame, out string ebootPath, out string libsrePath)
        {
            var f = new List<Finding>();
            discGame = ebootPath = libsrePath = null;
            // disc
            if (string.IsNullOrEmpty(disc)) f.Add(new Finding(2, "Disc folder: not chosen."));
            else
            {
                string g = Directory.Exists(Path.Combine(disc, "PS3_GAME")) ? Path.Combine(disc, "PS3_GAME") : disc;
                string sfo = Path.Combine(g, "PARAM.SFO");
                if (!File.Exists(sfo)) f.Add(new Finding(2, "Disc folder: no PS3_GAME\\PARAM.SFO under " + disc));
                else
                {
                    try
                    {
                        var s = Ps3Files.ParseSfo(sfo);
                        string id; s.TryGetValue("TITLE_ID", out id);
                        if (id == "BCUS98125") f.Add(new Finding(0, "Disc: inFamous 2, BCUS98125 (North America)."));
                        else f.Add(new Finding(1, "Disc: title id " + id + " -- this port targets BCUS98125; other regions are untested."));
                        if (!Directory.Exists(Path.Combine(g, "USRDIR"))) f.Add(new Finding(2, "Disc: PS3_GAME has no USRDIR folder."));
                        else discGame = g;
                    }
                    catch (Exception e) { f.Add(new Finding(2, "Disc: PARAM.SFO unreadable (" + e.Message + ").")); }
                }
            }
            // update
            string updEboot = null;
            if (string.IsNullOrEmpty(update)) f.Add(new Finding(2, "Update 1.04 folder: not chosen (this port runs update 1.04)."));
            else
            {
                string sfo = Path.Combine(update, "PARAM.SFO");
                if (!File.Exists(sfo)) f.Add(new Finding(2, "Update folder: no PARAM.SFO in " + update + " (choose the BCUS98125 folder that holds PARAM.SFO and USRDIR)."));
                else
                {
                    try
                    {
                        var s = Ps3Files.ParseSfo(sfo);
                        string v; s.TryGetValue("APP_VER", out v);
                        if (v == "01.04") f.Add(new Finding(0, "Update: APP_VER 01.04."));
                        else f.Add(new Finding(2, "Update: APP_VER " + v + " -- the port needs update 1.04 (install every update up to 1.04)."));
                    }
                    catch (Exception e) { f.Add(new Finding(2, "Update: PARAM.SFO unreadable (" + e.Message + ").")); }
                    if (!Directory.Exists(Path.Combine(update, "USRDIR"))) f.Add(new Finding(2, "Update folder has no USRDIR."));
                    else updEboot = Ps3Files.FindEboot(Path.Combine(update, "USRDIR"));
                }
            }
            // EBOOT
            string e2 = !string.IsNullOrEmpty(eboot) ? eboot : updEboot;
            if (e2 == null) f.Add(new Finding(2, "Decrypted EBOOT.elf: not found in the update's USRDIR -- choose the EBOOT.elf you decrypted with RPCS3."));
            else if (!File.Exists(e2)) f.Add(new Finding(2, "Decrypted EBOOT.elf: " + e2 + " does not exist."));
            else
            {
                string why;
                if (Ps3Files.ElfKind(e2, out why) != "ok") f.Add(new Finding(2, "EBOOT: " + why + "."));
                else
                {
                    string h = Util.Sha1File(e2), name;
                    if (Ps3Files.KnownEboots.TryGetValue(h, out name))
                    {
                        if (h == "7814ff1a87d7d3d10bd0e9627dc05ac353ed4162") f.Add(new Finding(0, "EBOOT: " + name + " (SHA-1 matches)."));
                        else f.Add(new Finding(2, "EBOOT: this is the " + name + "; decrypt the UPDATE's EBOOT.BIN (1.04) instead."));
                    }
                    else f.Add(new Finding(1, "EBOOT: SHA-1 " + h.Substring(0, 12) + "... is not the known 1.04 EBOOT; the build may not match it."));
                    ebootPath = e2;
                }
            }
            // libsre
            string l = libsre;
            if (!string.IsNullOrEmpty(l) && Directory.Exists(l))
            {
                string c = Path.Combine(l, @"dev_flash\sys\external\libsre.prx");
                string enc = Path.Combine(l, @"dev_flash\sys\external\libsre.sprx");
                if (File.Exists(c)) l = c;
                else if (File.Exists(enc)) { f.Add(new Finding(2, "libsre: " + l + " has only the encrypted libsre.sprx -- decrypt it in RPCS3 (Utilities > Decrypt PS3 Binaries) first.")); l = null; }
                else { f.Add(new Finding(2, "libsre: no dev_flash\\sys\\external\\libsre.prx under " + l + ".")); l = null; }
            }
            if (l == null) { }
            else if (string.IsNullOrEmpty(l)) f.Add(new Finding(2, "Decrypted libsre.prx: not chosen."));
            else if (!File.Exists(l)) f.Add(new Finding(2, "libsre: " + l + " does not exist."));
            else
            {
                string why;
                if (!Ps3Files.IsLibsre(l, out why)) f.Add(new Finding(2, "libsre: " + why + "."));
                else
                {
                    string h = Util.Sha1File(l);
                    if (Ps3Files.KnownLibsre.ContainsKey(h)) f.Add(new Finding(0, "libsre.prx: decrypted, the known version."));
                    else f.Add(new Finding(1, "libsre.prx: decrypted, another firmware version than the one tested (should work)."));
                    libsrePath = l;
                }
            }
            return f;
        }
    }

    // ------------------------------------------------------------------ prerequisites
    class Prereq
    {
        public string Id, Name, Why, WingetId, WingetArgs, Found, Note;
        public bool Ok;
    }

    static class Prereqs
    {
        public static List<Prereq> Detect(Options o)
        {
            string path = Util.FreshPath();
            var list = new List<Prereq>();
            string o1;

            // Git for Windows: build.ps1 runs Git Bash from Program Files; setup.ps1 runs git.
            var git = new Prereq { Id = "git", Name = "Git for Windows (with Git Bash)", Why = "fetches the toolkit, runs the lift scripts", WingetId = "Git.Git" };
            foreach (var b in new[] { Path.Combine(Util.Pf, @"Git\bin\bash.exe"), Path.Combine(Util.Pf86, @"Git\bin\bash.exe") })
                if (File.Exists(b)) { git.Ok = true; git.Found = b; break; }
            if (!git.Ok) git.Note = "Git Bash must be in Program Files\\Git (the installer's default).";
            list.Add(git);

            // Python 3.10+
            var py = new Prereq { Id = "python", Name = "Python 3.10 or newer", Why = "runs the recompiler (ps3recomp)", WingetId = "Python.Python.3.12" };
            foreach (var c in new[] { new[] { "py", "-3" }, new[] { "python", "" }, new[] { "python3", "" } })
            {
                string exe = Util.FindOnPath(c[0] + ".exe", path);
                if (exe == null) continue;
                if (Util.Run(exe, (c[1] + " -c \"import sys;print(sys.executable);print('%d.%d'%sys.version_info[:2])\"").Trim(), out o1, 15000) != 0) continue;
                var lines = o1.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries);
                if (lines.Length < 2) continue;
                Version v;
                if (Version.TryParse(lines[1].Trim(), out v) && v >= new Version(3, 10)) { py.Ok = true; py.Found = lines[0].Trim() + " (" + v + ")"; break; }
                py.Note = "found Python " + lines[1].Trim() + "; 3.10 or newer is needed";
            }
            list.Add(py);

            // Visual Studio 2022 Build Tools with the C++ tools
            var vs = new Prereq
            {
                Id = "vs", Name = "Visual Studio 2022 Build Tools (C++)", Why = "the C++ libraries and linker", WingetId = "Microsoft.VisualStudio.2022.BuildTools",
                WingetArgs = "--override \"--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended\""
            };
            string vswhere = Path.Combine(Util.Pf86, @"Microsoft Visual Studio\Installer\vswhere.exe");
            string vsDir = null;
            if (File.Exists(vswhere) &&
                Util.Run(vswhere, "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath", out o1) == 0 &&
                o1.Trim().Length > 0)
            { vsDir = o1.Trim().Split('\n')[0].Trim(); vs.Ok = true; vs.Found = vsDir; }
            else if (File.Exists(vswhere)) vs.Note = "Visual Studio is installed without the C++ tools: open Visual Studio Installer, Modify, and add \"Desktop development with C++\".";
            list.Add(vs);

            // CMake and Ninja: on PATH, in their usual folders, or the copies Visual Studio's C++ CMake tools bring.
            var cm = new Prereq { Id = "cmake", Name = "CMake 3.20 or newer", Why = "configures the build", WingetId = "Kitware.CMake" };
            cm.Found = Util.FindOnPath("cmake.exe", path);
            if (cm.Found == null && File.Exists(Path.Combine(Util.Pf, @"CMake\bin\cmake.exe"))) cm.Found = Path.Combine(Util.Pf, @"CMake\bin\cmake.exe");
            if (cm.Found == null && vsDir != null)
            {
                string c = Path.Combine(vsDir, @"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe");
                if (File.Exists(c)) cm.Found = c;
            }
            cm.Ok = cm.Found != null;
            list.Add(cm);

            var nj = new Prereq { Id = "ninja", Name = "Ninja", Why = "runs the compiler on every core", WingetId = "Ninja-build.Ninja" };
            nj.Found = Util.FindOnPath("ninja.exe", path);
            if (nj.Found == null)
            {
                string l = Path.Combine(Util.LocalAppData, @"Microsoft\WinGet\Links\ninja.exe");
                if (File.Exists(l)) nj.Found = l;
            }
            if (nj.Found == null && vsDir != null)
            {
                string c = Path.Combine(vsDir, @"Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe");
                if (File.Exists(c)) nj.Found = c;
            }
            nj.Ok = nj.Found != null;
            list.Add(nj);

            var llvm = new Prereq { Id = "llvm", Name = "LLVM (clang-cl)", Why = "the compiler", WingetId = "LLVM.LLVM" };
            string cl = Path.Combine(Util.Pf, @"LLVM\bin\clang-cl.exe");
            if (File.Exists(cl)) { llvm.Ok = true; llvm.Found = cl; }
            else llvm.Note = "clang-cl must be in Program Files\\LLVM (the installer's default).";
            list.Add(llvm);

            foreach (var p in list) if (o.PretendMissing.Contains(p.Id)) { p.Ok = false; p.Found = null; p.Note = "(shown as missing: --pretend-missing)"; }
            return list;
        }

        // Folders the build's processes need on PATH: where CMake, Ninja, Git and Python were found.
        public static string ChildPath(List<Prereq> list)
        {
            var dirs = new List<string>();
            foreach (var p in list)
            {
                if (!p.Ok || p.Found == null) continue;
                string f = p.Found;
                int paren = f.IndexOf(" (", StringComparison.Ordinal);
                if (paren > 0) f = f.Substring(0, paren);
                if (p.Id == "cmake" || p.Id == "ninja" || p.Id == "python") dirs.Add(Path.GetDirectoryName(f));
                if (p.Id == "git") dirs.Add(Path.Combine(Path.GetDirectoryName(Path.GetDirectoryName(f)), "cmd"));
            }
            dirs.Add(Util.FreshPath());
            return string.Join(";", dirs.Where(d => !string.IsNullOrEmpty(d)).Distinct(StringComparer.OrdinalIgnoreCase));
        }

        public static string WingetExe()
        {
            string w = Path.Combine(Util.LocalAppData, @"Microsoft\WindowsApps\winget.exe");
            if (File.Exists(w)) return w;
            return Util.FindOnPath("winget.exe", Util.FreshPath());
        }
        public static string WingetCommand(Prereq p)
        {
            return "winget install --id " + p.WingetId + " --exact --source winget" + (p.WingetArgs != null ? " " + p.WingetArgs : "");
        }
    }

    // ------------------------------------------------------------------ the pipeline (tools\build.ps1 -Step <id>)
    class StepDef
    {
        public string Id, Title, Marker; public int Weight; public string[] Inputs;
        public StepDef(string id, string title, int weight, string marker, params string[] inputs)
        { Id = id; Title = title; Weight = weight; Marker = marker; Inputs = inputs; }
    }

    class Pipeline
    {
        public static readonly StepDef[] Steps =
        {
            new StepDef("setup",   "Fetch and patch the ps3recomp toolkit", 5, @"external\ps3recomp\.git",
                        "ps3recomp.pin", @"patches\ps3recomp\*", @"tools\setup.ps1", @"tools\python.ps1"),
            new StepDef("check",   "Check your game files", 2, @"analysis\inputs.json", @"tools\check_inputs.py"),
            new StepDef("analyze", "Analyze the game's executable", 5, @"analysis\EBOOT.functions.json",
                        @"tools\analyze.sh", @"tools\import_coverage.py", @"tools\code_end.py"),
            new StepDef("lift",    "Translate the PowerPC and SPU code to C++", 18, @"generated\ppu",
                        @"tools\relift.sh", @"tools\ppu_postlift.py", @"config\ss_cont_list.txt", @"config\ss_cont_list.104.txt"),
            new StepDef("libsre",  "Translate the SPURS library (libsre)", 5, @"generated\prx\libsre.linked.bin",
                        @"tools\lift_libsre.sh", @"config\ss_cont_list_libsre.txt"),
            new StepDef("jobs",    "Translate the SPURS jobs", 18, @"generated\spu_jobs",
                        @"tools\lift_jobs.sh", @"tools\extract_eboot_jobs.py", @"tools\lift_policy.py", @"tools\lift_jobchain.py",
                        @"tools\extract_policy_modules.py"),
            new StepDef("build",   "Compile the game (the long step)", 47, @"build\infamous2.exe",
                        "CMakeLists.txt", @"src\*", @"tools\build.ps1"),
        };

        readonly string root;
        readonly GameFiles files;
        public Pipeline(string root, GameFiles files) { this.root = root; this.files = files; }

        IEnumerable<string> Expand(string pattern)
        {
            if (pattern.EndsWith(@"\*"))
            {
                string d = Path.Combine(root, pattern.Substring(0, pattern.Length - 2));
                if (!Directory.Exists(d)) return new string[0];
                return Directory.GetFiles(d, "*", SearchOption.AllDirectories).OrderBy(x => x, StringComparer.OrdinalIgnoreCase);
            }
            return new[] { Path.Combine(root, pattern) };
        }

        // Each step's fingerprint covers its own inputs and the step before it: a change re-runs that step and every later one.
        public string[] Fingerprints()
        {
            var fp = new string[Steps.Length];
            string prev = "";
            for (int i = 0; i < Steps.Length; i++)
            {
                var sb = new StringBuilder(prev).Append('\n').Append(Steps[i].Id);
                foreach (var pat in Steps[i].Inputs)
                    foreach (var f in Expand(pat))
                        sb.Append('\n').Append(f.Substring(root.Length)).Append('=').Append(File.Exists(f) ? Util.Sha1File(f) : "missing");
                if (Steps[i].Id == "check") sb.Append('\n').Append(files.Identity());
                prev = fp[i] = Util.Sha1(sb.ToString());
            }
            return fp;
        }

        public bool MarkerOk(int i)
        {
            string m = Path.Combine(root, Steps[i].Marker);
            return File.Exists(m) || Directory.Exists(m);
        }
    }

    // ------------------------------------------------------------------ state (.launcher\state.ini: key=value)
    class State
    {
        readonly string path;
        public Dictionary<string, string> V = new Dictionary<string, string>();
        public State(string root)
        {
            path = Path.Combine(root, @".launcher\state.ini");
            try
            {
                if (File.Exists(path))
                    foreach (var l in File.ReadAllLines(path))
                    {
                        int k = l.IndexOf('=');
                        if (k > 0) V[l.Substring(0, k)] = l.Substring(k + 1);
                    }
            }
            catch { }
        }
        public string Get(string k) { string v; return V.TryGetValue(k, out v) ? v : null; }
        public void Set(string k, string v) { if (v == null) V.Remove(k); else V[k] = v; Save(); }
        public void Save()
        {
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path));
                File.WriteAllLines(path, V.OrderBy(kv => kv.Key).Select(kv => kv.Key + "=" + kv.Value));
            }
            catch { }
        }
    }

    // ------------------------------------------------------------------ the game files dialog
    class GameFilesForm : Form
    {
        readonly TextBox tDisc = new TextBox(), tUpd = new TextBox(), tEboot = new TextBox(), tLib = new TextBox();
        readonly TextBox tResult = new TextBox();
        readonly Button bUse = new Button(), bCheck = new Button(), bCancel = new Button();
        readonly GameFiles files;
        public string DiscGame, UpdateDir, EbootPath, LibsrePath;
        bool lastOk;

        public GameFilesForm(GameFiles files, Options o)
        {
            this.files = files;
            Text = "inFamous 2 -- your game files";
            Font = new Font("Segoe UI", 9f);
            ClientSize = new Size(820, 560);
            FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;

            var intro = new Label
            {
                Left = 16, Top = 12, Width = 790, Height = 92,
                Text = "This port runs YOUR copy of inFamous 2 (BCUS98125) with title update 1.04. Nothing from the game is included.\r\n" +
                       "From your own console and dumps you need: the disc's files (the folder that holds PS3_GAME), the 1.04 update folder " +
                       "(BCUS98125, holding PARAM.SFO and USRDIR), the update's EBOOT.BIN decrypted with RPCS3 (Utilities > Decrypt PS3 Binaries) " +
                       "as EBOOT.elf, and libsre.prx decrypted from your PS3 firmware in RPCS3 (dev_flash\\sys\\external\\libsre.sprx).\r\n" +
                       "The disc folder is linked, not copied; the update (about 230 MB), the EBOOT and libsre are copied into this folder's game\\. " +
                       "Your originals are never changed. See game\\README.md for details."
            };
            Controls.Add(intro);
            int y = 112;
            Row("Disc folder (holds PS3_GAME):", tDisc, true, ref y, o.Disc);
            Row("Update 1.04 folder (BCUS98125 with PARAM.SFO + USRDIR):", tUpd, true, ref y, o.Update);
            Row("Decrypted update EBOOT.elf (leave empty if it is in the update's USRDIR):", tEboot, false, ref y, o.Eboot);
            Row("Decrypted libsre.prx (or type the path of your RPCS3 folder, which holds dev_flash):", tLib, false, ref y, o.Libsre);

            tResult.Multiline = true; tResult.ReadOnly = true; tResult.ScrollBars = ScrollBars.Vertical;
            tResult.SetBounds(16, y + 6, 788, 560 - y - 60);
            tResult.Font = new Font("Consolas", 9f);
            Controls.Add(tResult);

            bCheck.Text = "Check"; bCheck.SetBounds(16, 518, 110, 30); bCheck.Click += (s, e) => DoCheck();
            bUse.Text = "Use these files"; bUse.SetBounds(574, 518, 130, 30); bUse.Click += (s, e) => DoUse();
            bCancel.Text = "Cancel"; bCancel.SetBounds(714, 518, 90, 30); bCancel.DialogResult = DialogResult.Cancel;
            Controls.AddRange(new Control[] { bCheck, bUse, bCancel });
            CancelButton = bCancel; AcceptButton = bUse;
            Shown += (s, e) => { if (tDisc.Text.Length > 0) DoCheck(); };
        }

        void Row(string label, TextBox t, bool folder, ref int y, string init)
        {
            Controls.Add(new Label { Text = label, Left = 16, Top = y, Width = 700, Height = 18 });
            t.SetBounds(16, y + 20, 690, 24);
            if (!string.IsNullOrEmpty(init)) t.Text = init;
            t.TextChanged += (s, e) => { lastOk = false; };
            Controls.Add(t);
            var b = new Button { Text = "Browse...", Left = 714, Top = y + 19, Width = 90, Height = 26 };
            b.Click += (s, e) =>
            {
                if (folder)
                {
                    using (var d = new FolderBrowserDialog { Description = label, ShowNewFolderButton = false })
                        if (d.ShowDialog(this) == DialogResult.OK) t.Text = d.SelectedPath;
                }
                else
                {
                    using (var d = new OpenFileDialog { Title = label, Filter = t == tLib ? "libsre.prx|libsre.prx|PRX (*.prx)|*.prx|All files|*.*" : "EBOOT.elf|EBOOT.elf;*.elf|All files|*.*" })
                        if (d.ShowDialog(this) == DialogResult.OK) t.Text = d.FileName;
                }
                lastOk = false;
            };
            Controls.Add(b);
            y += 54;
        }

        bool DoCheck()
        {
            Cursor = Cursors.WaitCursor;
            string dg, eb, lp;
            var f = GameFiles.Check(tDisc.Text.Trim(), tUpd.Text.Trim(), tEboot.Text.Trim(), tLib.Text.Trim(), out dg, out eb, out lp);
            Cursor = Cursors.Default;
            var sb = new StringBuilder();
            foreach (var x in f) sb.Append(x.Level == 0 ? "[ok] " : x.Level == 1 ? "[!]  " : "[x]  ").Append(x.Text).Append("\r\n");
            lastOk = !f.Any(x => x.Level == 2);
            sb.Append(lastOk ? "\r\nReady: press \"Use these files\"." : "\r\nFix the [x] lines, then Check again.");
            tResult.Text = sb.ToString();
            DiscGame = dg; UpdateDir = tUpd.Text.Trim(); EbootPath = eb; LibsrePath = lp;
            return lastOk;
        }

        void DoUse()
        {
            if (!DoCheck()) { MessageBox.Show(this, "Some files are missing or wrong: see the [x] lines.", Text); return; }
            DialogResult = DialogResult.OK;
            Close();
        }
    }

    // ------------------------------------------------------------------ main window
    class MainForm : Form
    {
        readonly string root;
        readonly Options opts;
        readonly GameFiles files;
        readonly Pipeline pipe;
        readonly State state;
        List<Prereq> prereqs = new List<Prereq>();

        readonly Label lblFiles = new Label(), lblStep = new Label(), lblElapsed = new Label(), lblHint = new Label();
        readonly Button btnFiles = new Button(), btnMain = new Button(), btnCancel = new Button(), btnRebuild = new Button(),
                        btnLogs = new Button(), btnFolder = new Button(), btnRecheck = new Button();
        readonly LinkLabel lnkOldBuild = new LinkLabel();
        readonly ProgressBar bar = new ProgressBar();
        readonly TextBox txtLog = new TextBox();
        readonly TableLayoutPanel tblPre = new TableLayoutPanel();
        readonly System.Windows.Forms.Timer tick = new System.Windows.Forms.Timer();

        // running state
        Process proc;              // the current step's powershell
        int stepIndex = -1, runFrom = 0;
        bool cancelling, building;
        DateTime runStart, stepStart;
        double stepFrac;
        string[] fps;
        StreamWriter logFile;
        string logPath;
        readonly object qLock = new object();
        readonly List<string> queue = new List<string>();
        readonly List<string> tail = new List<string>();
        Process game;              // the run.ps1 powershell while the game runs
        DateTime gameStart;

        public MainForm(string root, Options opts)
        {
            this.root = root; this.opts = opts;
            files = new GameFiles(root);
            pipe = new Pipeline(root, files);
            state = new State(root);

            Text = "inFamous 2 -- PC launcher";
            Font = new Font("Segoe UI", 9f);
            ClientSize = new Size(1000, 760);
            MinimumSize = new Size(820, 640);
            StartPosition = FormStartPosition.CenterScreen;
            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

            var title = new Label { Text = "inFamous 2 -- native PC build (static recompilation)", Font = new Font("Segoe UI", 15f, FontStyle.Bold), Left = 14, Top = 10, Width = 960, Height = 32 };
            var sub = new Label { Text = "Builds a Windows executable from your own copy of the game (BCUS98125, update 1.04) and starts it. No emulator; no game files included.", Left = 16, Top = 44, Width = 960, Height = 20, ForeColor = SystemColors.GrayText };
            Controls.Add(title); Controls.Add(sub);

            var gFiles = new GroupBox { Text = "1. Your game files", Left = 12, Top = 70, Width = 976, Height = 64, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            lblFiles.SetBounds(12, 22, 820, 36); lblFiles.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            btnFiles.Text = "Choose..."; btnFiles.SetBounds(860, 22, 104, 28); btnFiles.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            btnFiles.Click += (s, e) => ChooseFiles();
            gFiles.Controls.Add(lblFiles); gFiles.Controls.Add(btnFiles);
            Controls.Add(gFiles);

            var gPre = new GroupBox { Text = "2. Build tools", Left = 12, Top = 140, Width = 976, Height = 196, Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right };
            tblPre.SetBounds(10, 20, 954, 140); tblPre.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            tblPre.ColumnCount = 3;
            tblPre.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 300));
            tblPre.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
            tblPre.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 90));
            btnRecheck.Text = "Check again"; btnRecheck.SetBounds(10, 162, 104, 26);
            btnRecheck.Click += (s, e) => { DetectPrereqs(); RefreshUi(); };
            gPre.Controls.Add(tblPre); gPre.Controls.Add(btnRecheck);
            Controls.Add(gPre);

            var gBuild = new GroupBox { Text = "3. Build and play", Left = 12, Top = 342, Width = 976, Height = 360, Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right };
            lblStep.SetBounds(12, 22, 700, 20); lblStep.Font = new Font("Segoe UI", 9.5f, FontStyle.Bold); lblStep.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            lblElapsed.SetBounds(720, 22, 244, 20); lblElapsed.TextAlign = ContentAlignment.TopRight; lblElapsed.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            bar.SetBounds(12, 46, 952, 20); bar.Maximum = 1000; bar.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
            txtLog.Multiline = true; txtLog.ReadOnly = true; txtLog.WordWrap = false; txtLog.ScrollBars = ScrollBars.Both;
            txtLog.Font = new Font("Consolas", 8.5f); txtLog.BackColor = Color.FromArgb(24, 24, 24); txtLog.ForeColor = Color.Gainsboro;
            txtLog.SetBounds(12, 72, 952, 278); txtLog.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
            gBuild.Controls.AddRange(new Control[] { lblStep, lblElapsed, bar, txtLog });
            Controls.Add(gBuild);

            int by = 712;
            btnMain.SetBounds(808, by, 180, 38); btnMain.Font = new Font("Segoe UI", 11f, FontStyle.Bold); btnMain.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            btnMain.Click += (s, e) => MainAction();
            btnCancel.Text = "Cancel"; btnCancel.SetBounds(712, by + 6, 90, 30); btnCancel.Anchor = AnchorStyles.Bottom | AnchorStyles.Right;
            btnCancel.Click += (s, e) => CancelRun();
            btnRebuild.Text = "Rebuild"; btnRebuild.SetBounds(12, by + 6, 90, 30); btnRebuild.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnRebuild.Click += (s, e) => Rebuild();
            btnLogs.Text = "Open logs"; btnLogs.SetBounds(108, by + 6, 90, 30); btnLogs.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnLogs.Click += (s, e) => OpenFolder(Path.Combine(root, "logs"), null);
            btnFolder.Text = "Open game folder"; btnFolder.SetBounds(204, by + 6, 120, 30); btnFolder.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            btnFolder.Click += (s, e) => OpenFolder(root, File.Exists(ExePath) ? ExePath : null);
            lnkOldBuild.Text = "Play the existing build without updating it"; lnkOldBuild.SetBounds(336, by + 12, 300, 20); lnkOldBuild.Anchor = AnchorStyles.Bottom | AnchorStyles.Left;
            lnkOldBuild.LinkClicked += (s, e) => LaunchGame();
            lblHint.SetBounds(336, by + 12, 370, 20); lblHint.Anchor = AnchorStyles.Bottom | AnchorStyles.Left; lblHint.ForeColor = SystemColors.GrayText;
            Controls.AddRange(new Control[] { btnMain, btnCancel, btnRebuild, btnLogs, btnFolder, lnkOldBuild, lblHint });
            AcceptButton = btnMain;

            tick.Interval = 150;
            tick.Tick += (s, e) => OnTick();
            tick.Start();
            FormClosing += OnClosing;
            Shown += (s, e) => OnShown();
        }

        string ExePath { get { return Path.Combine(root, @"build\infamous2.exe"); } }

        void OnShown()
        {
            DetectPrereqs();
            RefreshUi();
            Append("launcher: " + root);
            if (!files.Present) BeginInvoke(new Action(ChooseFiles));
        }

        // ---------------------------------------------------------- game files
        void ChooseFiles()
        {
            if (building) return;
            using (var f = new GameFilesForm(files, opts))
            {
                if (f.ShowDialog(this) != DialogResult.OK) { RefreshUi(); return; }
                string err = null;
                Cursor = Cursors.WaitCursor;
                lblStep.Text = "Copying your update files into game\\ ...";
                Refresh();
                try { Import(f.DiscGame, f.UpdateDir, f.EbootPath, f.LibsrePath); }
                catch (Exception ex) { err = ex.Message; }
                Cursor = Cursors.Default;
                if (err != null) { Append("import failed: " + err); MessageBox.Show(this, "Could not set up game\\:\r\n\r\n" + err, Text, MessageBoxButtons.OK, MessageBoxIcon.Error); }
                else Append("game files set up in game\\ (disc linked from " + f.DiscGame + ")");
            }
            RefreshUi();
        }

        void Import(string discGame, string updDir, string eboot, string libsre)
        {
            string game = files.Game;
            Directory.CreateDirectory(Path.Combine(game, "disc"));
            // disc: a junction game\disc\PS3_GAME -> the player's PS3_GAME (read-only use; 15+ GB stays where it is)
            string link = files.DiscGame;
            string full = Path.GetFullPath(discGame).TrimEnd('\\');
            if (!string.Equals(full, Path.GetFullPath(link).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
            {
                if (Directory.Exists(link))
                {
                    var a = File.GetAttributes(link);
                    if ((a & FileAttributes.ReparsePoint) == 0)
                        throw new IOException(link + " is a real folder. Move it away (or choose it as the disc folder) and try again.");
                    Directory.Delete(link, false);         // the link only, never its target
                }
                string o;
                int rc = Util.Run(Path.Combine(Environment.SystemDirectory, "cmd.exe"), "/c mklink /J \"" + link + "\" \"" + full + "\"", out o, 20000);
                if (rc != 0 || !Directory.Exists(link)) throw new IOException("mklink /J failed: " + o.Trim());
            }
            // update: a copy (small), so nothing the game writes ever lands in the original
            string title = files.TitleId;
            string dst = Path.Combine(game, @"hdd0\game\" + title);
            string src = Path.GetFullPath(updDir).TrimEnd('\\');
            if (!string.Equals(src, Path.GetFullPath(dst).TrimEnd('\\'), StringComparison.OrdinalIgnoreCase))
                CopyTree(src, dst);
            if (!string.IsNullOrEmpty(eboot))
            {
                string to = Path.Combine(dst, @"USRDIR\EBOOT.elf");
                if (!string.Equals(Path.GetFullPath(eboot), Path.GetFullPath(to), StringComparison.OrdinalIgnoreCase))
                    File.Copy(eboot, to, true);
            }
            // libsre
            string lto = files.LibsrePath;
            Directory.CreateDirectory(Path.GetDirectoryName(lto));
            if (!string.Equals(Path.GetFullPath(libsre), Path.GetFullPath(lto), StringComparison.OrdinalIgnoreCase))
                File.Copy(libsre, lto, true);
            state.Set("files.disc", full);
            state.Set("files.update", src);
        }

        static void CopyTree(string src, string dst)
        {
            Directory.CreateDirectory(dst);
            foreach (var d in Directory.GetDirectories(src, "*", SearchOption.AllDirectories))
                Directory.CreateDirectory(Path.Combine(dst, d.Substring(src.Length + 1)));
            foreach (var f in Directory.GetFiles(src, "*", SearchOption.AllDirectories))
            {
                string t = Path.Combine(dst, f.Substring(src.Length + 1));
                var fi = new FileInfo(f);
                if (File.Exists(t))
                {
                    var ti = new FileInfo(t);
                    if (ti.Length == fi.Length && ti.LastWriteTimeUtc == fi.LastWriteTimeUtc) continue;
                }
                File.Copy(f, t, true);
                File.SetAttributes(t, FileAttributes.Normal);
            }
        }

        // ---------------------------------------------------------- prerequisites
        void DetectPrereqs()
        {
            prereqs = Prereqs.Detect(opts);
            tblPre.SuspendLayout();
            tblPre.Controls.Clear();
            tblPre.RowStyles.Clear();
            tblPre.RowCount = prereqs.Count;
            int r = 0;
            foreach (var p in prereqs)
            {
                tblPre.RowStyles.Add(new RowStyle(SizeType.Absolute, 23));
                tblPre.Controls.Add(new Label { Text = p.Name, AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft }, 0, r);
                string st = p.Ok ? "OK  " + p.Found : "MISSING -- " + p.Why + (p.Note != null ? ". " + p.Note : "");
                tblPre.Controls.Add(new Label
                {
                    Text = st, AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft, AutoEllipsis = true,
                    ForeColor = p.Ok ? Color.DarkGreen : Color.Firebrick
                }, 1, r);
                if (!p.Ok && p.WingetId != null)
                {
                    var b = new Button { Text = "Install", Dock = DockStyle.Fill, Margin = new Padding(2, 0, 2, 1), Tag = p };
                    b.Click += (s, e) => InstallPrereq((Prereq)((Button)s).Tag);
                    tblPre.Controls.Add(b, 2, r);
                }
                r++;
            }
            tblPre.ResumeLayout();
            foreach (var p in prereqs) Append("prerequisite " + p.Id + ": " + (p.Ok ? "ok (" + p.Found + ")" : "missing" + (p.Note != null ? " -- " + p.Note : "")));
        }

        void InstallPrereq(Prereq p)
        {
            string cmd = Prereqs.WingetCommand(p);
            string winget = Prereqs.WingetExe();
            if (winget == null)
            {
                MessageBox.Show(this, "winget (App Installer) is not available on this PC.\r\n\r\nInstall " + p.Name + " yourself, then press \"Check again\".", Text);
                return;
            }
            var ok = MessageBox.Show(this,
                p.Name + " is missing.\r\n\r\nInstall it with winget? This runs, in a console window you can watch:\r\n\r\n    " + cmd +
                "\r\n\r\nwinget downloads it from its publisher and may ask you to accept its licence and to allow the installer (UAC)." +
                (opts.WingetDryRun ? "\r\n\r\n(dry run: the command is only written to the log)" : ""),
                "Install " + p.Name, MessageBoxButtons.OKCancel, MessageBoxIcon.Question);
            if (ok != DialogResult.OK) { Append("install of " + p.Id + " declined"); return; }
            if (opts.WingetDryRun) { Append("[dry run] would run: " + cmd); return; }
            Append("running: " + cmd);
            try
            {
                var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "cmd.exe"),
                    "/c \"" + cmd + " & echo. & echo winget finished with exit code %ERRORLEVEL%. & pause\"") { UseShellExecute = false };
                using (var pr = Process.Start(psi))
                {
                    Enabled = false;
                    while (!pr.WaitForExit(200)) Application.DoEvents();
                    Enabled = true;
                    Append("winget exited with " + pr.ExitCode);
                }
            }
            catch (Exception ex) { Enabled = true; Append("winget failed to start: " + ex.Message); }
            DetectPrereqs();
            RefreshUi();
        }

        // ---------------------------------------------------------- UI state
        int FirstStale()
        {
            fps = pipe.Fingerprints();
            for (int i = 0; i < Pipeline.Steps.Length; i++)
                if (state.Get("done." + Pipeline.Steps[i].Id) != fps[i] || !pipe.MarkerOk(i)) return i;
            return Pipeline.Steps.Length;
        }

        void RefreshUi()
        {
            // files
            if (files.Present)
            {
                string e = files.TargetEboot, label = "unknown EBOOT";
                try { string n; if (Ps3Files.KnownEboots.TryGetValue(StateHash(e), out n)) label = n; } catch { }
                lblFiles.Text = "Disc: " + (state.Get("files.disc") ?? files.DiscGame) + "\r\nEBOOT: " + label +
                                "    libsre: " + (File.Exists(files.LibsrePath) ? "game\\firmware\\libsre.prx" : "lifted earlier");
                btnFiles.Text = "Change...";
            }
            else { lblFiles.Text = "Not set up yet: choose your disc, update 1.04, decrypted EBOOT.elf and libsre.prx."; btnFiles.Text = "Choose..."; }

            bool toolsOk = prereqs.Count > 0 && prereqs.All(p => p.Ok);
            bool running = game != null;
            btnCancel.Enabled = building;
            btnRebuild.Enabled = !building && !running && files.Present && toolsOk;
            btnFiles.Enabled = !building && !running;
            btnRecheck.Enabled = !building;
            lnkOldBuild.Visible = false; lblHint.Text = "";
            if (building) { btnMain.Text = "Building..."; btnMain.Enabled = false; return; }
            if (running) { btnMain.Text = "Running"; btnMain.Enabled = false; lblHint.Text = "The game is running. Close it to come back here."; return; }
            int stale = files.Present ? FirstStale() : 0;
            if (!files.Present) { btnMain.Text = "Build and play"; btnMain.Enabled = false; lblHint.Text = "Choose your game files first (1)."; }
            else if (stale == Pipeline.Steps.Length && File.Exists(ExePath))
            {
                btnMain.Text = "Play"; btnMain.Enabled = true;
                lblStep.Text = "Ready: the build is up to date.";
                bar.Value = 1000;
                lblHint.Text = "Built " + File.GetLastWriteTime(ExePath).ToString("g", CultureInfo.CurrentCulture) + ".";
            }
            else
            {
                btnMain.Text = state.Get("failed") != null ? "Retry" : stale > 0 ? "Continue build" : "Build and play";
                btnMain.Enabled = toolsOk;
                if (!toolsOk) lblHint.Text = "Install the missing build tools first (2).";
                else if (File.Exists(ExePath)) lnkOldBuild.Visible = true;
                lblStep.Text = stale == 0 ? "Not built yet (" + Pipeline.Steps.Length + " steps; the first build takes a while)."
                                          : "Next: step " + (stale + 1) + " of " + Pipeline.Steps.Length + ", " + Pipeline.Steps[stale].Title + ".";
                bar.Value = (int)(1000 * DoneWeight(stale) / TotalWeight());
            }
        }

        string StateHash(string path)
        {
            // the EBOOT's SHA-1, cached by identity (it is 10 MB)
            string id = Util.Identity(path);
            if (state.Get("eboot.id") == id && state.Get("eboot.sha1") != null) return state.Get("eboot.sha1");
            string h = Util.Sha1File(path);
            state.V["eboot.id"] = id; state.Set("eboot.sha1", h);
            return h;
        }

        static double TotalWeight() { return Pipeline.Steps.Sum(s => s.Weight); }
        static double DoneWeight(int upTo) { double w = 0; for (int i = 0; i < upTo; i++) w += Pipeline.Steps[i].Weight; return w; }

        void MainAction()
        {
            if (btnMain.Text == "Play") { LaunchGame(); return; }
            StartBuild(false);
        }

        void Rebuild()
        {
            if (MessageBox.Show(this, "Run every step again, from fetching the toolkit to compiling? (The first build's time again.)",
                                "Rebuild", MessageBoxButtons.OKCancel, MessageBoxIcon.Question) != DialogResult.OK) return;
            StartBuild(true);
        }

        // ---------------------------------------------------------- running the steps
        void StartBuild(bool all)
        {
            if (building) return;
            try
            {
                var drive = new DriveInfo(Path.GetPathRoot(root));
                if (drive.AvailableFreeSpace < 12L << 30 &&
                    MessageBox.Show(this, "Only " + (drive.AvailableFreeSpace >> 30) + " GB free on " + drive.Name + ". The build needs about 5 GB, " +
                                    "and the game's first start installs about 2.5 GB more. Continue?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                    return;
            }
            catch { }
            runFrom = all ? 0 : FirstStale();
            if (all) foreach (var s in Pipeline.Steps) state.V.Remove("done." + s.Id);
            state.Set("failed", null);
            if (runFrom >= Pipeline.Steps.Length) { LaunchGame(); return; }
            Directory.CreateDirectory(Path.Combine(root, "logs"));
            logPath = Path.Combine(root, "logs", "launcher-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
            logFile = new StreamWriter(logPath, false, new UTF8Encoding(false));
            building = true; cancelling = false;
            runStart = DateTime.Now;
            txtLog.Clear();
            Append("log file: " + logPath);
            if (runFrom > 0) Append("steps 1-" + runFrom + " are up to date: starting at step " + (runFrom + 1) + " (" + Pipeline.Steps[runFrom].Id + ")");
            RefreshUi();
            RunStep(runFrom);
        }

        void RunStep(int i)
        {
            stepIndex = i; stepFrac = 0; stepStart = DateTime.Now;
            var s = Pipeline.Steps[i];
            state.Set("done." + s.Id, null);
            for (int k = i + 1; k < Pipeline.Steps.Length; k++) state.V.Remove("done." + Pipeline.Steps[k].Id);
            state.Save();
            lblStep.Text = "Step " + (i + 1) + " of " + Pipeline.Steps.Length + ": " + s.Title;
            Append("");
            Append("===== step " + (i + 1) + "/" + Pipeline.Steps.Length + ": " + s.Id + " -- " + s.Title + " =====");
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"),
                "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + Path.Combine(root, @"tools\build.ps1") + "\" -Step " + s.Id)
            {
                UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = root,
                RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true,
                StandardOutputEncoding = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
                StandardErrorEncoding = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
            };
            psi.EnvironmentVariables["PATH"] = Prereqs.ChildPath(prereqs);
            psi.EnvironmentVariables["PYTHONUNBUFFERED"] = "1";
            psi.EnvironmentVariables["NINJA_STATUS"] = "[%f/%t] ";
            try
            {
                proc = new Process { StartInfo = psi, EnableRaisingEvents = true };
                proc.OutputDataReceived += (o, e) => { if (e.Data != null) Enqueue(e.Data); };
                proc.ErrorDataReceived += (o, e) => { if (e.Data != null) Enqueue(e.Data); };
                proc.Exited += (o, e) => BeginInvoke(new Action(() => StepExited(i)));
                proc.Start();
                proc.StandardInput.Close();
                proc.BeginOutputReadLine();
                proc.BeginErrorReadLine();
            }
            catch (Exception ex)
            {
                Append("could not start powershell: " + ex.Message);
                Finish(false, "could not start powershell.exe");
            }
        }

        void StepExited(int i)
        {
            if (proc == null) return;
            proc.WaitForExit();            // drains the output readers
            int code = proc.ExitCode;
            proc.Dispose(); proc = null;
            Flush();
            var s = Pipeline.Steps[i];
            Append("step " + s.Id + " ended with exit code " + code + " after " + Util.Elapsed(DateTime.Now - stepStart));
            if (cancelling) { Finish(false, null); return; }
            if (code != 0) { Finish(false, "Step " + (i + 1) + " (" + s.Title + ") failed."); return; }
            fps = pipe.Fingerprints();     // a step may create inputs of later ones (e.g. setup the toolkit)
            state.Set("done." + s.Id, fps[i]);
            if (i + 1 < Pipeline.Steps.Length) RunStep(i + 1);
            else Finish(true, null);
        }

        void Finish(bool ok, string error)
        {
            building = false;
            Append(ok ? "build finished in " + Util.Elapsed(DateTime.Now - runStart)
                      : cancelling ? "cancelled after " + Util.Elapsed(DateTime.Now - runStart) : "FAILED: " + error);
            if (!ok && !cancelling) state.Set("failed", Pipeline.Steps[Math.Max(0, stepIndex)].Id);
            if (logFile != null) { Flush(); logFile.Dispose(); logFile = null; }
            stepIndex = -1;
            RefreshUi();
            if (ok)
            {
                lblStep.Text = "Built. Starting the game...";
                LaunchGame();                 // "press play"
            }
            else if (!cancelling)
            {
                lblStep.Text = error;
                string t;
                lock (qLock) t = string.Join("\r\n", tail.Skip(Math.Max(0, tail.Count - 25)));
                MessageBox.Show(this, error + "\r\n\r\nThe last lines of the log:\r\n\r\n" + t + "\r\n\r\nFull log: " + logPath +
                                "\r\n\r\nFix the cause and press Retry: the steps that already finished are not run again.",
                                "Build failed", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            else lblStep.Text = "Cancelled. Press \"Continue build\" to go on from the step that was interrupted.";
        }

        void CancelRun()
        {
            if (!building || proc == null) return;
            if (MessageBox.Show(this, "Stop the build? Finished steps are kept; the current one starts over next time.", "Cancel",
                                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
            cancelling = true;
            Append("cancelling: ending the step's processes");
            try { Util.KillTree(proc.Id); } catch { }
        }

        void OnClosing(object sender, FormClosingEventArgs e)
        {
            if (building)
            {
                if (MessageBox.Show(this, "A build is running. Stop it and close?", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes)
                { e.Cancel = true; return; }
                cancelling = true;
                try { if (proc != null) Util.KillTree(proc.Id); } catch { }
            }
        }

        // ---------------------------------------------------------- the log pane
        void Enqueue(string line)
        {
            lock (qLock)
            {
                queue.Add(line);
                tail.Add(line);
                if (tail.Count > 400) tail.RemoveRange(0, 200);
            }
        }
        void Append(string line) { Enqueue(line); }

        void Flush()
        {
            List<string> lines;
            lock (qLock) { if (queue.Count == 0) return; lines = new List<string>(queue); queue.Clear(); }
            var sb = new StringBuilder();
            foreach (var l in lines)
            {
                sb.Append(l).Append("\r\n");
                if (logFile != null) logFile.WriteLine(l);
                // ninja's "[n/total]" lines: the build step's own progress
                if (stepIndex >= 0 && l.Length > 3 && l[0] == '[')
                {
                    int sl = l.IndexOf('/'), rb = l.IndexOf(']');
                    int n, t;
                    if (sl > 1 && rb > sl && int.TryParse(l.Substring(1, sl - 1), out n) && int.TryParse(l.Substring(sl + 1, rb - sl - 1), out t) && t > 0)
                        stepFrac = Math.Min(1.0, (double)n / t);
                }
            }
            if (logFile != null) logFile.Flush();
            if (txtLog.TextLength > 400000) { string keep = txtLog.Text.Substring(txtLog.TextLength - 200000); txtLog.Text = keep; }
            txtLog.AppendText(sb.ToString());
        }

        void OnTick()
        {
            Flush();
            if (building)
            {
                lblElapsed.Text = "elapsed " + Util.Elapsed(DateTime.Now - runStart) + "   (this step " + Util.Elapsed(DateTime.Now - stepStart) + ")";
                if (stepIndex >= 0)
                {
                    double w = DoneWeight(stepIndex) + Pipeline.Steps[stepIndex].Weight * stepFrac;
                    bar.Value = Math.Max(0, Math.Min(1000, (int)(1000 * w / TotalWeight())));
                }
            }
            else if (game != null)
            {
                lblElapsed.Text = "playing " + Util.Elapsed(DateTime.Now - gameStart);
                if (game.HasExited) GameExited();
            }
        }

        // ---------------------------------------------------------- the game
        // Started the way tools\run.ps1 starts it -- run.ps1 itself: the project folder as the working directory,
        // the lifted EBOOT as the argument, the runtime switches it sets, and its log in logs\run-<time>.log.
        // On top, for players: PS3_VERBOSE=0 (quieter log, as when the exe is double-clicked) and the play-session
        // issues file logs\play-<time>-issues.txt (tools\play_triage.py). Either one set beforehand wins.
        void LaunchGame()
        {
            if (game != null) return;
            if (!File.Exists(ExePath)) { MessageBox.Show(this, "There is no build\\infamous2.exe yet.", Text); return; }
            var running = Process.GetProcessesByName("infamous2").Where(p => SameExe(p)).ToList();
            if (running.Count > 0) { MessageBox.Show(this, "The game is already running (process " + running[0].Id + ").", Text); return; }
            string stamp = DateTime.Now.ToString("yyyyMMdd-HHmmss");
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, @"WindowsPowerShell\v1.0\powershell.exe"),
                "-NoProfile -ExecutionPolicy Bypass -File \"" + Path.Combine(root, @"tools\run.ps1") + "\"")
            { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = root };
            if (Environment.GetEnvironmentVariable("PS3_VERBOSE") == null) psi.EnvironmentVariables["PS3_VERBOSE"] = "0";
            if (Environment.GetEnvironmentVariable("PLAY_ISSUES") == null)
                psi.EnvironmentVariables["PLAY_ISSUES"] = Path.Combine(root, @"logs\play-" + stamp + "-issues.txt");
            Directory.CreateDirectory(Path.Combine(root, "logs"));
            try
            {
                game = Process.Start(psi);
                gameStart = DateTime.Now;
                state.Set("last.play", stamp);
                Append("");
                Append("starting the game: tools\\run.ps1 (log: logs\\run-" + stamp + ".log or the next second's)");
                lblStep.Text = "Playing. The launcher waits here until the game closes.";
                WindowState = FormWindowState.Minimized;
            }
            catch (Exception ex) { game = null; MessageBox.Show(this, "Could not start the game: " + ex.Message, Text); }
            RefreshUi();
        }

        bool SameExe(Process p)
        {
            try { return string.Equals(p.MainModule.FileName, ExePath, StringComparison.OrdinalIgnoreCase); }
            catch { return false; }
        }

        void GameExited()
        {
            int code = -1;
            try { code = game.ExitCode; } catch { }
            game.Dispose(); game = null;
            var newest = new DirectoryInfo(Path.Combine(root, "logs")).GetFiles("run-*.log").OrderByDescending(f => f.LastWriteTimeUtc).FirstOrDefault();
            Append("the game closed (exit code " + code + ")" + (newest != null ? "; its log: logs\\" + newest.Name : ""));
            if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
            RefreshUi();
            lblStep.Text = code == 0 ? "The game closed." : "The game ended with exit code " + code + ". Its log is in logs\\ (Open logs).";
        }

        static void OpenFolder(string dir, string select)
        {
            Directory.CreateDirectory(dir);
            Process.Start("explorer.exe", select != null ? "/select,\"" + select + "\"" : "\"" + dir + "\"");
        }

        // ---------------------------------------------------------- --detect-out
        public static string Report(string root, Options o)
        {
            var sb = new StringBuilder();
            sb.AppendLine("root: " + root);
            foreach (var p in Prereqs.Detect(o))
                sb.AppendLine("prereq " + p.Id + ": " + (p.Ok ? "ok " + p.Found : "MISSING" + (p.Note != null ? " (" + p.Note + ")" : "")) + "  [winget: " + Prereqs.WingetCommand(p) + "]");
            sb.AppendLine("winget: " + (Prereqs.WingetExe() ?? "not found"));
            var gf = new GameFiles(root);
            sb.AppendLine("game files present: " + gf.Present + "  target EBOOT: " + (gf.TargetEboot ?? "none"));
            if (gf.Present)
            {
                var pl = new Pipeline(root, gf);
                var st = new State(root);
                var fp = pl.Fingerprints();
                for (int i = 0; i < Pipeline.Steps.Length; i++)
                    sb.AppendLine("step " + Pipeline.Steps[i].Id + ": " + (st.Get("done." + Pipeline.Steps[i].Id) == fp[i] && pl.MarkerOk(i) ? "up to date" : "to run"));
            }
            return sb.ToString();
        }
    }

    static class Program
    {
        static string FindRoot()
        {
            string d = Path.GetDirectoryName(Application.ExecutablePath);
            for (int up = 0; up < 3 && d != null; up++)
            {
                if (File.Exists(Path.Combine(d, @"tools\build.ps1")) && File.Exists(Path.Combine(d, "ps3recomp.pin"))) return d;
                d = Path.GetDirectoryName(d);
            }
            return null;
        }

        [STAThread]
        static int Main(string[] args)
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            var o = Options.Parse(args);
            string root = FindRoot();
            if (root == null)
            {
                MessageBox.Show("Put inFamous2Launcher.exe in the inFamous 2 recomp folder (the one with tools\\build.ps1 and ps3recomp.pin).",
                                "inFamous 2 launcher", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return 1;
            }
            if (o.DetectOut != null)
            {
                File.WriteAllText(o.DetectOut, MainForm.Report(root, o));
                return 0;
            }
            Application.Run(new MainForm(root, o));
            return 0;
        }
    }
}
