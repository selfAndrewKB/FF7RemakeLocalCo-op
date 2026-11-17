// ETW dumper for the perf captures (TraceProcessor 1.12.10): CPU samples and stacks, context switches, GPU (DxgKrnl
// generic events + DirectX DMA packets), DPC/ISR and hard-fault data, images/sysproc/sysmeta dumps, all-process cswitch
// (cswitch_all.tsv.gz), and Windows symbols for non-game modules when TP_SYMDIR is set (symbol server; ~0.5 GB of PDBs on
// first use). The game exe is the 4th argument (default ff7remake_.exe).
// usage: tp2 <etl> <outDir> <gamePid> [gameExe]
// Writes: meta.txt, modules.tsv, threads.tsv, allthreads.tsv, stacks.tsv.gz, samples.tsv.gz, procsec.tsv,
//         cswitch.tsv.gz (game pid), dxg.tsv.gz + dxg_census.tsv (DxgKrnl), dma.tsv.gz (UseDirectXData),
//         dpc.tsv.gz / isr.tsv.gz (reflected rows), hardfault.tsv (per process), schema.txt (reflected property names)
using System.IO.Compression;
using System.Reflection;
using System.Text;
using Microsoft.Windows.EventTracing;
using Microsoft.Windows.EventTracing.Cpu;
using Microsoft.Windows.EventTracing.Events;
using Microsoft.Windows.EventTracing.Processes;
using Microsoft.Windows.EventTracing.Symbols;

var etl = args[0]; var outDir = args[1]; int gamePid = int.Parse(args[2]); var gameExe = args.Length > 3 ? args[3] : "ff7remake_.exe";
Directory.CreateDirectory(outDir);
var DXG = new Guid("802ec45a-1e99-4b83-9920-87c98277ba9d");

var settings = new TraceProcessorSettings { AllowLostEvents = true, SuppressFirstTimeSetupMessage = true, AllowTimeInversion = true };
using var tp = TraceProcessor.Create(etl, settings);
var pMeta = tp.UseMetadata();
var pProc = tp.UseProcesses();
var pThr = tp.UseThreads();
var pSamp = tp.UseCpuSamplingData();
var pSched = tp.UseCpuSchedulingData();
var pGen = tp.UseGenericEvents(new[] { DXG });
IPendingResult<Microsoft.Windows.EventTracing.Graphics.IDirectXDataSource>? pDx = null;
try { pDx = tp.UseDirectXData(); } catch (Exception e) { Console.Error.WriteLine("UseDirectXData: " + e.Message); }
var pIsr = tp.UseInterruptHandlingData();
var pHf = tp.UseHardFaults();
var pSys = tp.UseSystemMetadata();
var pSym = tp.UseSymbols();
var symDir = Environment.GetEnvironmentVariable("TP_SYMDIR");
var sw = System.Diagnostics.Stopwatch.StartNew();
tp.Process(new Progress<TraceProcessingProgress>(p => { }));
Console.Error.WriteLine($"processed in {sw.Elapsed}");
bool useSym = !string.IsNullOrEmpty(symDir);
if (useSym) {
  var sp = new SymbolPath(new[] { $"srv*{symDir}/pdb*https://msdl.microsoft.com/download/symbols" });
  var sc = new SymCachePath(symDir + "/symcache", Array.Empty<string>());
  var excl = new[] { gameExe, "main.dll", "UE4SS.dll", "nvwgf2umx.dll", "nvldumdx.dll", "sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll", "GameOverlayRenderer64.dll", "dwmapi.dll" };
  try { await pSym.Result.LoadSymbolsAsync(sc, sp, new Progress<SymbolLoadingProgress>(p => { }), new[] { gameExe }, excl); Console.Error.WriteLine($"symbols loaded at {sw.Elapsed}"); }
  catch (Exception e) { Console.Error.WriteLine("symbol load failed: " + e.Message); useSym = false; }
}

