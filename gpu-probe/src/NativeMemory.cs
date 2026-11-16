using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;

namespace IrisProbe
{
    internal static partial class Wddm
    {
        [StructLayout(LayoutKind.Sequential)] private struct PagingQueue
        {
            internal uint Device;
            internal int Priority;
            internal uint Handle, Sync;
            internal IntPtr Fence;
            internal uint PhysicalAdapter;
        }
        [StructLayout(LayoutKind.Explicit, Size = 72)] private struct Allocation
        {
            [FieldOffset(0)] internal uint Device;
            [FieldOffset(44)] internal uint Count;
            [FieldOffset(48)] internal IntPtr Info;
        }
        [StructLayout(LayoutKind.Sequential)] private struct Lock
        { internal uint Device, Allocation, Flags; internal IntPtr Data; }
        [StructLayout(LayoutKind.Sequential)] private struct Unlock { internal uint Device, Allocation; }
        [StructLayout(LayoutKind.Sequential)] private struct MapGpu
        {
            internal uint Queue;
            internal ulong Base, Minimum, Maximum;
            internal uint Allocation;
            internal ulong OffsetPages, Pages, Protection, DriverProtection;
            internal uint Reserved0;
            internal ulong Reserved1, Address, Fence;
        }
        [StructLayout(LayoutKind.Sequential)] private struct Resident
        {
            internal uint Queue, Count;
            internal IntPtr Allocations, Priorities;
            internal uint Flags;
            internal ulong Fence, BytesToTrim;
        }
        [StructLayout(LayoutKind.Sequential)] private struct FreeVa { internal uint Adapter; internal ulong Address, Size; }
        [StructLayout(LayoutKind.Sequential)] private struct DestroyAllocation { internal uint Device, Resource; internal IntPtr Allocations; internal uint Count, Flags; }
        [DllImport("gdi32.dll")] private static extern int D3DKMTCreatePagingQueue(ref PagingQueue queue);
        [DllImport("gdi32.dll")] private static extern int D3DKMTDestroyPagingQueue(ref uint queue);
        [DllImport("gdi32.dll")] private static extern int D3DKMTCreateAllocation2(ref Allocation allocation);
        [DllImport("gdi32.dll")] private static extern int D3DKMTDestroyAllocation2(ref DestroyAllocation allocation);
        [DllImport("gdi32.dll")] private static extern int D3DKMTLock2(ref Lock resource);
        [DllImport("gdi32.dll")] private static extern int D3DKMTUnlock2(ref Unlock resource);
        [DllImport("gdi32.dll")] private static extern int D3DKMTMapGpuVirtualAddress(ref MapGpu map);
        [DllImport("gdi32.dll")] private static extern int D3DKMTFreeGpuVirtualAddress(ref FreeVa address);
        [DllImport("gdi32.dll")] private static extern int D3DKMTMakeResident(ref Resident resident);

