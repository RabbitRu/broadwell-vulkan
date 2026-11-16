using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Web.Script.Serialization;

namespace IrisProbe
{
    internal sealed class Gpu : IDisposable
    {
        internal IntPtr Device, Queue, Allocator, List, Fence;
        internal N.AdapterDesc Adapter;
        private readonly List<IntPtr> owned = new List<IntPtr>();
        private readonly AutoResetEvent completed = new AutoResetEvent(false);
        private ulong fenceValue;
        internal Gpu()
        {
            N.ValidateAbi();
            IntPtr factory = IntPtr.Zero, selected = IntPtr.Zero;
            try
            {
                Guid iid = N.FactoryIid;
                N.Check(N.CreateDXGIFactory1(ref iid, out factory), "CreateDXGIFactory1");
                for (uint index = 0; ; index++)
                {
                    IntPtr adapter;
                    int hr = N.Fn<N.EnumAdapter>(factory, 12)(factory, index, out adapter);
                    if (hr == unchecked((int)0x887a0002)) break;
                    N.Check(hr, "EnumAdapters1");
                    N.AdapterDesc desc;
                    try
                    {
                        N.Check(N.Fn<N.GetAdapterDesc>(adapter, 10)(adapter, out desc), "GetDesc1");
                        Console.WriteLine("Adapter: {0}; PCI {1:X4}:{2:X4}; software={3}", desc.Description, desc.Vendor, desc.Device, (desc.Flags & 2) != 0);
                        if (desc.Vendor == 0x8086 && desc.Device == 0x162b && (desc.Flags & 2) == 0)
                        { selected = adapter; adapter = IntPtr.Zero; Adapter = desc; break; }
                    }
                    finally { N.Release(ref adapter); }
                }
                if (selected == IntPtr.Zero) throw new InvalidOperationException("Hardware Iris 6100 (8086:162B) was not found. No WARP fallback is permitted.");
                iid = N.DeviceIid;
                N.Check(N.D3D12CreateDevice(selected, 0xb000, ref iid, out Device), "D3D12CreateDevice on Iris 6100");
                Track(Device);
                var queue = new N.QueueDesc();
                iid = N.QueueIid;
                N.Check(N.Fn<N.CreateQueue>(Device, 8)(Device, ref queue, ref iid, out Queue), "CreateCommandQueue"); Track(Queue);
                iid = N.AllocatorIid;
                N.Check(N.Fn<N.CreateAllocator>(Device, 9)(Device, 0, ref iid, out Allocator), "CreateCommandAllocator"); Track(Allocator);
                iid = N.ListIid;
                N.Check(N.Fn<N.CreateList>(Device, 12)(Device, 0, 0, Allocator, IntPtr.Zero, ref iid, out List), "CreateCommandList"); Track(List);
                N.Check(N.Fn<N.HResult>(List, 9)(List), "Close initial list");
                iid = N.FenceIid;
                N.Check(N.Fn<N.CreateFence>(Device, 36)(Device, 0, 0, ref iid, out Fence), "CreateFence"); Track(Fence);
            }
            catch { Dispose(); throw; }
            finally { N.Release(ref selected); N.Release(ref factory); }
        }
        private void Track(IntPtr obj) { owned.Add(obj); }
        internal int ShaderModel()
        {
            IntPtr memory = Marshal.AllocHGlobal(4);
            try
            {
                Marshal.WriteInt32(memory, 0x60);
                N.Check(N.Fn<N.Feature>(Device, 13)(Device, 7, memory, 4), "Query shader model");
                return Marshal.ReadInt32(memory);
            }
            finally { Marshal.FreeHGlobal(memory); }
        }
        internal IntPtr Resource(N.ResourceDesc desc, int heapType, uint state)
        {
            var heap = new N.HeapProps { Type = heapType, CreationMask = 1, VisibleMask = 1 };
            var iid = N.ResourceIid;
            IntPtr resource;
            N.Check(N.Fn<N.CreateResource>(Device, 27)(Device, ref heap, 0, ref desc, state, IntPtr.Zero, ref iid, out resource), "CreateCommittedResource");
            Track(resource); return resource;
        }
        internal IntPtr Buffer(int bytes, int heapType, uint state, uint flags)
        {
            return Resource(new N.ResourceDesc { Dimension = 1, Width = (ulong)bytes, Height = 1, Depth = 1, Mips = 1, Samples = 1, Layout = 1, Flags = flags }, heapType, state);
        }
        internal void Upload(IntPtr buffer, byte[] bytes)
        {
            var range = new N.Range(); IntPtr pointer;
            N.Check(N.Fn<N.Map>(buffer, 8)(buffer, 0, ref range, out pointer), "Map upload");
            try { Marshal.Copy(bytes, 0, pointer, bytes.Length); }
            finally { range.End = (UIntPtr)(uint)bytes.Length; N.Fn<N.Unmap>(buffer, 9)(buffer, 0, ref range); }
        }
        internal byte[] Readback(IntPtr buffer, int bytes)
        {
            var range = new N.Range { End = (UIntPtr)(uint)bytes }; IntPtr pointer;
            N.Check(N.Fn<N.Map>(buffer, 8)(buffer, 0, ref range, out pointer), "Map readback");
            var data = new byte[bytes];
            try { Marshal.Copy(pointer, data, 0, bytes); }
            finally { range.End = UIntPtr.Zero; N.Fn<N.Unmap>(buffer, 9)(buffer, 0, ref range); }
            return data;
        }
        internal void Begin(IntPtr pipeline)
        {
            N.Check(N.Fn<N.HResult>(Allocator, 8)(Allocator), "Reset allocator");
            N.Check(N.Fn<N.ResetList>(List, 10)(List, Allocator, pipeline), "Reset list");
        }
        internal void Transition(IntPtr resource, uint before, uint after)
        {
            var barrier = new N.Barrier { Resource = resource, Subresource = UInt32.MaxValue, Before = before, After = after };
            N.Fn<N.Barriers>(List, 26)(List, 1, ref barrier);
        }
        internal void Copy(IntPtr destination, IntPtr source, int bytes)
        { N.Fn<N.CopyBuffer>(List, 15)(List, destination, 0, source, 0, (ulong)bytes); }
        internal void Submit()
        {
            N.Check(N.Fn<N.HResult>(List, 9)(List), "Close list");
            N.Fn<N.Execute>(Queue, 10)(Queue, 1, ref List);
            WaitForQueue();
        }
        internal void WaitForQueue()
        {
            ulong value = ++fenceValue;
            N.Check(N.Fn<N.Signal>(Queue, 14)(Queue, Fence, value), "Signal fence");
            if (N.Fn<N.CompletedValue>(Fence, 8)(Fence) < value)
            {
                N.Check(N.Fn<N.FenceEvent>(Fence, 9)(Fence, value, completed.SafeWaitHandle.DangerousGetHandle()), "SetEventOnCompletion");
                uint wait = N.WaitForSingleObject(completed.SafeWaitHandle.DangerousGetHandle(), 15000);
                if (wait != 0) throw new TimeoutException("GPU completion wait returned 0x" + wait.ToString("X8"));
            }
            ulong actual = N.Fn<N.CompletedValue>(Fence, 8)(Fence);
            if (actual == UInt64.MaxValue) throw new InvalidOperationException("GPU device removed while waiting.");
            N.Check(N.Fn<N.HResult>(Device, 37)(Device), "GetDeviceRemovedReason");
        }
        internal IntPtr Compile(string sourcePath, string entry, string target)
        {
            byte[] source = File.ReadAllBytes(sourcePath); IntPtr shader, errors;
            int hr = N.D3DCompile(source, (UIntPtr)(uint)source.Length, sourcePath, IntPtr.Zero, IntPtr.Zero, entry, target, 0x800, 0, out shader, out errors);
            try
            {
                if (hr < 0)
                {
                    string details = errors == IntPtr.Zero ? "" : Marshal.PtrToStringAnsi(N.Fn<N.BlobPointer>(errors, 3)(errors));
                    N.Release(ref shader);
                    throw new InvalidOperationException("D3DCompile " + entry + " 0x" + hr.ToString("X8") + ": " + details);
                }
                Track(shader); return shader;
            }
            finally { N.Release(ref errors); }
        }
        internal static N.Bytecode Code(IntPtr blob)
        { return new N.Bytecode { Pointer = N.Fn<N.BlobPointer>(blob, 3)(blob), Length = N.Fn<N.BlobSize>(blob, 4)(blob) }; }
        internal IntPtr Root(bool compute)
        {
            IntPtr parameters = IntPtr.Zero, blob = IntPtr.Zero, errors = IntPtr.Zero;
            try
            {
                var desc = new N.RootDesc();
                if (compute)
                {
                    parameters = Marshal.AllocHGlobal(96);
                    for (int i = 0; i < 96; i++) Marshal.WriteByte(parameters, i, 0);
                    Marshal.StructureToPtr(new N.RootParameter { Type = 3 }, parameters, false);
                    Marshal.StructureToPtr(new N.RootParameter { Type = 4 }, IntPtr.Add(parameters, 32), false);
                    Marshal.StructureToPtr(new N.RootParameter { Type = 1, Count = 2 }, IntPtr.Add(parameters, 64), false);
                    desc.ParameterCount = 3; desc.Parameters = parameters;
                }
                int hr = N.D3D12SerializeRootSignature(ref desc, 1, out blob, out errors);
                if (hr < 0) throw new InvalidOperationException("SerializeRootSignature: " + (errors == IntPtr.Zero ? hr.ToString("X8") : Marshal.PtrToStringAnsi(N.Fn<N.BlobPointer>(errors, 3)(errors))));
                var code = Code(blob); var iid = N.RootIid; IntPtr root;
                N.Check(N.Fn<N.CreateRoot>(Device, 16)(Device, 0, code.Pointer, code.Length, ref iid, out root), "CreateRootSignature"); Track(root); return root;
            }
            finally { if (parameters != IntPtr.Zero) Marshal.FreeHGlobal(parameters); N.Release(ref blob); N.Release(ref errors); }
        }
        internal Dictionary<string, object> CopyTest(uint seed)
        {
            const int count = 16384, bytes = count * 4;
            uint[] values = Program.Pattern(count, seed);
            byte[] input = Program.Bytes(values);
            IntPtr upload = Buffer(bytes, 2, 0xac3, 0), gpu = Buffer(bytes, 1, 0x400, 0), readback = Buffer(bytes, 3, 0x400, 0);
            Upload(upload, input);
            Begin(IntPtr.Zero); Copy(gpu, upload, bytes); Transition(gpu, 0x400, 0x800); Copy(readback, gpu, bytes); Submit();
            byte[] actual = Readback(readback, bytes);
            Program.CheckWords(actual, values, "GPU copy");
            return new Dictionary<string, object> { { "passed", true }, { "bytesVerified", bytes }, { "seed", seed }, { "sha256", Program.Hash(actual) }, { "path", "D3D12 upload -> default GPU buffer -> readback, fence completion" } };
        }
        internal Dictionary<string, object> ComputeTest(string shaderPath, uint seed)
        {
            // Not divisible by the workgroup size: also checks the shader's bounds guard.
            const int count = 16381, bytes = count * 4;
            uint salt = seed ^ 0xa5c39e71u;
            uint[] input = Program.Pattern(count, seed), expected = new uint[count];
            for (int i = 0; i < count; i++) expected[i] = Program.Mix(unchecked(input[i] ^ salt ^ ((uint)i * 0x9e3779b9u)));
            IntPtr root = Root(true), shader = Compile(shaderPath, "CSMain", "cs_5_1");
            var desc = new N.ComputeDesc { Root = root, Shader = Code(shader) }; var iid = N.PipelineIid; IntPtr pipeline;
            N.Check(N.Fn<N.CreateCompute>(Device, 11)(Device, ref desc, ref iid, out pipeline), "CreateComputePipelineState"); Track(pipeline);
            IntPtr upload = Buffer(bytes, 2, 0xac3, 0), gpuInput = Buffer(bytes, 1, 0x400, 0), output = Buffer(bytes, 1, 8, 4), readback = Buffer(bytes, 3, 0x400, 0);
            KernelTrace.SetComputeParameters(count, salt, upload, gpuInput, output, readback, shaderPath);
            Upload(upload, Program.Bytes(input));
            Begin(pipeline); Copy(gpuInput, upload, bytes); Transition(gpuInput, 0x400, 0x40);
            N.Fn<N.SetObject>(List, 29)(List, root);
            N.Fn<N.RootAddress>(List, 39)(List, 0, N.Fn<N.GpuAddress>(gpuInput, 11)(gpuInput));
            N.Fn<N.RootAddress>(List, 41)(List, 1, N.Fn<N.GpuAddress>(output, 11)(output));
            N.Fn<N.RootConstants>(List, 35)(List, 2, 2, new uint[] { count, salt }, 0);
            N.Fn<N.Dispatch>(List, 14)(List, (count + 63) / 64, 1, 1);
            Transition(output, 8, 0x800); Copy(readback, output, bytes); Submit();
            byte[] actual = Readback(readback, bytes);
            Program.CheckWords(actual, expected, "GPU compute");
            return new Dictionary<string, object> { { "passed", true }, { "elementsVerified", count }, { "seed", seed }, { "salt", salt }, { "shaderTarget", "cs_5_1" }, { "sha256", Program.Hash(actual) }, { "firstResults", new uint[] { expected[0], expected[1], expected[2], expected[count - 1] } } };
        }
        internal Dictionary<string, object> TriangleTest(string shaderPath, string imagePath)
        {
            const int width = 512, height = 384;
            IntPtr root = Root(false), vs = Compile(shaderPath, "VSMain", "vs_5_1"), ps = Compile(shaderPath, "PSMain", "ps_5_1");
            var targets = new N.RenderBlend[8];
            for (int i = 0; i < 8; i++) targets[i] = new N.RenderBlend { Source = 2, Destination = 1, Operation = 1, SourceAlpha = 2, DestinationAlpha = 1, OperationAlpha = 1, LogicOperation = 5, WriteMask = 15 };
            var stencil = new N.StencilOp { Fail = 1, DepthFail = 1, Pass = 1, Function = 8 };
            var desc = new N.GraphicsDesc
            {
                Root = root, VS = Code(vs), PS = Code(ps),
                Blend = new N.Blend { Targets = targets }, SampleMask = UInt32.MaxValue,
                Rasterizer = new N.Rasterizer { Fill = 3, Cull = 1, DepthClip = 1 },
                DepthStencil = new N.DepthStencil { DepthFunction = 8, ReadMask = 255, WriteMask = 255, Front = stencil, Back = stencil },
                Topology = 3, TargetCount = 1, Formats = new int[] { 28, 0, 0, 0, 0, 0, 0, 0 }, Samples = 1
            };
            var iid = N.PipelineIid; IntPtr pipeline;
            N.Check(N.Fn<N.CreateGraphics>(Device, 10)(Device, ref desc, ref iid, out pipeline), "CreateGraphicsPipelineState"); Track(pipeline);
            Console.WriteLine("  Graphics pipeline created");
            var textureDesc = new N.ResourceDesc { Dimension = 3, Width = width, Height = height, Depth = 1, Mips = 1, Format = 28, Samples = 1, Flags = 1 };
            IntPtr texture = Resource(textureDesc, 1, 4);
            var heapDesc = new N.DescriptorHeapDesc { Type = 2, Count = 1 }; iid = N.HeapIid; IntPtr heap;
            N.Check(N.Fn<N.CreateDescriptorHeap>(Device, 14)(Device, ref heapDesc, ref iid, out heap), "CreateDescriptorHeap RTV"); Track(heap);
            ulong rtv; N.Fn<N.CpuDescriptor>(heap, 9)(heap, out rtv);
            N.Fn<N.CreateRtv>(Device, 20)(Device, texture, IntPtr.Zero, rtv);
            Console.WriteLine("  Render target and RTV created");
            N.Footprint footprint; uint rows; ulong rowBytes, total;
            N.Fn<N.GetFootprints>(Device, 38)(Device, ref textureDesc, 0, 1, 0, out footprint, out rows, out rowBytes, out total);
            if (total > Int32.MaxValue || rows != height || rowBytes != width * 4) throw new InvalidOperationException("Unexpected texture readback layout.");
            IntPtr readback = Buffer((int)total, 3, 0x400, 0);
            Begin(pipeline);
            N.Fn<N.SetObject>(List, 30)(List, root);
            var viewport = new N.Viewport { Width = width, Height = height, MaxDepth = 1 };
            var scissor = new N.Rect { Right = width, Bottom = height };
            N.Fn<N.SetViewport>(List, 21)(List, 1, ref viewport);
            N.Fn<N.SetScissor>(List, 22)(List, 1, ref scissor);
            N.Fn<N.SetTargets>(List, 46)(List, 1, ref rtv, 1, IntPtr.Zero);
            N.Fn<N.ClearTarget>(List, 48)(List, rtv, new float[] { 16f / 255, 24f / 255, 40f / 255, 1 }, 0, IntPtr.Zero);
            N.Fn<N.Topology>(List, 20)(List, 4);
            N.Fn<N.Draw>(List, 12)(List, 3, 1, 0, 0);
            Console.WriteLine("  Triangle draw recorded");
            Transition(texture, 4, 0x800);
            var destination = new N.TextureLocation { Resource = readback, Type = 1, Footprint = footprint };
            var source = new N.TextureLocation { Resource = texture, Type = 0, Subresource = 0 };
            N.Fn<N.CopyTexture>(List, 16)(List, ref destination, 0, 0, 0, ref source, IntPtr.Zero); Submit();
            Console.WriteLine("  Triangle draw and texture copy completed");
            byte[] raw = Readback(readback, (int)total);
            byte[] rgba = new byte[width * height * 4];
            for (int y = 0; y < height; y++) BufferCopy(raw, checked((int)footprint.Offset + y * (int)footprint.RowPitch), rgba, y * width * 4, width * 4);
            Dictionary<string, object> checks = Program.ValidateTriangle(rgba, width, height);
            Program.SavePng(rgba, width, height, imagePath);
            return new Dictionary<string, object> { { "passed", true }, { "width", width }, { "height", height }, { "image", imagePath }, { "rgbaSha256", Program.Hash(rgba) }, { "pixelChecks", checks }, { "path", "D3D12 DrawInstanced -> RGBA8 render target -> GPU texture copy -> readback" } };
        }
        internal Dictionary<string, object> RawStoreTest(uint seed, string outputDirectory, bool ownContext)
        {
            // Borrow only the already-idle stock context, allocator and fence. The command
            // bytes below are generated here and sent directly via D3DKMTSubmitCommand.
            byte[] header = KernelTrace.LastSubmitHeader == null ? null : (byte[])KernelTrace.LastSubmitHeader.Clone();
            uint context = KernelTrace.LastContext;
            if (context == 0 || header == null) throw new InvalidOperationException("Raw command test needs a successful traced D3D12 submission first.");
            const int batchBytes = 8192, resultBytes = 4096;
            // Retain the captured allocation-size field, matching our own 8 KiB batch.
            if (BitConverter.ToUInt32(header, 20) != batchBytes) throw new InvalidOperationException("Unexpected captured Intel batch allocation size.");
            IntPtr commands = Buffer(batchBytes, 2, 0xac3, 0), target = Buffer(resultBytes, 3, 0x400, 0);
            Wddm.OwnContext createdContext = null;
            if (ownContext) { createdContext = new Wddm.OwnContext(KernelTrace.LastRenderDevice); context = createdContext.Handle; }
            try
            {
            Action wait = ownContext ? (Action)createdContext.Wait : WaitForQueue;
            ulong batchAddress = N.Fn<N.GpuAddress>(commands, 11)(commands), resultAddress = N.Fn<N.GpuAddress>(target, 11)(target);
            if (batchAddress == 0 || resultAddress == 0 || (batchAddress & 7) != 0 || (resultAddress & 3) != 0) throw new InvalidOperationException("Invalid own-buffer GPU virtual addresses.");
            uint[] expected = new uint[resultBytes / 4]; for (int i = 0; i < expected.Length; i++) expected[i] = 0xcdcdcdcdu;
            Upload(target, Program.Bytes(expected));
            var bytes = new byte[batchBytes];
            System.Buffer.BlockCopy(new uint[] { 0, 0x05000000 }, 0, bytes, 0, 8); // MI_NOOP, MI_BATCH_BUFFER_END
            Upload(commands, bytes);
            KernelTrace.Phase = ownContext ? "own-context-noop" : "raw-noop";
            int noopStatus = Wddm.SubmitBatch(context, batchAddress, 8, header);
            N.Check(noopStatus, "Direct D3DKMTSubmitCommand MI_NOOP"); wait();
            Console.WriteLine("PASS: directly submitted native MI_NOOP completed");
            int[] offsets = { 0, 7, 255, 1023 };
            var words = new List<uint>();
            foreach (int index in offsets)
            {
                ulong address = resultAddress + (ulong)index * 4;
                uint value = Program.Mix(seed ^ (uint)index); if (value == 0xcdcdcdcdu) value ^= 1;
                expected[index] = value;
                words.Add(0x10000002); // Gen8 MI_STORE_DWORD_IMM, PPGTT, length = 4 dwords.
                words.Add((uint)address); words.Add((uint)(address >> 32)); words.Add(value);
            }
            words.Add(0x05000000); words.Add(0); // MI_BATCH_BUFFER_END, 8-byte alignment.
            byte[] nativeBatch = Program.Bytes(words.ToArray());
            Array.Clear(bytes, 0, bytes.Length); System.Buffer.BlockCopy(nativeBatch, 0, bytes, 0, nativeBatch.Length);
            string prefix = ownContext ? "own-context" : "raw";
            File.WriteAllBytes(Path.Combine(outputDirectory, prefix + "-gen8-batch.bin"), nativeBatch);
            Upload(commands, bytes);
            KernelTrace.Phase = ownContext ? "own-context-store" : "raw-store";
            int storeStatus = Wddm.SubmitBatch(context, batchAddress, (uint)nativeBatch.Length, header);
            N.Check(storeStatus, "Direct D3DKMTSubmitCommand MI_STORE_DWORD_IMM"); wait();
            byte[] actual = Readback(target, resultBytes);
            Program.CheckWords(actual, expected, "Direct Gen8 MI_STORE_DWORD_IMM and untouched guard words");
            File.WriteAllBytes(Path.Combine(outputDirectory, prefix + "-gen8-readback.bin"), actual);
            return new Dictionary<string, object> {
                { "passed", true }, { "noopStatus", "0x" + noopStatus.ToString("X8") }, { "storeStatus", "0x" + storeStatus.ToString("X8") },
                { "nativeCommand", "Gen8 MI_STORE_DWORD_IMM" }, { "nativeBatchBytes", nativeBatch.Length },
                { "writtenWordIndices", offsets }, { "wordsVerifiedIncludingGuards", expected.Length }, { "seed", seed },
                { "sha256", Program.Hash(actual) }, { "rawIntelCommandSubmissionProven", true },
                { "ownContext", ownContext }, { "ownContextWithoutPrivateData", ownContext },
                { "completion", ownContext ? "D3DKMTSignalSynchronizationObject2 EnqueueCpuEvent" : "stock D3D12 queue fence" },
                { "stockUmdStillUsedFor", ownContext ? "buffer allocation/mapping and parent device initialization" : "buffer allocation and mapping, context initialization, fence signaling/waiting" },
                { "path", "our Gen8 batch -> direct D3DKMTSubmitCommand -> stock Intel KMD -> CPU readback" }
            };
            }
            finally { if (createdContext != null) createdContext.Dispose(); }
        }
        private static void BufferCopy(byte[] source, int start, byte[] destination, int target, int count) { System.Buffer.BlockCopy(source, start, destination, target, count); }
        public void Dispose()
        {
            for (int i = owned.Count - 1; i >= 0; i--) { IntPtr obj = owned[i]; N.Release(ref obj); }
            owned.Clear(); completed.Dispose();
        }
    }