GZipStream Gz(string n) => new GZipStream(File.Create(Path.Combine(outDir, n)), CompressionLevel.Fastest);
var schema = new StreamWriter(Path.Combine(outDir, "schema.txt"));

// ---------- reflection row writer ----------
string Fmt(object? v) {
  if (v == null) return "";
  switch (v) {
    case string s: return s.Replace('\t', ' ').Replace('\n', ' ');
    case bool or int or long or uint or ulong or short or ushort or byte or sbyte or double or float or Enum or Guid: return v.ToString()!;
  }
  var t = v.GetType();
  foreach (var pn in new[] { "Nanoseconds", "Id", "Value" }) {
    var pi = t.GetProperty(pn);
    if (pi != null) { try { var x = pi.GetValue(v); if (x is IConvertible) return Convert.ToString(x, System.Globalization.CultureInfo.InvariantCulture)!; } catch { } }
  }
  var fnp = t.GetProperty("FileName"); if (fnp != null) { try { return fnp.GetValue(v)?.ToString() ?? ""; } catch { } }
  return "";
}
List<PropertyInfo> PropsOf(Type t) {
  var ps = new List<PropertyInfo>();
  var seen = new HashSet<string>();
  foreach (var it in new[] { t }.Concat(t.GetInterfaces()))
    foreach (var p in it.GetProperties(BindingFlags.Public | BindingFlags.Instance))
      if (p.GetIndexParameters().Length == 0 && seen.Add(p.Name) && !p.PropertyType.Name.Contains("Stack")) ps.Add(p);
  return ps;
}
long DumpObjs<T>(string file, IEnumerable<T> items, Func<T, bool>? filter = null) {
  long n = 0; List<PropertyInfo>? ps = null;
  using var w = new StreamWriter(Gz(file));
  foreach (var o in items) {
    if (o == null) continue;
    if (filter != null && !filter(o)) continue;
    if (ps == null) {
      ps = PropsOf(o.GetType());
      schema.WriteLine($"{file}: {o.GetType().FullName}: " + string.Join(", ", ps.Select(p => p.Name + ":" + p.PropertyType.Name)));
      w.WriteLine(string.Join('\t', ps.Select(p => p.Name)));
    }
    w.WriteLine(string.Join('\t', ps.Select(p => { try { return Fmt(p.GetValue(o)); } catch { return "!"; } })));
    n++;
  }
  return n;
}

var meta = pMeta;
using (var w = new StreamWriter(Path.Combine(outDir, "meta.txt"))) {
  w.WriteLine($"StartTime\t{meta.StartTime:o}\nStopTime\t{meta.StopTime:o}\nFirstEvent\t{meta.FirstAnalyzerDisplayedEventTime.DateTimeOffset:o}\t{meta.FirstAnalyzerDisplayedEventTime.Nanoseconds}\nLastEvent\t{meta.LastEventTime.DateTimeOffset:o}\t{meta.LastEventTime.Nanoseconds}");
  w.WriteLine($"Processors\t{meta.ProcessorCount}\nLostEvents\t{meta.LostEventCount}\nLostBuffers\t{meta.LostBufferCount}\nOS\t{meta.OSVersion}");
}

var gamePids = new HashSet<int> { gamePid };
var procs = pProc.Result.Processes.Where(p => p.Id == gamePid).ToList();
Console.Error.WriteLine($"game processes: {string.Join(",", procs.Select(p => p.Id + ":" + p.ImageName))}");

