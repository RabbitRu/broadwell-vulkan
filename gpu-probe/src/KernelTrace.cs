using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Web.Script.Serialization;

namespace IrisProbe
{
    // All inspection is restricted to this probe's address space.
    internal sealed partial class KernelTrace : IDisposable
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr LoadLibrary(string name);
        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, ExactSpelling = true)] private static extern IntPtr GetProcAddress(IntPtr module, string name);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint type, uint protection);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool VirtualProtect(IntPtr address, UIntPtr size, uint protection, out uint previous);
        [DllImport("kernel32.dll")] private static extern bool FlushInstructionCache(IntPtr process, IntPtr address, UIntPtr size);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] private delegate int Thunk(IntPtr data);
        private sealed class Hook
        {
            internal string Name;
            internal IntPtr Address;
            internal byte[] OriginalBytes;
            internal Thunk Original, Callback;
        }
        private static readonly string[] names = { "CreateDevice", "CreateContext", "CreateContextVirtual", "CreateAllocation", "SubmitCommand", "Render", "MapGpuVirtualAddress", "MakeResident", "CreateSynchronizationObject" };
        private readonly List<Hook> hooks = new List<Hook>();
        // Keep callbacks/trampolines rooted until process exit, including native calls in flight.
        private static readonly List<Hook> retained = new List<Hook>();
        private readonly object gate = new object();
        private readonly string directory;
        private readonly StreamWriter writer;
        private int sequence;
        private bool disposed;
        [ThreadStatic] private static bool inside;
        internal static string Phase = "initialization";
        internal static uint LastContext;
        internal static uint LastRenderDevice;
        internal static byte[] LastSubmitHeader;
        internal static byte[] Buffer8192Template, Buffer4096Template;

        internal KernelTrace(string output)
        {
            directory = Path.Combine(output, "wddm"); Directory.CreateDirectory(directory);
            writer = new StreamWriter(Path.Combine(directory, "calls.jsonl"), false);
            writer.AutoFlush = true;
            try
            {
                IntPtr module = LoadLibrary("win32u.dll");
                foreach (string name in names) Install(module, name);
                Console.WriteLine("WDDM trace enabled for {0} entry points (this process only)", hooks.Count);
            }
            catch { Dispose(); throw; }
        }
        private void Install(IntPtr module, string name)
        {
            IntPtr address = GetProcAddress(module, "NtGdiDdDDI" + name);
            if (address == IntPtr.Zero) return;
            byte[] code = new byte[32]; Marshal.Copy(address, code, 0, code.Length);
            // Only support the observed, self-contained Windows x64 syscall stub.
            byte[] expected = { 0x4c,0x8b,0xd1,0xb8,0,0,0,0,0xf6,0x04,0x25,0x08,0x03,0xfe,0x7f,0x01,0x75,0x03,0x0f,0x05,0xc3,0xcd,0x2e,0xc3,0x0f,0x1f,0x84,0,0,0,0,0 };
            for (int i = 0; i < code.Length; i++) if ((i < 4 || i > 7) && code[i] != expected[i])
                throw new InvalidOperationException("Unsupported syscall entry layout: " + name);
            IntPtr trampoline = VirtualAlloc(IntPtr.Zero, (UIntPtr)32, 0x3000, 4);
            if (trampoline == IntPtr.Zero) throw new InvalidOperationException("Cannot allocate trace trampoline");
            Marshal.Copy(code, 0, trampoline, 32);
            uint previous;
            if (!VirtualProtect(trampoline, (UIntPtr)32, 0x20, out previous)) throw new InvalidOperationException("Cannot finalize trace trampoline");
            FlushInstructionCache(new IntPtr(-1), trampoline, (UIntPtr)32);
            var hook = new Hook { Name = name, Address = address, OriginalBytes = code };
            hook.Original = (Thunk)Marshal.GetDelegateForFunctionPointer(trampoline, typeof(Thunk));
            hook.Callback = delegate(IntPtr data) { return Invoke(hook, data); };
            // Force both managed thunks to JIT before any native entry point changes.
            IntPtr callback = Marshal.GetFunctionPointerForDelegate(hook.Callback);
            byte[] jump = new byte[12]; jump[0] = 0x48; jump[1] = 0xb8;
            Array.Copy(BitConverter.GetBytes(callback.ToInt64()), 0, jump, 2, 8); jump[10] = 0xff; jump[11] = 0xe0;
            retained.Add(hook);
            WriteCode(address, jump); hooks.Add(hook);
        }
        private static void WriteCode(IntPtr address, byte[] code)
        {
            uint previous;
            if (!VirtualProtect(address, (UIntPtr)(uint)code.Length, 0x40, out previous)) throw new InvalidOperationException("Cannot write local trace entry point");
            try { Marshal.Copy(code, 0, address, code.Length); }
            finally { uint ignored; VirtualProtect(address, (UIntPtr)(uint)code.Length, previous, out ignored); FlushInstructionCache(new IntPtr(-1), address, (UIntPtr)(uint)code.Length); }
        }
        private int Invoke(Hook hook, IntPtr data)
        {
            if (inside) return hook.Original(data);
            inside = true;
            try
            {
                int id = Interlocked.Increment(ref sequence);
                var entry = new Dictionary<string, object> { { "id", id }, { "phase", Phase }, { "function", hook.Name }, { "thread", Thread.CurrentThread.ManagedThreadId } };
                try { Describe(entry, id, hook.Name, data, false); }
                catch (Exception error) { entry["captureError"] = error.Message; }
                if (hook.Name == "CreateAllocation" && Marshal.ReadInt32(data, 44) == 1)
                {
                    IntPtr allocation = Marshal.ReadIntPtr(data, 48);
                    if (Marshal.ReadInt32(allocation, 24) == 1096)
                    {
                        byte[] blob = new byte[1096]; Marshal.Copy(Marshal.ReadIntPtr(allocation, 16), blob, 0, blob.Length);
                        if (BitConverter.ToUInt32(blob, 8) == 6 && BitConverter.ToUInt32(blob, 20) == 0x401080 && BitConverter.ToUInt32(blob, 28) == 0x40008)
                        {
                            if (BitConverter.ToUInt64(blob, 40) == 8192) Buffer8192Template = blob;
                            if (BitConverter.ToUInt64(blob, 40) == 4096) Buffer4096Template = blob;
                        }
                    }
                }
                Log(entry);
                if (CaptureComputeEnabled && hook.Name == "SubmitCommand" && Phase == "compute")
                {
                    try { CaptureCompute(data); }
                    catch (Exception error) { Log(new Dictionary<string, object> { { "captureComputeError", error.ToString() } }); }
                }
                List<CapturedAllocation> pendingAllocations = CaptureComputeEnabled && hook.Name == "CreateAllocation" ? CaptureAllocationInputs(data) : null;
                int status = hook.Original(data);
                if (CaptureComputeEnabled && status >= 0) TrackComputeCall(hook.Name, data, pendingAllocations);
                if (status == 0 && hook.Name == "CreateContextVirtual" && Marshal.ReadInt32(data, 12) == 0)
                    LastRenderDevice = unchecked((uint)Marshal.ReadInt32(data, 0));
                if (status == 0 && hook.Name == "SubmitCommand" && Marshal.ReadInt32(data, 24) == 1 && Marshal.ReadInt32(data, 296) == 80)
                {
                    byte[] header = new byte[80]; Marshal.Copy(Marshal.ReadIntPtr(data, 288), header, 0, 80);
                    LastContext = unchecked((uint)Marshal.ReadInt32(data, 28)); LastSubmitHeader = header;
                }
                var result = new Dictionary<string, object> { { "id", id }, { "return", "0x" + status.ToString("X8") } };
                if (status >= 0)
                {
                    try { Describe(result, id, hook.Name, data, true); }
                    catch (Exception error) { result["captureError"] = error.Message; }
                }
                Log(result);
                return status;
            }
            finally { inside = false; }
        }
        private void Log(Dictionary<string, object> entry)
        { lock (gate) { if (!disposed) writer.WriteLine(new JavaScriptSerializer().Serialize(entry)); } }
        private object Dump(int id, string name, IntPtr pointer, int bytes)
        {
            if (pointer == IntPtr.Zero || bytes == 0) return null;
            if (bytes < 0 || bytes > 65536) return "capture size outside 1..65536: " + bytes;
            byte[] data = new byte[bytes]; Marshal.Copy(pointer, data, 0, bytes);
            string file = id.ToString("D5") + "-" + name + ".bin";
            File.WriteAllBytes(Path.Combine(directory, file), data);
            return new Dictionary<string, object> { { "file", file }, { "bytes", bytes }, { "sha256", Program.Hash(data) }, { "prefix", BitConverter.ToString(data, 0, Math.Min(64, bytes)) } };
        }
        private void Describe(Dictionary<string, object> entry, int id, string name, IntPtr data, bool returned)
        {
            if (data == IntPtr.Zero) return;
            int size = name == "CreateDevice" ? 64 : name == "CreateContext" ? 96 : name == "CreateContextVirtual" ? 40 : name == "CreateAllocation" ? 72 : name == "SubmitCommand" ? 384 : name == "Render" ? 368 : name == "MapGpuVirtualAddress" ? 104 : name == "MakeResident" ? 48 : name == "CreateSynchronizationObject" ? 96 : 8;
            string suffix = returned ? "out" : "in";
            entry["arguments"] = Dump(id, "args-" + suffix, data, size);
            if (name == "CreateContext" || name == "CreateContextVirtual")
            {
                entry["device"] = Marshal.ReadInt32(data, 0);
                entry["context"] = Marshal.ReadInt32(data, 32);
                entry["privateData"] = Dump(id, "context-private-" + suffix, Marshal.ReadIntPtr(data, 16), Marshal.ReadInt32(data, 24));
            }
            else if (name == "CreateAllocation")
            {
                entry["device"] = Marshal.ReadInt32(data, 0);
                entry["flags"] = Marshal.ReadInt32(data, 56).ToString("X8");
                entry["privateData"] = Dump(id, "resource-private-" + suffix, Marshal.ReadIntPtr(data, 32), Marshal.ReadInt32(data, 40));
                int count = Marshal.ReadInt32(data, 44); entry["allocationCount"] = count;
                IntPtr allocations = Marshal.ReadIntPtr(data, 48);
                if (count < 0 || count > 64 || allocations == IntPtr.Zero) return;
                var objects = new List<object>();
                for (int i = 0; i < count; i++)
                {
                    IntPtr allocation = IntPtr.Add(allocations, i * 96);
                    objects.Add(new Dictionary<string, object> {
                        { "handle", Marshal.ReadInt32(allocation, 0) },
                        { "systemMemory", Marshal.ReadIntPtr(allocation, 8).ToInt64().ToString("X") },
                        { "gpuAddress", Marshal.ReadInt64(allocation, 40).ToString("X") },
                        { "privateData", Dump(id, "allocation-" + i + "-private-" + suffix, Marshal.ReadIntPtr(allocation, 16), Marshal.ReadInt32(allocation, 24)) }
                    });
                }
                entry["allocations"] = objects;
            }
            else if (name == "SubmitCommand")
            {
                entry["gpuAddress"] = Marshal.ReadInt64(data, 0).ToString("X");
                entry["commandBytes"] = Marshal.ReadInt32(data, 8);
                entry["contextCount"] = Marshal.ReadInt32(data, 24);
                entry["context"] = Marshal.ReadInt32(data, 28);
                entry["privateData"] = Dump(id, "submit-private-" + suffix, Marshal.ReadIntPtr(data, 288), Marshal.ReadInt32(data, 296));
            }
        }
        public void Dispose()
        {
            if (disposed) return;
            for (int i = hooks.Count - 1; i >= 0; i--) WriteCode(hooks[i].Address, hooks[i].OriginalBytes);
            lock (gate) { disposed = true; writer.Dispose(); }
        }
        internal static void SaveBootstrap(string output)
        {
            if (Buffer8192Template == null || Buffer4096Template == null || LastSubmitHeader == null) throw new InvalidOperationException("Incomplete captured allocation templates.");
            string path = Path.Combine(output, "bootstrap"); Directory.CreateDirectory(path);
            File.WriteAllBytes(Path.Combine(path, "buffer-8192.bin"), Buffer8192Template);
            File.WriteAllBytes(Path.Combine(path, "buffer-4096.bin"), Buffer4096Template);
            File.WriteAllBytes(Path.Combine(path, "submit-header.bin"), LastSubmitHeader);
        }
        internal static Dictionary<string, object> Inspect()
        {
            var output = new Dictionary<string, object>();
            IntPtr module = LoadLibrary("win32u.dll");
            if (module == IntPtr.Zero) throw new InvalidOperationException("Cannot load win32u.dll");
            foreach (string name in names)
            {
                IntPtr address = GetProcAddress(module, "NtGdiDdDDI" + name);
                if (address == IntPtr.Zero) { output[name] = "export absent"; continue; }
                var bytes = new byte[32]; Marshal.Copy(address, bytes, 0, bytes.Length);
                output[name] = BitConverter.ToString(bytes);
            }
            return output;
        }
    }
}
