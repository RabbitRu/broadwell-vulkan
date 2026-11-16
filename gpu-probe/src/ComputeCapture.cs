using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;

namespace IrisProbe
{
    internal sealed partial class KernelTrace
    {
        private sealed class CapturedAllocation
        {
            internal uint Device, Handle;
            internal byte[] Template;
            internal ulong Address;
            internal int Bytes;
        }
        private readonly Dictionary<uint, CapturedAllocation> computeAllocations = new Dictionary<uint, CapturedAllocation>();
        internal static bool CaptureComputeEnabled;
        private bool computeSaved;
        internal bool ComputeSnapshotSaved { get { return computeSaved; } }
        internal static Dictionary<string, object> ComputeParameters;
        internal static void SetComputeParameters(int count, uint salt, IntPtr upload, IntPtr input, IntPtr output, IntPtr readback, string shader)
        {
            if (!CaptureComputeEnabled) return;
            ComputeParameters = new Dictionary<string, object> {
                { "count", count }, { "salt", salt },
                { "uploadAddress", N.Fn<N.GpuAddress>(upload, 11)(upload).ToString("X") },
                { "inputAddress", N.Fn<N.GpuAddress>(input, 11)(input).ToString("X") },
                { "outputAddress", N.Fn<N.GpuAddress>(output, 11)(output).ToString("X") },
                { "readbackAddress", N.Fn<N.GpuAddress>(readback, 11)(readback).ToString("X") },
                { "hlslSha256", Program.Hash(File.ReadAllBytes(shader)) },
                { "kernelCompiler", "stock Intel UMD 20.19.15.5171" }
            };
        }
        private List<CapturedAllocation> CaptureAllocationInputs(IntPtr data)
        {
            int count = Marshal.ReadInt32(data, 44); if (count < 1 || count > 64) return null;
            IntPtr info = Marshal.ReadIntPtr(data, 48); var records = new List<CapturedAllocation>();
            for (int i = 0; i < count; i++)
            {
                IntPtr item = IntPtr.Add(info, i * 96); int length = Marshal.ReadInt32(item, 24);
                if (length != 1096) { records.Add(null); continue; }
                var template = new byte[length]; Marshal.Copy(Marshal.ReadIntPtr(item, 16), template, 0, length);
                ulong bytes = BitConverter.ToUInt64(template, 40);
                if (BitConverter.ToUInt32(template, 8) != 6 || bytes == 0 || bytes > 64 * 1024 * 1024) { records.Add(null); continue; }
                records.Add(new CapturedAllocation { Device = unchecked((uint)Marshal.ReadInt32(data, 0)), Template = template, Bytes = checked((int)((bytes + 4095) & ~4095UL)) });
            }
            return records;
        }
        private void TrackComputeCall(string name, IntPtr data, List<CapturedAllocation> pending)
        {
            if (name == "CreateAllocation" && pending != null)
            {
                IntPtr info = Marshal.ReadIntPtr(data, 48);
                for (int i = 0; i < pending.Count; i++)
                {
                    var allocation = pending[i]; if (allocation == null) continue;
                    allocation.Handle = unchecked((uint)Marshal.ReadInt32(info, i * 96));
                    allocation.Address = unchecked((ulong)Marshal.ReadInt64(info, i * 96 + 40));
                    lock (gate) { computeAllocations[allocation.Handle] = allocation; }
                }
            }
            else if (name == "MapGpuVirtualAddress")
            {
                CapturedAllocation allocation;
                lock (gate)
                    if (computeAllocations.TryGetValue(unchecked((uint)Marshal.ReadInt32(data, 32)), out allocation))
                        allocation.Address = unchecked((ulong)Marshal.ReadInt64(data, 88));
            }
        }
        private void CaptureCompute(IntPtr submission)
        {
            if (computeSaved) return;
            if (ComputeParameters == null) throw new InvalidOperationException("Compute parameters missing.");
            string path = Path.Combine(Path.GetDirectoryName(directory), "compute-capture"); Directory.CreateDirectory(path);
            var records = new List<object>(); List<CapturedAllocation> allocations;
            lock (gate) { allocations = new List<CapturedAllocation>(computeAllocations.Values); }
            allocations.Sort(delegate(CapturedAllocation a, CapturedAllocation b) { return a.Address.CompareTo(b.Address); });
            long total = 0;
            foreach (var allocation in allocations)
            {
                if (allocation.Address == 0) continue;
                total += allocation.Bytes; if (total > 256 * 1024 * 1024) throw new InvalidOperationException("Compute snapshot exceeds memory limit.");
                string prefix = allocation.Address.ToString("X12");
                byte[] memory; string readError = null;
                try { memory = Wddm.ReadAllocation(allocation.Device, allocation.Handle, allocation.Bytes); }
                catch (InvalidOperationException error) { memory = new byte[allocation.Bytes]; readError = error.Message; }
                File.WriteAllBytes(Path.Combine(path, prefix + ".memory.bin"), memory);
                File.WriteAllBytes(Path.Combine(path, prefix + ".metadata.bin"), allocation.Template);
                records.Add(new Dictionary<string, object> {
                    { "address", prefix }, { "bytes", allocation.Bytes }, { "device", allocation.Device },
                    { "memoryFile", prefix + ".memory.bin" }, { "metadataFile", prefix + ".metadata.bin" },
                    { "memorySha256", Program.Hash(memory) }, { "metadataSha256", Program.Hash(allocation.Template) },
                    { "cpuReadSucceeded", readError == null }, { "readError", readError }
                });
            }
            int headerBytes = Marshal.ReadInt32(submission, 296);
            byte[] header = new byte[headerBytes]; Marshal.Copy(Marshal.ReadIntPtr(submission, 288), header, 0, header.Length);
            File.WriteAllBytes(Path.Combine(path, "submit-header.bin"), header);
            var manifest = new Dictionary<string, object> {
                { "schema", 1 }, { "capturedUtc", DateTime.UtcNow.ToString("o") }, { "driverVersion", "20.19.15.5171" },
                { "batchAddress", unchecked((ulong)Marshal.ReadInt64(submission, 0)).ToString("X") },
                { "batchBytes", Marshal.ReadInt32(submission, 8) }, { "totalBytes", total },
                { "parameters", ComputeParameters }, { "allocations", records }, { "submitHeaderSha256", Program.Hash(header) }
            };
            File.WriteAllText(Path.Combine(path, "capture.json"), new JavaScriptSerializer().Serialize(manifest));
            computeSaved = true; Console.WriteLine("Captured compute state: {0} buffers, {1} bytes", records.Count, total);
        }
    }
    internal static partial class Wddm
    {
        internal static byte[] ReadAllocation(uint device, uint allocation, int bytes)
        {
            var locking = new Lock { Device = device, Allocation = allocation };
            N.Check(D3DKMTLock2(ref locking), "Capture Lock2");
            try { var data = new byte[bytes]; Marshal.Copy(locking.Data, data, 0, bytes); return data; }
            finally { var unlock = new Unlock { Device = device, Allocation = allocation }; N.Check(D3DKMTUnlock2(ref unlock), "Capture Unlock2"); }
        }
    }
}