var mods = new Dictionary<(string, long), int>();
var modList = new List<string>();
int ModIdx(IImage img) {
  var key = (img.Path ?? img.FileName ?? "?", img.AddressRange.BaseAddress.Value);
  if (!mods.TryGetValue(key, out var i)) {
    i = modList.Count; mods[key] = i;
    modList.Add($"{i}\t{img.FileName}\t0x{img.AddressRange.BaseAddress.Value:X}\t0x{img.AddressRange.Size.Bytes:X}\t{img.ProcessId}\t{img.Path}\t");
  }
  return i;
}
foreach (var p in procs) foreach (var img in p.Images) ModIdx(img);
try { Console.Error.WriteLine($"images {DumpObjs("images.tsv.gz", procs.SelectMany(p => p.Images))}"); } catch (Exception ex) { Console.Error.WriteLine("images: " + ex.Message); }
try { Console.Error.WriteLine($"cpus {DumpObjs("sysproc.tsv.gz", pSys.Result.Processors)}"); } catch (Exception ex) { Console.Error.WriteLine("sysproc: " + ex.Message); }
try { DumpObjs("sysmeta.tsv.gz", new[] { pSys.Result }); } catch (Exception ex) { Console.Error.WriteLine("sysmeta: " + ex.Message); }

var stacks = new Dictionary<string, int>();
var sb = new StringBuilder();
int StackId(IStackSnapshot? s) {
  if (s == null || s.Frames == null || s.Frames.Count == 0) return -1;
  sb.Clear();
  foreach (var f in s.Frames) {
    if (sb.Length > 0) sb.Append(';');
    if (f.Image == null) { sb.Append('?').Append(f.Address.Value.ToString("x")); continue; }
    sb.Append(ModIdx(f.Image)).Append('+').Append(f.RelativeVirtualAddress.Value.ToString("x"));
    if (useSym && !string.Equals(f.Image.FileName, gameExe, StringComparison.OrdinalIgnoreCase)) {
      string? fn = null; try { fn = f.Symbol?.FunctionName; } catch { }
      if (!string.IsNullOrEmpty(fn)) sb.Append('|').Append(fn.Replace(';', ',').Replace('	', ' '));
    }
  }
  var k = sb.ToString();
  if (!stacks.TryGetValue(k, out var id)) { id = stacks.Count; stacks[k] = id; }
  return id;
}

// ---------- CPU samples ----------
var procSec = new Dictionary<(string, int, long), int>();
long nSamp = 0, nGame = 0;
using (var w = new StreamWriter(Gz("samples.tsv.gz"))) {
  w.WriteLine("t_ns\ttid\tcpu\tstack");
  foreach (var s in pSamp.Result.Samples) {
    nSamp++;
    var pid = s.Process?.Id ?? -1;
    var key = (s.Process?.ImageName ?? "?", pid, s.Timestamp.Nanoseconds / 1_000_000_000);
    procSec[key] = procSec.GetValueOrDefault(key) + 1;
    if (!gamePids.Contains(pid)) continue;
    nGame++;
    w.WriteLine($"{s.Timestamp.Nanoseconds}\t{s.Thread?.Id ?? -1}\t{s.Processor}\t{StackId(s.Stack)}");
  }
}
Console.Error.WriteLine($"samples total {nSamp}, game {nGame}, at {sw.Elapsed}");
using (var w = new StreamWriter(Path.Combine(outDir, "procsec.tsv"))) {
  w.WriteLine("image\tpid\tsec\tsamples");
  foreach (var kv in procSec.OrderBy(k => k.Key.Item2).ThenBy(k => k.Key.Item3)) w.WriteLine($"{kv.Key.Item1}\t{kv.Key.Item2}\t{kv.Key.Item3}\t{kv.Value}");
}

