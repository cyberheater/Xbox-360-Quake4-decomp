// Quake4-Setup.exe - installer for the Xbox 360 Quake 4 static recompilation.
//
// Carries this repository's source (setup script, project, SDK patches) - no game files and no
// game code. It unpacks into a folder you choose and runs setup.ps1, which builds the game on
// your PC from YOUR OWN copy of Quake 4 (ISO or default.xex).
//
// Build (see build_installer.ps1): csc with the repository zipped as the embedded resource "repo.zip".
// Command line: Quake4-Setup.exe [--dir <folder>] [setup.ps1 options, e.g. -GamePath <iso> -NoPause]

using System;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Windows.Forms;

[assembly: AssemblyTitle("Quake 4 (Xbox 360) for PC - Setup")]
[assembly: AssemblyProduct("Xbox-360-Quake4-decomp")]
[assembly: AssemblyVersion("0.2.0.0")]
[assembly: AssemblyFileVersion("0.2.0.0")]
[assembly: AssemblyInformationalVersion("0.2.0")]

static class Quake4Setup {
    const string DefaultDir = @"C:\Quake4Recomp";

    [STAThread]
    static int Main(string[] args) {
        Console.Title = "Quake 4 (Xbox 360) for PC - Setup 0.2.0";
        Console.WriteLine("Quake 4 (Xbox 360) for PC - Setup 0.2.0");
        Console.WriteLine("=======================================");
        Console.WriteLine();
        Console.WriteLine("This installs the project and builds the game on this PC from YOUR OWN copy");
        Console.WriteLine("of the Xbox 360 version of Quake 4 (your ISO, or default.xex from an extracted");
        Console.WriteLine("copy). It contains no game files and no game code.");
        Console.WriteLine();

        string dir = null;
        var passthrough = new System.Collections.Generic.List<string>();
        for (int i = 0; i < args.Length; i++) {
            if (string.Equals(args[i], "--dir", StringComparison.OrdinalIgnoreCase) && i + 1 < args.Length) {
                dir = args[++i];
            } else {
                passthrough.Add(args[i]);
            }
        }

        if (dir == null) dir = AskFolder();
        if (dir == null) return Finish("No folder chosen.", 1, passthrough);
        if (dir.IndexOf(' ') >= 0) {
            return Finish("Please choose a folder whose path has no spaces (for example " + DefaultDir + ").", 1, passthrough);
        }

        try {
            Directory.CreateDirectory(dir);
            Console.WriteLine("Unpacking into " + dir + " ...");
            using (Stream s = Assembly.GetExecutingAssembly().GetManifestResourceStream("repo.zip"))
            using (var zip = new ZipArchive(s, ZipArchiveMode.Read)) {
                string root = Path.GetFullPath(dir);
                foreach (ZipArchiveEntry e in zip.Entries) {
                    string dest = Path.GetFullPath(Path.Combine(root, e.FullName));
                    if (!dest.StartsWith(root, StringComparison.OrdinalIgnoreCase)) continue;
                    if (string.IsNullOrEmpty(e.Name)) { Directory.CreateDirectory(dest); continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(dest));
                    e.ExtractToFile(dest, true);
                }
            }
        } catch (Exception ex) {
            return Finish("Could not unpack: " + ex.Message, 1, passthrough);
        }

        string script = Path.Combine(dir, "setup.ps1");
        string psArgs = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\"";
        foreach (string a in passthrough) psArgs += " \"" + a.Replace("\"", "\\\"") + "\"";
        Console.WriteLine("Starting the setup...");
        var psi = new ProcessStartInfo("powershell.exe", psArgs) { UseShellExecute = false, WorkingDirectory = dir };
        try {
            using (Process p = Process.Start(psi)) {
                p.WaitForExit();
                return p.ExitCode;
            }
        } catch (Exception ex) {
            return Finish("Could not start PowerShell: " + ex.Message, 1, passthrough);
        }
    }

    static string AskFolder() {
        Console.WriteLine("Choose where to install (default " + DefaultDir + ").");
        using (var dlg = new FolderBrowserDialog()) {
            dlg.Description = "Choose a folder for Quake 4 (Xbox 360) for PC. About 15 GB of free space is needed while building.\n" +
                              "Use a path without spaces, for example " + DefaultDir + ".";
            dlg.ShowNewFolderButton = true;
            try { Directory.CreateDirectory(DefaultDir); } catch { }
            dlg.SelectedPath = DefaultDir;
            if (dlg.ShowDialog() != DialogResult.OK) return null;
            return dlg.SelectedPath;
        }
    }

    static int Finish(string message, int code, System.Collections.Generic.List<string> passthrough) {
        Console.WriteLine();
        Console.WriteLine(message);
        if (!passthrough.Exists(a => string.Equals(a, "-NoPause", StringComparison.OrdinalIgnoreCase)) && !Console.IsInputRedirected) {
            Console.WriteLine("Press Enter to close.");
            Console.ReadLine();
        }
        return code;
    }
}