        private sealed class NativeBuffer : IDisposable
        {
            private readonly Session session;
            internal uint Handle;
            internal ulong Address;
            internal IntPtr Cpu;
            internal readonly int Bytes;
            internal NativeBuffer(Session owner, byte[] template, int bytes, ulong protection = 1)
            {
                session = owner; Bytes = bytes;
                if (template.Length != 1096 || ((BitConverter.ToUInt64(template, 40) + 4095) & ~4095UL) != (ulong)bytes) throw new InvalidOperationException("Unsupported Intel allocation template.");
                IntPtr info = Marshal.AllocHGlobal(96), blob = Marshal.AllocHGlobal(template.Length);
                try
                {
                    for (int i = 0; i < 96; i++) Marshal.WriteByte(info, i, 0);
                    Marshal.Copy(template, 0, blob, template.Length);
                    Marshal.WriteIntPtr(info, 16, blob); Marshal.WriteInt32(info, 24, template.Length);
                    var request = new Allocation { Device = session.device.Handle, Count = 1, Info = info };
                    N.Check(D3DKMTCreateAllocation2(ref request), "Native CreateAllocation2");
                    Handle = unchecked((uint)Marshal.ReadInt32(info));
                    var locking = new Lock { Device = session.device.Handle, Allocation = Handle };
                    N.Check(D3DKMTLock2(ref locking), "Native Lock2"); Cpu = locking.Data;
                    // The working Intel UMD maps GPU-only allocations above the CPU VA range.
                    var mapping = new MapGpu { Queue = session.queue.Handle, Allocation = Handle, Minimum = 0x800000000000, Maximum = 0xfffffffff000, Pages = (ulong)bytes / 4096, Protection = protection };
                    N.Check(D3DKMTMapGpuVirtualAddress(ref mapping), "Native MapGpuVirtualAddress bytes=" + bytes);
                    Address = mapping.Address; session.WaitPaging(mapping.Fence);
                    Marshal.WriteInt32(info, unchecked((int)Handle));
                    var resident = new Resident { Queue = session.queue.Handle, Count = 1, Allocations = info };
                    N.Check(D3DKMTMakeResident(ref resident), "Native MakeResident"); session.WaitPaging(resident.Fence);
                }
                catch { Dispose(); throw; }
                finally { Marshal.FreeHGlobal(info); Marshal.FreeHGlobal(blob); }
            }
            public void Dispose()
            {
                if (Cpu != IntPtr.Zero) { var unlock = new Unlock { Device = session.device.Handle, Allocation = Handle }; N.Check(D3DKMTUnlock2(ref unlock), "Native Unlock2"); Cpu = IntPtr.Zero; }
                if (Address != 0) { var address = new FreeVa { Adapter = session.open.Adapter, Address = Address, Size = (ulong)Bytes }; N.Check(D3DKMTFreeGpuVirtualAddress(ref address), "Native FreeGpuVirtualAddress"); Address = 0; }
                if (Handle != 0)
                {
                    IntPtr list = Marshal.AllocHGlobal(4);
                    try { Marshal.WriteInt32(list, unchecked((int)Handle)); var request = new DestroyAllocation { Device = session.device.Handle, Allocations = list, Count = 1 }; N.Check(D3DKMTDestroyAllocation2(ref request), "Native DestroyAllocation2"); Handle = 0; }
                    finally { Marshal.FreeHGlobal(list); }
                }
            }
        }
        private sealed class Session : IDisposable
        {
            internal Open open;
            internal Device device;
            internal PagingQueue queue;
            internal OwnContext Context;
            internal string DriverVersion;
            internal Session(N.Luid luid)
            {
                if (Marshal.SizeOf(typeof(MapGpu)) != 104 || Marshal.SizeOf(typeof(Resident)) != 48 || Marshal.SizeOf(typeof(PagingQueue)) != 32 || Marshal.SizeOf(typeof(Lock)) != 24) throw new InvalidOperationException("Native memory x64 ABI mismatch.");
                try
                {
                    open.Luid = luid; N.Check(D3DKMTOpenAdapterFromLuid(ref open), "Native OpenAdapterFromLuid");
                    DriverVersion = QueryValue(open.Adapter, 18, 8).ToString();
                    if (DriverVersion != "20.19.15.5171") throw new InvalidOperationException("Captured Intel metadata is pinned to 20.19.15.5171; queried driver: " + DriverVersion);
                    device.AdapterUnion = (UIntPtr)open.Adapter; N.Check(D3DKMTCreateDevice(ref device), "Native CreateDevice");
                    queue.Device = device.Handle; N.Check(D3DKMTCreatePagingQueue(ref queue), "Native CreatePagingQueue");
                    Context = new OwnContext(device.Handle);
                }
                catch { Dispose(); throw; }
            }
            internal void WaitPaging(ulong value)
            {
                var timer = Stopwatch.StartNew();
                while (unchecked((ulong)Marshal.ReadInt64(queue.Fence)) < value)
                { if (timer.ElapsedMilliseconds > 15000) throw new TimeoutException("Native GPU paging fence timeout."); Thread.Sleep(1); }
            }
            public void Dispose()
            {
                if (Context != null) { Context.Dispose(); Context = null; }
                if (queue.Handle != 0) { N.Check(D3DKMTDestroyPagingQueue(ref queue.Handle), "Native DestroyPagingQueue"); queue.Handle = 0; }
                if (device.Handle != 0) { N.Check(D3DKMTDestroyDevice(ref device.Handle), "Native DestroyDevice"); device.Handle = 0; }
                if (open.Adapter != 0) { N.Check(D3DKMTCloseAdapter(ref open.Adapter), "Native CloseAdapter"); open.Adapter = 0; }
            }
        }
        internal static Dictionary<string, object> NativeMemoryStoreTest(N.Luid luid, string bootstrapDirectory, string output)
        {
            byte[] commandTemplate = File.ReadAllBytes(Path.Combine(bootstrapDirectory, "buffer-8192.bin"));
            byte[] resultTemplate = File.ReadAllBytes(Path.Combine(bootstrapDirectory, "buffer-4096.bin"));
            byte[] header = File.ReadAllBytes(Path.Combine(bootstrapDirectory, "submit-header.bin"));
            if (Program.Hash(commandTemplate) != "c556da668a4af55e8c69ae99821604c7e7fc417512e83c1acb2b1073a1655ada" ||
                Program.Hash(resultTemplate) != "a0e961d199ce684b7f940bf5607d5f33bd7a21ae0fa007acda95ec4b14c50078" ||
                Program.Hash(header) != "f0dee23679c6f9cb6d7de450ac10a94f0e71056fcaf7e4a690be2c0a23ce490b")
                throw new InvalidOperationException("Captured Intel metadata checksum mismatch.");
            KernelTrace.Phase = "native-memory";
            using (var session = new Session(luid))
            using (var commands = new NativeBuffer(session, commandTemplate, 8192))
            using (var result = new NativeBuffer(session, resultTemplate, 4096))
            {
                uint seed; byte[] random = new byte[4]; using (var rng = System.Security.Cryptography.RandomNumberGenerator.Create()) rng.GetBytes(random); seed = BitConverter.ToUInt32(random, 0);
                var expected = new uint[1024]; for (int i = 0; i < expected.Length; i++) expected[i] = 0xcdcdcdcdu;
                Marshal.Copy(Program.Bytes(expected), 0, result.Cpu, 4096);
                var words = new List<uint>(); int[] indices = { 0, 7, 255, 1023 };
                foreach (int index in indices)
                {
                    ulong address = result.Address + (ulong)index * 4; uint value = Program.Mix(seed ^ (uint)index); if (value == 0xcdcdcdcdu) value ^= 1; expected[index] = value;
                    words.Add(0x10000002); words.Add((uint)address); words.Add((uint)(address >> 32)); words.Add(value);
                }
                words.Add(0x05000000); words.Add(0); byte[] batch = Program.Bytes(words.ToArray());
                Marshal.Copy(new byte[8192], 0, commands.Cpu, 8192); Marshal.Copy(batch, 0, commands.Cpu, batch.Length);
                N.Check(SubmitBatch(session.Context.Handle, commands.Address, (uint)batch.Length, header), "Fully native SubmitCommand"); session.Context.Wait();
                byte[] actual = new byte[4096]; Marshal.Copy(result.Cpu, actual, 0, actual.Length);
                Program.CheckWords(actual, expected, "Native WDDM allocator/store/readback");
                var samples = new List<object>(); foreach (int index in indices) samples.Add(new Dictionary<string, object> { { "wordIndex", index }, { "expected", expected[index].ToString("X8") }, { "actual", BitConverter.ToUInt32(actual, index * 4).ToString("X8") } });
                File.WriteAllBytes(Path.Combine(output, "native-memory-batch.bin"), batch); File.WriteAllBytes(Path.Combine(output, "native-memory-readback.bin"), actual);
                return new Dictionary<string, object> { { "passed", true }, { "driverVersion", session.DriverVersion }, { "ownDevice", true }, { "ownContext", true }, { "ownPagingQueue", true }, { "ownAllocations", true }, { "d3d12CallsInTest", 0 }, { "usesCapturedIntelAllocationMetadata", true }, { "wordsVerifiedIncludingGuards", 1024 }, { "writeSamples", samples }, { "seed", seed }, { "sha256", Program.Hash(actual) } };
            }
        }
    }
}