// ---------- context switches (game pid) + per-process run totals (all) ----------
long nCs = 0;
var procRun = new Dictionary<(string, int), long>();
using var wall = new StreamWriter(Gz("cswitch_all.tsv.gz"));
wall.WriteLine("start_ns	stop_ns	pid	tid	cpu	ready_ns	prio");
using (var w = new StreamWriter(Gz("cswitch.tsv.gz"))) {
  w.WriteLine("start_ns\tstop_ns\ttid\tcpu\twait_ns\tready_ns\tready_ts_ns\treadying_pid\treadying_tid\tprev_wait_reason\tprev_state\tin_prio\tin_stack\tready_stack");
  foreach (var a in pSched.Result.ThreadActivity) {
    var pid = a.Process?.Id ?? -1;
    var k = (a.Process?.ImageName ?? "?", pid);
    procRun[k] = procRun.GetValueOrDefault(k) + (a.StopTime.Nanoseconds - a.StartTime.Nanoseconds);
    wall.WriteLine($"{a.StartTime.Nanoseconds}	{a.StopTime.Nanoseconds}	{pid}	{a.Thread?.Id ?? -1}	{a.Processor}	{a.ReadyDuration?.Nanoseconds ?? -1}	{a.SwitchIn?.Priority?.ToString() ?? ""}");
    if (!gamePids.Contains(pid)) continue;
    nCs++;
    var prev = a.PreviousActivity?.SwitchOut;
    long rts = -1; try { if (a.ReadyThreadEvent != null) rts = a.ReadyThreadEvent.Timestamp.Nanoseconds; } catch { }
    w.WriteLine(string.Join('\t', a.StartTime.Nanoseconds, a.StopTime.Nanoseconds, a.Thread?.Id ?? -1, a.Processor,
      a.WaitingDuration?.Nanoseconds ?? -1, a.ReadyDuration?.Nanoseconds ?? -1, rts,
      a.ReadyingProcess?.Id ?? -1, a.ReadyingThread?.Id ?? -1,
      prev?.WaitReason?.ToString() ?? "", prev?.ThreadState?.ToString() ?? "", a.SwitchIn?.Priority?.ToString() ?? "",
      StackId(a.SwitchIn?.Stack), StackId(a.ReadyThreadStack)));
  }
}
wall.Close();
using (var w = new StreamWriter(Path.Combine(outDir, "procrun.tsv"))) {
  w.WriteLine("image\tpid\trun_ns");
  foreach (var kv in procRun.OrderByDescending(k => k.Value)) w.WriteLine($"{kv.Key.Item1}\t{kv.Key.Item2}\t{kv.Value}");
}
Console.Error.WriteLine($"cswitch game {nCs} at {sw.Elapsed}");

// ---------- DxgKrnl generic events ----------
var census = new Dictionary<(int, string, string, int), long>();
var fieldProp = new Dictionary<Type, Dictionary<string, PropertyInfo?>>();
string FieldVal(object f) {
  var t = f.GetType();
  if (!fieldProp.TryGetValue(t, out var d)) { d = new(); fieldProp[t] = d; }
  string ty = "";
  try { ty = t.GetProperty("Type")?.GetValue(f)?.ToString() ?? ""; } catch { }
  if (!d.TryGetValue(ty, out var pi)) { pi = t.GetProperty("As" + ty); d[ty] = pi; }
  try {
    if (pi != null) return Fmt(pi.GetValue(f));
    return f.ToString() ?? "";
  } catch { return "?"; }
}
long nDxg = 0;
try {
  using var w = new StreamWriter(Gz("dxg.tsv.gz"));
  w.WriteLine("t_ns\tid\ttask\topcode\tpid\ttid\tcpu\tfields");
  bool first = true;
  foreach (var e in pGen.Result.Events) {
    if (e.ProviderId != DXG) continue;
    var key = (e.Id, e.TaskName ?? "", e.OpcodeName ?? "", e.ProcessId == gamePid ? 1 : 0);
    census[key] = census.GetValueOrDefault(key) + 1;
    if (first) { first = false; schema.WriteLine("generic field type: " + (e.Fields.Count > 0 ? e.Fields[0].GetType().FullName + " props " + string.Join(",", PropsOf(e.Fields[0].GetType()).Select(p => p.Name)) : "")); }
    var fs = new StringBuilder();
    foreach (var f in e.Fields) {
      string nm = ""; try { nm = f.GetType().GetProperty("Name")?.GetValue(f)?.ToString() ?? ""; } catch { }
      if (fs.Length > 0) fs.Append(';');
      fs.Append(nm).Append('=').Append(FieldVal(f));
    }
    w.WriteLine($"{e.Timestamp.Nanoseconds}\t{e.Id}\t{e.TaskName}\t{e.OpcodeName}\t{e.ProcessId}\t{e.ThreadId}\t{e.Processor}\t{fs}");
    nDxg++;
  }
} catch (Exception ex) { Console.Error.WriteLine("dxg: " + ex); }
using (var w = new StreamWriter(Path.Combine(outDir, "dxg_census.tsv"))) {
  w.WriteLine("id\ttask\topcode\tgamepid\tcount");
  foreach (var kv in census.OrderByDescending(k => k.Value)) w.WriteLine($"{kv.Key.Item1}\t{kv.Key.Item2}\t{kv.Key.Item3}\t{kv.Key.Item4}\t{kv.Value}");
}
Console.Error.WriteLine($"dxg {nDxg} at {sw.Elapsed}");