    internal static partial class Program
    {
        internal static uint Mix(uint value)
        {
            unchecked { value ^= value >> 16; value *= 0x7feb352du; value ^= value >> 15; value *= 0x846ca68bu; return value ^ (value >> 16); }
        }
        internal static uint[] Pattern(int count, uint seed)
        {
            uint[] data = new uint[count];
            for (int i = 0; i < count; i++) data[i] = Mix(unchecked(seed + (uint)i * 0x9e3779b9u));
            return data;
        }
        internal static byte[] Bytes(uint[] values)
        { byte[] bytes = new byte[values.Length * 4]; System.Buffer.BlockCopy(values, 0, bytes, 0, bytes.Length); return bytes; }
        internal static string Hash(byte[] bytes)
        { using (var sha = SHA256.Create()) return BitConverter.ToString(sha.ComputeHash(bytes)).Replace("-", "").ToLowerInvariant(); }
        internal static void CheckWords(byte[] actual, uint[] expected, string stage)
        {
            if (actual.Length != expected.Length * 4) throw new InvalidOperationException(stage + " size mismatch");
            for (int i = 0; i < expected.Length; i++)
                if (BitConverter.ToUInt32(actual, i * 4) != expected[i])
                    throw new InvalidOperationException(String.Format("{0} mismatch at {1}: expected {2:X8}, actual {3:X8}", stage, i, expected[i], BitConverter.ToUInt32(actual, i * 4)));
        }
        internal static Dictionary<string, object> ValidateTriangle(byte[] rgba, int width, int height)
        {
            // Analytic barycentric interpolation at pixel centres, well away from edges.
            int[,] points = { { 5, 5 }, { 506, 378 }, { 256, 192 }, { 256, 80 }, { 140, 300 }, { 372, 300 } };
            double ax = width * 0.5, ay = height * 0.125, bx = width * 0.875, by = height * 0.875, cx = width * 0.125, cy = height * 0.875;
            double denominator = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
            var samples = new Dictionary<string, object>();
            for (int n = 0; n < points.GetLength(0); n++)
            {
                int x = points[n, 0], y = points[n, 1], offset = (y * width + x) * 4;
                double a = ((by - cy) * (x + 0.5 - cx) + (cx - bx) * (y + 0.5 - cy)) / denominator;
                double b = ((cy - ay) * (x + 0.5 - cx) + (ax - cx) * (y + 0.5 - cy)) / denominator;
                double c = 1 - a - b;
                int[] expected = a >= 0 && b >= 0 && c >= 0 ? new int[] { (int)Math.Round(a * 255), (int)Math.Round(b * 255), (int)Math.Round(c * 255), 255 } : new int[] { 16, 24, 40, 255 };
                int[] actual = { rgba[offset], rgba[offset + 1], rgba[offset + 2], rgba[offset + 3] };
                for (int channel = 0; channel < 4; channel++) if (Math.Abs(actual[channel] - expected[channel]) > 2) throw new InvalidOperationException("Triangle pixel mismatch at " + x + "," + y + " channel " + channel + "; actual=" + actual[channel] + ", expected=" + expected[channel]);
                samples[x + "," + y] = actual;
            }
            return samples;
        }
        internal static void SavePng(byte[] rgba, int width, int height, string path)
        {
            using (var bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb))
            {
                var data = bitmap.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
                try
                {
                    byte[] row = new byte[width * 4];
                    for (int y = 0; y < height; y++)
                    {
                        for (int x = 0; x < width; x++) { int s = (y * width + x) * 4, d = x * 4; row[d] = rgba[s + 2]; row[d + 1] = rgba[s + 1]; row[d + 2] = rgba[s]; row[d + 3] = rgba[s + 3]; }
                        Marshal.Copy(row, 0, IntPtr.Add(data.Scan0, y * data.Stride), row.Length);
                    }
                }
                finally { bitmap.UnlockBits(data); }
                bitmap.Save(path, ImageFormat.Png);
            }
        }
        public static int Main(string[] args)
        {
            string root = AppDomain.CurrentDomain.BaseDirectory;
            string shader = Path.Combine(root, "..", "shaders", "probe.hlsl");
            string output = args.Length == 0 ? root : Path.GetFullPath(args[0]);
            Directory.CreateDirectory(output);
            if (Array.IndexOf(args, "--native-compute") >= 0)
            {
                if (args.Length < 5) { Console.Error.WriteLine("Native compute needs output --native-compute capture bootstrap count"); return 2; }
                return NativeComputeMain(output, Path.GetFullPath(args[2]), Path.GetFullPath(args[3]), Int32.Parse(args[4]));
            }
            if (Array.IndexOf(args, "--native") >= 0)
            {
                if (args.Length < 3) { Console.Error.WriteLine("Native mode requires: output-directory --native bootstrap-directory"); return 2; }
                return NativeMain(output, Path.GetFullPath(args[2]));
            }
            var report = new Dictionary<string, object> { { "schema", 1 }, { "startedUtc", DateTime.UtcNow.ToString("o") }, { "runtime", Environment.Version.ToString() }, { "processBits", IntPtr.Size * 8 }, { "transport", "stock Intel UMD + KMD via D3D12" }, { "isVulkanDriver", false }, { "rawIntelCommandSubmissionProven", false } };
            int exitCode = 0;
            KernelTrace trace = null;
            bool raw = Array.IndexOf(args, "--raw") >= 0;
            bool captureCompute = Array.IndexOf(args, "--capture-compute") >= 0;
            KernelTrace.CaptureComputeEnabled = captureCompute;
            SaveReport(output, report);
            try
            {
                if (raw || captureCompute || Array.IndexOf(args, "--trace") >= 0) { report["kernelEntryPoints"] = KernelTrace.Inspect(); SaveReport(output, report); trace = new KernelTrace(output); }
                using (var gpu = new Gpu())
                {
                    report["adapter"] = new Dictionary<string, object> { { "name", gpu.Adapter.Description }, { "vendorId", gpu.Adapter.Vendor }, { "deviceId", gpu.Adapter.Device }, { "software", false }, { "luid", gpu.Adapter.Luid.High.ToString("X8") + ":" + gpu.Adapter.Luid.Low.ToString("X8") }, { "shaderModel", gpu.ShaderModel().ToString("X") } };
                    Console.WriteLine("Querying WDDM directly...");
                    KernelTrace.Phase = "direct-wddm-probe";
                    report["wddm"] = Wddm.Probe(gpu.Adapter.Luid);
                    Console.WriteLine(new JavaScriptSerializer().Serialize(report["wddm"]));
                    report["loadedDriverImports"] = Imports.Snapshot();
                    SaveReport(output, report);
                    uint seed; byte[] random = new byte[4]; using (var rng = RandomNumberGenerator.Create()) rng.GetBytes(random); seed = BitConverter.ToUInt32(random, 0);
                    KernelTrace.Phase = "copy";
                    Console.WriteLine("Submitting GPU copy..."); report["copy"] = gpu.CopyTest(seed); Console.WriteLine("PASS: 65536 copied bytes verified");
                    SaveReport(output, report);
                    KernelTrace.Phase = "compute";
                    Console.WriteLine("Dispatching compute shader..."); report["compute"] = gpu.ComputeTest(shader, seed ^ 0x6d2b79f5u); Console.WriteLine("PASS: 16381 shader results verified");
                    if (captureCompute)
                    {
                        if (trace == null || !trace.ComputeSnapshotSaved) throw new InvalidOperationException("Compute capture failed; see wddm/calls.jsonl.");
                        report["computeSnapshotSaved"] = true;
                    }
                    SaveReport(output, report);
                    KernelTrace.Phase = "triangle";
                    Console.WriteLine("Drawing triangle..."); report["triangle"] = gpu.TriangleTest(shader, Path.Combine(output, "triangle.png")); Console.WriteLine("PASS: triangle pixels match barycentric reference");
                    report["baselinePassed"] = true;
                    SaveReport(output, report);
                    if (raw)
                    {
                        Console.WriteLine("Submitting our own native Gen8 batch directly through WDDM...");
                        report["rawStore"] = gpu.RawStoreTest(seed ^ 0xb80ad0e1u, output, false);
                        report["rawIntelCommandSubmissionProven"] = true;
                        Console.WriteLine("PASS: directly submitted Gen8 stores and all untouched guard words verified");
                        SaveReport(output, report);
                        Console.WriteLine("Repeating native stores on our own WDDM context...");
                        report["ownContextStore"] = gpu.RawStoreTest(seed ^ 0x2b8100fdu, output, true);
                        Console.WriteLine("PASS: own WDDM context, native stores, CPU completion event and guard words");
                        SaveReport(output, report);
                        KernelTrace.SaveBootstrap(output);
                        Console.WriteLine("Testing our own WDDM device, paging queue and allocations...");
                        report["nativeMemoryStore"] = Wddm.NativeMemoryStoreTest(gpu.Adapter.Luid, Path.Combine(output, "bootstrap"), output);
                        Console.WriteLine("PASS: native WDDM allocation, GPU mapping, residency, submit, completion, readback and cleanup");
                    }
                }
            }
            catch (Exception error) { if (!report.ContainsKey("baselinePassed")) report["baselinePassed"] = false; report["error"] = error.ToString(); Console.Error.WriteLine(error); exitCode = 1; }
            finally { if (trace != null) trace.Dispose(); }
            report["finishedUtc"] = DateTime.UtcNow.ToString("o");
            report["overallPassed"] = exitCode == 0;
            SaveReport(output, report);
            return exitCode;
        }
        private static void SaveReport(string output, Dictionary<string, object> report)
        { File.WriteAllText(Path.Combine(output, "report.json"), new JavaScriptSerializer().Serialize(report), new UTF8Encoding(false)); }
    }
}
