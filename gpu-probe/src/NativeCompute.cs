using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Web.Script.Serialization;

namespace IrisProbe
{
    internal static partial class Wddm
    {
        private sealed class Packet
        {
            internal string Name;
            internal ulong Address;
            internal uint[] Words;
        }
        private static ulong GpuAddress(object text) { return Convert.ToUInt64((string)text, 16) & 0xffffffffffffUL; }
        private static Dictionary<string, object> ObjectMap(object value) { return (Dictionary<string, object>)value; }
        private static ulong PacketBase(uint[] words, int index) { return (((ulong)words[index + 1] << 32) | (words[index] & 0xfffff000u)) & 0xffffffffffffUL; }
        private static ulong Canonical(ulong address) { return (address & (1UL << 47)) != 0 ? address | 0xffff000000000000UL : address; }
        private static void SetPacketBase(uint[] words, int index, ulong address)
        { ulong value = Canonical(address); words[index] = (uint)value | (words[index] & 0xfffu); words[index + 1] = (uint)(value >> 32); }
        private static List<Packet> LoadPackets(string path)
        {
            var result = new List<Packet>();
            foreach (object item in (object[])new JavaScriptSerializer().DeserializeObject(File.ReadAllText(path)))
            {
                var record = ObjectMap(item); var source = (object[])record["Words"]; var words = new uint[source.Length];
                for (int i = 0; i < words.Length; i++) words[i] = Convert.ToUInt32((string)source[i], 16);
                result.Add(new Packet { Name = (string)record["Name"], Address = GpuAddress(record["Address"]), Words = words });
            }
            return result;
        }
        private static Packet Pick(List<Packet> packets, string name, Predicate<Packet> predicate, bool last)
        {
            Packet result = null;
            foreach (var packet in packets) if (packet.Name == name && (predicate == null || predicate(packet))) { result = packet; if (!last) break; }
            if (result == null) throw new InvalidOperationException("Required captured packet absent: " + name);
            return result;
        }
        private static Dictionary<string, object> FindCapturedAllocation(object[] allocations, ulong address)
        {
            address &= 0xffffffffffffUL;
            foreach (object item in allocations)
            {
                var allocation = ObjectMap(item); ulong start = GpuAddress(allocation["address"]);
                if (address >= start && address < start + Convert.ToUInt64(allocation["bytes"])) return allocation;
            }
            throw new InvalidOperationException("Compute address absent from snapshot: " + address.ToString("X"));
        }
        private static byte[] CheckedCaptureFile(string directory, Dictionary<string, object> allocation, bool metadata)
        {
            string file = (string)allocation[metadata ? "metadataFile" : "memoryFile"];
            if (file != Path.GetFileName(file)) throw new InvalidOperationException("Invalid capture filename.");
            byte[] bytes = File.ReadAllBytes(Path.Combine(directory, file));
            if (bytes.Length != (metadata ? 1096 : Convert.ToInt32(allocation["bytes"]))) throw new InvalidOperationException("Capture file size mismatch: " + file);
            if (Program.Hash(bytes) != (string)allocation[metadata ? "metadataSha256" : "memorySha256"]) throw new InvalidOperationException("Capture checksum mismatch: " + file);
            return bytes;
        }
        internal static Dictionary<string, object> NativeComputeTest(N.Luid luid, string captureDirectory, string bootstrapDirectory, string output, int count)
        {
            var capture = ObjectMap(new JavaScriptSerializer().DeserializeObject(File.ReadAllText(Path.Combine(captureDirectory, "capture.json"))));
            var parameters = ObjectMap(capture["parameters"]);
            if ((string)capture["driverVersion"] != "20.19.15.5171" || (string)parameters["hlslSha256"] != "feaa223a6382390994b235b082e4688e274a7de004a69b5df98f731b6879c0b0")
                throw new InvalidOperationException("Compute snapshot is not the pinned Gen8 probe kernel.");
            object[] allocations = (object[])capture["allocations"];
            var packets = LoadPackets(Path.Combine(captureDirectory, "decoded-packets.json"));
            Packet walker = Pick(packets, "GPGPU_WALKER", null, false);
            if (walker.Words.Length != 15 || walker.Words[0] != 0x7105000d || walker.Words[4] != 0x40000300) throw new InvalidOperationException("Unexpected Gen8 SIMD16 walker layout.");
            Packet bases = Pick(packets, "STATE_BASE_ADDRESS", delegate(Packet p) { return p.Address < walker.Address && PacketBase(p.Words, 6) != 0 && PacketBase(p.Words, 10) != 0; }, false);
            Packet surfaceBases = Pick(packets, "STATE_BASE_ADDRESS", delegate(Packet p) { return p.Address < walker.Address && (p.Words[4] & 1) != 0 && PacketBase(p.Words, 4) != 0; }, true);
            Packet descriptorLoad = Pick(packets, "MEDIA_INTERFACE_DESCRIPTOR_LOAD", delegate(Packet p) { return p.Address < walker.Address; }, true);
            ulong dynamicBase = PacketBase(bases.Words, 6), indirectBase = PacketBase(bases.Words, 8), instructionBase = PacketBase(bases.Words, 10), surfaceBase = PacketBase(surfaceBases.Words, 4);
            ulong curbeAddress = indirectBase + walker.Words[3], descriptorAddress = dynamicBase + descriptorLoad.Words[3];
            var descriptorAllocation = FindCapturedAllocation(allocations, descriptorAddress);
            byte[] descriptorMemory = CheckedCaptureFile(captureDirectory, descriptorAllocation, false);
            int descriptorOffset = checked((int)(descriptorAddress - GpuAddress(descriptorAllocation["address"])));
            ulong kernelAddress = instructionBase + (BitConverter.ToUInt64(descriptorMemory, descriptorOffset) & 0xffffffffffc0UL);
            var kernelAllocation = FindCapturedAllocation(allocations, kernelAddress);
            var inputAllocation = FindCapturedAllocation(allocations, GpuAddress(parameters["inputAddress"]));
            var outputAllocation = FindCapturedAllocation(allocations, GpuAddress(parameters["outputAddress"]));
            var uploadAllocation = FindCapturedAllocation(allocations, GpuAddress(parameters["uploadAddress"]));
            int bufferBytes = Convert.ToInt32(inputAllocation["bytes"]), capacity = bufferBytes / 4;
            if (count < 1 || count > 16381 || capacity != 16384 || Convert.ToInt32(outputAllocation["bytes"]) != bufferBytes) throw new InvalidOperationException("Unsupported compute count or buffer shape.");
            byte[] cpuBufferTemplate = CheckedCaptureFile(captureDirectory, uploadAllocation, true);
            byte[] commandTemplate = File.ReadAllBytes(Path.Combine(bootstrapDirectory, "buffer-8192.bin"));
            byte[] scratchTemplate = File.ReadAllBytes(Path.Combine(bootstrapDirectory, "buffer-4096.bin"));
            byte[] header = File.ReadAllBytes(Path.Combine(bootstrapDirectory, "submit-header.bin"));
            if (Program.Hash(header) != "f0dee23679c6f9cb6d7de450ac10a94f0e71056fcaf7e4a690be2c0a23ce490b") throw new InvalidOperationException("Unsupported submit header.");

            var stateRecord = FindCapturedAllocation(allocations, curbeAddress);
            if (GpuAddress(stateRecord["address"]) != GpuAddress(descriptorAllocation["address"])) throw new InvalidOperationException("CURBE and descriptor require separate heaps.");
            var surfaceRecord = FindCapturedAllocation(allocations, surfaceBase);
            var resources = new List<NativeBuffer>();
            KernelTrace.Phase = "native-compute";
            using (var session = new Session(luid))
            {
                try
                {
                    Func<Dictionary<string, object>, NativeBuffer> loadState = delegate(Dictionary<string, object> record)
                    {
                        if (!(bool)record["cpuReadSucceeded"]) throw new InvalidOperationException("Compute state content was not captured.");
                        var resource = new NativeBuffer(session, CheckedCaptureFile(captureDirectory, record, true), Convert.ToInt32(record["bytes"]), 0);
                        resources.Add(resource);
                        byte[] contents = CheckedCaptureFile(captureDirectory, record, false); Marshal.Copy(contents, 0, resource.Cpu, contents.Length);
                        return resource;
                    };
                    NativeBuffer curbe = loadState(stateRecord), code = loadState(kernelAllocation), surfaces = loadState(surfaceRecord);
                    var input = new NativeBuffer(session, cpuBufferTemplate, bufferBytes); resources.Add(input);
                    var result = new NativeBuffer(session, cpuBufferTemplate, bufferBytes); resources.Add(result);
                    var scratch = new NativeBuffer(session, scratchTemplate, 4096); resources.Add(scratch);
                    var commands = new NativeBuffer(session, commandTemplate, 8192); resources.Add(commands);
                    byte[] random = new byte[8]; using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(random);
                    uint seed = BitConverter.ToUInt32(random, 0), salt = BitConverter.ToUInt32(random, 4);
                    uint[] inputWords = Program.Pattern(capacity, seed), expected = new uint[capacity];
                    for (int i = 0; i < capacity; i++) expected[i] = i < count ? Program.Mix(unchecked(inputWords[i] ^ salt ^ (uint)i * 0x9e3779b9u)) : 0xcdcdcdcdu;
                    Marshal.Copy(Program.Bytes(inputWords), 0, input.Cpu, bufferBytes);
                    var poison = new uint[capacity]; for (int i = 0; i < capacity; i++) poison[i] = 0xcdcdcdcdu;
                    Marshal.Copy(Program.Bytes(poison), 0, result.Cpu, bufferBytes);
                    int curbeOffset = checked((int)(curbeAddress - GpuAddress(stateRecord["address"])));
                    if ((unchecked((ulong)Marshal.ReadInt64(curbe.Cpu, curbeOffset + 32)) & 0xffffffffffffUL) != GpuAddress(parameters["inputAddress"]) ||
                        (unchecked((ulong)Marshal.ReadInt64(curbe.Cpu, curbeOffset + 40)) & 0xffffffffffffUL) != GpuAddress(parameters["outputAddress"]))
                        throw new InvalidOperationException("Captured root argument pointer layout mismatch.");
                    Marshal.WriteInt32(curbe.Cpu, curbeOffset, count); Marshal.WriteInt32(curbe.Cpu, curbeOffset + 4, unchecked((int)salt));
                    Marshal.WriteInt64(curbe.Cpu, curbeOffset + 32, unchecked((long)Canonical(input.Address)));
                    Marshal.WriteInt64(curbe.Cpu, curbeOffset + 40, unchecked((long)Canonical(result.Address)));
                    int relocatedDescriptorOffset = checked((int)(descriptorAddress - GpuAddress(stateRecord["address"])));
                    int relocatedKernelOffset = checked((int)(kernelAddress - GpuAddress(kernelAllocation["address"])));
                    ulong descriptorKernel = unchecked((ulong)Marshal.ReadInt64(curbe.Cpu, relocatedDescriptorOffset));
                    descriptorKernel = (descriptorKernel & ~0xffffffffffc0UL) | (ulong)(uint)relocatedKernelOffset;
                    Marshal.WriteInt64(curbe.Cpu, relocatedDescriptorOffset, unchecked((long)descriptorKernel));
                    int surfacePointerPatches = 0;
                    for (int offset = 0; offset + 8 <= surfaces.Bytes; offset += 4)
                    {
                        ulong address = unchecked((ulong)Marshal.ReadInt64(surfaces.Cpu, offset)) & 0xffffffffffffUL;
                        if (address == GpuAddress(parameters["inputAddress"])) { Marshal.WriteInt64(surfaces.Cpu, offset, unchecked((long)Canonical(input.Address))); surfacePointerPatches++; }
                        else if (address == GpuAddress(parameters["outputAddress"])) { Marshal.WriteInt64(surfaces.Cpu, offset, unchecked((long)Canonical(result.Address))); surfacePointerPatches++; }
                    }

                    // Emit our own compute-only stream. No blit draw, MI_ATOMIC or captured batch replay.
                    var words = new List<uint>(); var emitted = new List<string>();
                    Action<string, uint[]> emit = delegate(string name, uint[] data) { emitted.Add(name); words.AddRange(data); };
                    emit("PIPE_CONTROL", Pick(packets, "PIPE_CONTROL", null, false).Words);
                    emit("PIPELINE_SELECT", Pick(packets, "PIPELINE_SELECT", delegate(Packet p) { return (p.Words[0] & 3) == 2; }, false).Words);
                    foreach (uint register in new uint[] { 0x20c0, 0x7004, 0x7034 })
                        emit("MI_LOAD_REGISTER_IMM", Pick(packets, "MI_LOAD_REGISTER_IMM", delegate(Packet p) { return p.Address < walker.Address && p.Words.Length == 3 && p.Words[1] == register; }, true).Words);
                    uint[] stateBases = (uint[])bases.Words.Clone();
                    stateBases[1] &= 0xffeu; stateBases[2] = 0; // Unused general heap is not mapped.
                    stateBases[4] = surfaceBases.Words[4]; stateBases[5] = surfaceBases.Words[5];
                    SetPacketBase(stateBases, 4, surfaces.Address); SetPacketBase(stateBases, 6, curbe.Address);
                    SetPacketBase(stateBases, 8, curbe.Address); SetPacketBase(stateBases, 10, code.Address);
                    emit("STATE_BASE_ADDRESS", stateBases);
                    emit("PIPE_CONTROL", new uint[] { 0x7a000004, 0x01000c1c, 0, 0, 0, 0 });
                    emit("MEDIA_VFE_STATE", Pick(packets, "MEDIA_VFE_STATE", delegate(Packet p) { return p.Address < walker.Address; }, true).Words);
                    uint[] descriptorPacket = (uint[])descriptorLoad.Words.Clone(); descriptorPacket[3] = (uint)relocatedDescriptorOffset;
                    emit("MEDIA_INTERFACE_DESCRIPTOR_LOAD", descriptorPacket);
                    emit("MI_LOAD_REGISTER_IMM", new uint[] { 0x11000001, 0x2600, 0xffffffff });
                    uint[] dispatch = (uint[])walker.Words.Clone(); dispatch[3] = (uint)curbeOffset; dispatch[7] = (uint)(count + 63) / 64;
                    emit("GPGPU_WALKER", dispatch);
                    emit("MI_LOAD_REGISTER_IMM", new uint[] { 0x11000001, 0x2600, 0 });
                    emit("MEDIA_STATE_FLUSH", new uint[] { 0x70040000, 0 });
                    emit("PIPE_CONTROL", new uint[] { 0x7a000004, 0x00105021, (uint)scratch.Address, (uint)(scratch.Address >> 32), 0, 0 });
                    emit("MI_BATCH_BUFFER_END", new uint[] { 0x05000000 });
                    if ((words.Count & 1) != 0) words.Add(0);
                    byte[] batch = Program.Bytes(words.ToArray());
                    if (batch.Length > 8180) throw new InvalidOperationException("Compute stream too large.");
                    Marshal.Copy(new byte[8192], 0, commands.Cpu, 8192); Marshal.Copy(batch, 0, commands.Cpu, batch.Length);
                    File.WriteAllBytes(Path.Combine(output, "native-compute-batch.bin"), batch);
                    N.Check(SubmitBatch(session.Context.Handle, commands.Address, (uint)batch.Length, header), "Native GPGPU_WALKER SubmitCommand");
                    session.Context.Wait();
                    byte[] actual = new byte[bufferBytes]; Marshal.Copy(result.Cpu, actual, 0, actual.Length);
                    File.WriteAllBytes(Path.Combine(output, "native-compute-readback.bin"), actual);
                    Program.CheckWords(actual, expected, "Native compute shader and guard words");
                    int kernelOffset = checked((int)(kernelAddress - GpuAddress(kernelAllocation["address"])));
                    byte[] kernelMemory = CheckedCaptureFile(captureDirectory, kernelAllocation, false), metadata = CheckedCaptureFile(captureDirectory, kernelAllocation, true);
                    int kernelBytes = checked((int)BitConverter.ToUInt64(metadata, 40) - kernelOffset);
                    var kernel = new byte[kernelBytes]; System.Buffer.BlockCopy(kernelMemory, kernelOffset, kernel, 0, kernel.Length);
                    File.WriteAllBytes(Path.Combine(output, "gen8-compute-kernel.bin"), kernel);
                    return new Dictionary<string, object> {
                        { "passed", true }, { "elementsVerified", count }, { "guardWordsVerified", capacity - count }, { "seed", seed }, { "salt", salt },
                        { "groupsX", dispatch[7] }, { "workgroupSize", 64 }, { "nativeBatchBytes", batch.Length }, { "emittedPackets", emitted },
                        { "kernelBytes", kernel.Length }, { "kernelSha256", Program.Hash(kernel) }, { "readbackSha256", Program.Hash(actual) },
                        { "ownComputeCommandStream", true }, { "capturedFullBatchReplayed", false }, { "kernelCompiledBy", "stock Intel UMD during bootstrap" },
                        { "usesCapturedStateAndAllocationDescriptions", true }, { "driverVersion", session.DriverVersion }, { "d3d12CallsInTest", 0 },
                        { "stateAndBufferAddressesRelocated", true }, { "surfacePointerPatches", surfacePointerPatches }
                    };
                }
                finally { for (int i = resources.Count - 1; i >= 0; i--) resources[i].Dispose(); }
            }
        }
    }
    internal static partial class Program
    {
        private static int NativeComputeMain(string output, string capture, string bootstrap, int count)
        {
            var report = new Dictionary<string, object> { { "schema", 3 }, { "startedUtc", DateTime.UtcNow.ToString("o") }, { "nativeCompute", true }, { "isVulkanDriver", false } };
            int exitCode = 0;
            try
            {
                N.AdapterDesc adapter = FindIris(); Console.WriteLine("Native compute on {0} ({1:X4}:{2:X4})", adapter.Description, adapter.Vendor, adapter.Device);
                report["adapter"] = new Dictionary<string, object> { { "name", adapter.Description }, { "vendorId", adapter.Vendor }, { "deviceId", adapter.Device }, { "software", false } };
                report["compute"] = Wddm.NativeComputeTest(adapter.Luid, capture, bootstrap, output, count);
                bool d3d12 = false; using (var process = Process.GetCurrentProcess()) foreach (ProcessModule module in process.Modules)
                    if (module.ModuleName.StartsWith("d3d12", StringComparison.OrdinalIgnoreCase) || module.ModuleName.StartsWith("igd12", StringComparison.OrdinalIgnoreCase)) d3d12 = true;
                report["d3d12OrIntelD3d12UmdLoaded"] = d3d12;
                if (d3d12) throw new InvalidOperationException("D3D12 appeared in native compute process.");
                Console.WriteLine("PASS: native GPGPU_WALKER computed {0} values; guard words and absence of D3D12 verified", count);
            }
            catch (Exception error) { report["error"] = error.ToString(); Console.Error.WriteLine(error); exitCode = 1; }
            report["overallPassed"] = exitCode == 0; report["finishedUtc"] = DateTime.UtcNow.ToString("o"); SaveReport(output, report); return exitCode;
        }
    }
}