// ---------- DirectX DMA packets ----------
try { if (pDx != null) Console.Error.WriteLine($"dma {DumpObjs("dma.tsv.gz", pDx.Result.DmaPackets)} at {sw.Elapsed}"); }
catch (Exception ex) { Console.Error.WriteLine("dma: " + ex.Message); }
// ---------- DPC / ISR ----------
try { Console.Error.WriteLine($"dpc {DumpObjs("dpc.tsv.gz", pIsr.Result.DeferredProcedureCalls)} at {sw.Elapsed}"); } catch (Exception ex) { Console.Error.WriteLine("dpc: " + ex.Message); }
try { Console.Error.WriteLine($"isr {DumpObjs("isr.tsv.gz", pIsr.Result.InterruptServiceRoutines)} at {sw.Elapsed}"); } catch (Exception ex) { Console.Error.WriteLine("isr: " + ex.Message); }
// ---------- hard faults ----------
try { Console.Error.WriteLine($"hardfault {DumpObjs("hardfault.tsv.gz", pHf.Result.Faults)} at {sw.Elapsed}"); } catch (Exception ex) { Console.Error.WriteLine("hf: " + ex.Message); }

// ---------- threads, modules, stacks ----------
using (var w = new StreamWriter(Path.Combine(outDir, "threads.tsv"))) {
  w.WriteLine("pid\ttid\tname\tcreate_ns\texit_ns\tstart_addr\tstart_frame\tbase_prio");
  foreach (var t in pThr.Result.Threads) {
    if (!gamePids.Contains(t.ProcessId)) continue;
    string sf = "";
    try { var f = t.StartFrame; if (f.HasValue && f.Image != null) sf = $"{ModIdx(f.Image)}+{f.RelativeVirtualAddress.Value:x}|{f.Image.FileName}"; } catch { }
    w.WriteLine($"{t.ProcessId}\t{t.Id}\t{t.Name}\t{t.CreateTime?.Nanoseconds}\t{t.ExitTime?.Nanoseconds}\t0x{t.StartAddress.Value:X}\t{sf}\t{t.BasePriority}");
  }
}
using (var w = new StreamWriter(Path.Combine(outDir, "allthreads.tsv"))) {
  w.WriteLine("pid\tproc\ttid\tname");
  foreach (var t in pThr.Result.Threads) w.WriteLine($"{t.ProcessId}\t{t.Process?.ImageName}\t{t.Id}\t{t.Name}");
}
using (var w = new StreamWriter(Gz("stacks.tsv.gz"))) {
  foreach (var kv in stacks) w.WriteLine($"{kv.Value}\t{kv.Key}");
}
File.WriteAllLines(Path.Combine(outDir, "modules.tsv"), new[] { "idx\tname\tbase\tsize\tpid\tpath\tpdb" }.Concat(modList));
schema.Close();
Console.Error.WriteLine($"done: stacks {stacks.Count} modules {modList.Count} at {sw.Elapsed}");
