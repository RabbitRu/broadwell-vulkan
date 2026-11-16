using System;
using System.Runtime.InteropServices;

namespace IrisProbe
{
    // Windows x64 ABI. Vtable slots and layouts follow Microsoft's DirectX-Headers.
    internal static class N
    {
        internal static readonly Guid FactoryIid = new Guid("770aae78-f26f-4dba-a829-253c83d1b387");
        internal static readonly Guid DeviceIid = new Guid("189819f1-1db6-4b57-be54-1821339b85f7");
        internal static readonly Guid QueueIid = new Guid("0ec870a6-5d7e-4c22-8cfc-5baae07616ed");
        internal static readonly Guid AllocatorIid = new Guid("6102dee4-af59-4b09-b999-b44d73f09b24");
        internal static readonly Guid ListIid = new Guid("5b160d0f-ac1b-4185-8ba8-b3ae42a5a455");
        internal static readonly Guid ResourceIid = new Guid("696442be-a72e-4059-bc79-5b5c98040fad");
        internal static readonly Guid FenceIid = new Guid("0a753dcf-c4d8-4b91-adf6-be5a60d95a76");
        internal static readonly Guid RootIid = new Guid("c54a6b66-72df-4ee8-8be5-a946a1429214");
        internal static readonly Guid PipelineIid = new Guid("765a30f3-f624-4c6f-a828-ace948622445");
        internal static readonly Guid HeapIid = new Guid("8efb471d-616c-4f49-90f7-127bb763fa51");

        [DllImport("dxgi.dll")] internal static extern int CreateDXGIFactory1(ref Guid iid, out IntPtr factory);
        [DllImport("d3d12.dll")] internal static extern int D3D12CreateDevice(IntPtr adapter, uint level, ref Guid iid, out IntPtr device);
        [DllImport("d3d12.dll")] internal static extern int D3D12SerializeRootSignature(ref RootDesc desc, uint version, out IntPtr blob, out IntPtr errors);
        [DllImport("d3dcompiler_47.dll", CharSet = CharSet.Ansi)]
        internal static extern int D3DCompile(byte[] source, UIntPtr size, string name, IntPtr defines, IntPtr include, string entry, string target, uint flags, uint flags2, out IntPtr blob, out IntPtr errors);
        [DllImport("kernel32.dll", SetLastError = true)] internal static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        internal static T Fn<T>(IntPtr obj, int slot) where T : class
        {
            return (T)(object)Marshal.GetDelegateForFunctionPointer(Marshal.ReadIntPtr(Marshal.ReadIntPtr(obj), slot * IntPtr.Size), typeof(T));
        }
        internal static void Check(int hr, string name)
        {
            if (hr < 0) throw new InvalidOperationException(name + " failed: 0x" + hr.ToString("X8"));
        }
        internal static void Release(ref IntPtr obj)
        {
            if (obj != IntPtr.Zero) { Marshal.Release(obj); obj = IntPtr.Zero; }
        }

        [StructLayout(LayoutKind.Sequential)] internal struct Luid { internal uint Low; internal int High; }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)] internal struct AdapterDesc
        {
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] internal string Description;
            internal uint Vendor, Device, Subsystem, Revision;
            internal UIntPtr VideoMemory, SystemMemory, SharedMemory;
            internal Luid Luid;
            internal uint Flags;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct QueueDesc { internal int Type, Priority; internal uint Flags, NodeMask; }
        [StructLayout(LayoutKind.Sequential)] internal struct HeapProps { internal int Type, CpuPage, Pool; internal uint CreationMask, VisibleMask; }
        [StructLayout(LayoutKind.Sequential)] internal struct ResourceDesc
        {
            internal int Dimension;
            internal ulong Alignment, Width;
            internal uint Height;
            internal ushort Depth, Mips;
            internal int Format;
            internal uint Samples, Quality;
            internal int Layout;
            internal uint Flags;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct Range { internal UIntPtr Begin, End; }
        [StructLayout(LayoutKind.Sequential)] internal struct Barrier
        {
            internal uint Type, Flags;
            internal IntPtr Resource;
            internal uint Subresource, Before, After;
        }
        [StructLayout(LayoutKind.Explicit, Size = 32)] internal struct RootParameter
        {
            [FieldOffset(0)] internal int Type;
            [FieldOffset(8)] internal uint Register;
            [FieldOffset(12)] internal uint Space;
            [FieldOffset(16)] internal uint Count;
            [FieldOffset(24)] internal uint Visibility;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct RootDesc
        {
            internal uint ParameterCount;
            internal IntPtr Parameters;
            internal uint SamplerCount;
            internal IntPtr Samplers;
            internal uint Flags;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct Bytecode { internal IntPtr Pointer; internal UIntPtr Length; }
        [StructLayout(LayoutKind.Sequential)] internal struct CachedPipeline { internal IntPtr Pointer; internal UIntPtr Length; }
        [StructLayout(LayoutKind.Sequential)] internal struct ComputeDesc
        {
            internal IntPtr Root;
            internal Bytecode Shader;
            internal uint NodeMask;
            internal CachedPipeline Cached;
            internal uint Flags;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct StreamOutput
        {
            internal IntPtr Entries;
            internal uint Count;
            internal IntPtr Strides;
            internal uint StrideCount, RasterizedStream;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct RenderBlend
        {
            internal int Enable, LogicEnable, Source, Destination, Operation, SourceAlpha, DestinationAlpha, OperationAlpha, LogicOperation;
            internal byte WriteMask;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct Blend
        {
            internal int AlphaCoverage, Independent;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] internal RenderBlend[] Targets;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct Rasterizer
        {
            internal int Fill, Cull, CounterClockwise, DepthBias;
            internal float DepthBiasClamp, SlopeDepthBias;
            internal int DepthClip, Multisample, AntialiasedLines;
            internal uint ForcedSamples;
            internal int Conservative;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct StencilOp { internal int Fail, DepthFail, Pass, Function; }
        [StructLayout(LayoutKind.Sequential)] internal struct DepthStencil
        {
            internal int DepthEnable, DepthWriteMask, DepthFunction, StencilEnable;
            internal byte ReadMask, WriteMask;
            internal StencilOp Front, Back;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct InputLayout { internal IntPtr Elements; internal uint Count; }
        [StructLayout(LayoutKind.Sequential)] internal struct GraphicsDesc
        {
            internal IntPtr Root;
            internal Bytecode VS, PS, DS, HS, GS;
            internal StreamOutput Stream;
            internal Blend Blend;
            internal uint SampleMask;
            internal Rasterizer Rasterizer;
            internal DepthStencil DepthStencil;
            internal InputLayout Input;
            internal int StripCut, Topology;
            internal uint TargetCount;
            [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)] internal int[] Formats;
            internal int DepthFormat;
            internal uint Samples, Quality, NodeMask;
            internal CachedPipeline Cached;
            internal uint Flags;
        }
        [StructLayout(LayoutKind.Sequential)] internal struct DescriptorHeapDesc { internal int Type; internal uint Count, Flags, NodeMask; }
        [StructLayout(LayoutKind.Sequential)] internal struct Viewport { internal float X, Y, Width, Height, MinDepth, MaxDepth; }
        [StructLayout(LayoutKind.Sequential)] internal struct Rect { internal int Left, Top, Right, Bottom; }
        [StructLayout(LayoutKind.Sequential)] internal struct Footprint
        {
            internal ulong Offset;
            internal int Format;
            internal uint Width, Height, Depth, RowPitch;
        }
        [StructLayout(LayoutKind.Explicit, Size = 48)] internal struct TextureLocation
        {
            [FieldOffset(0)] internal IntPtr Resource;
            [FieldOffset(8)] internal int Type;
            [FieldOffset(16)] internal Footprint Footprint;
            [FieldOffset(16)] internal uint Subresource;
        }

        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int EnumAdapter(IntPtr self, uint index, out IntPtr adapter);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int GetAdapterDesc(IntPtr self, out AdapterDesc desc);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateQueue(IntPtr self, ref QueueDesc desc, ref Guid iid, out IntPtr queue);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateAllocator(IntPtr self, int type, ref Guid iid, out IntPtr allocator);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateList(IntPtr self, uint mask, int type, IntPtr allocator, IntPtr state, ref Guid iid, out IntPtr list);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateResource(IntPtr self, ref HeapProps heap, uint heapFlags, ref ResourceDesc desc, uint state, IntPtr clear, ref Guid iid, out IntPtr resource);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateFence(IntPtr self, ulong initial, uint flags, ref Guid iid, out IntPtr fence);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int Feature(IntPtr self, int feature, IntPtr data, uint bytes);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int Map(IntPtr self, uint subresource, ref Range range, out IntPtr data);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void Unmap(IntPtr self, uint subresource, ref Range written);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void CopyBuffer(IntPtr self, IntPtr dst, ulong dstOffset, IntPtr src, ulong srcOffset, ulong size);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void Barriers(IntPtr self, uint count, ref Barrier barrier);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int HResult(IntPtr self);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int ResetList(IntPtr self, IntPtr allocator, IntPtr pipeline);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void Execute(IntPtr self, uint count, ref IntPtr list);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int Signal(IntPtr self, IntPtr fence, ulong value);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate ulong CompletedValue(IntPtr self);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int FenceEvent(IntPtr self, ulong value, IntPtr handle);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate IntPtr BlobPointer(IntPtr self);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate UIntPtr BlobSize(IntPtr self);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateRoot(IntPtr self, uint mask, IntPtr data, UIntPtr size, ref Guid iid, out IntPtr root);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateCompute(IntPtr self, ref ComputeDesc desc, ref Guid iid, out IntPtr pipeline);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateGraphics(IntPtr self, ref GraphicsDesc desc, ref Guid iid, out IntPtr pipeline);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void SetObject(IntPtr self, IntPtr obj);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate ulong GpuAddress(IntPtr self);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void RootAddress(IntPtr self, uint index, ulong address);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void RootConstants(IntPtr self, uint index, uint count, [In] uint[] values, uint offset);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void Dispatch(IntPtr self, uint x, uint y, uint z);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate int CreateDescriptorHeap(IntPtr self, ref DescriptorHeapDesc desc, ref Guid iid, out IntPtr heap);
        // Struct returns from these COM methods use an explicit return buffer on Windows x64.
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate IntPtr CpuDescriptor(IntPtr self, out ulong handle);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void CreateRtv(IntPtr self, IntPtr resource, IntPtr desc, ulong handle);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void SetViewport(IntPtr self, uint count, ref Viewport viewport);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void SetScissor(IntPtr self, uint count, ref Rect rect);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void SetTargets(IntPtr self, uint count, ref ulong targets, int singleRange, IntPtr depth);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void ClearTarget(IntPtr self, ulong target, [In] float[] color, uint count, IntPtr rects);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void Topology(IntPtr self, int topology);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void Draw(IntPtr self, uint vertices, uint instances, uint startVertex, uint startInstance);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void GetFootprints(IntPtr self, ref ResourceDesc desc, uint first, uint count, ulong offset, out Footprint layout, out uint rows, out ulong rowBytes, out ulong total);
        [UnmanagedFunctionPointer(CallingConvention.StdCall)] internal delegate void CopyTexture(IntPtr self, ref TextureLocation dst, uint x, uint y, uint z, ref TextureLocation src, IntPtr box);

        internal static void ValidateAbi()
        {
            if (IntPtr.Size != 8) throw new PlatformNotSupportedException("This probe requires a 64-bit process.");
            Size<AdapterDesc>(312); Size<ResourceDesc>(56); Size<HeapProps>(20);
            Size<Barrier>(32); Size<RootParameter>(32); Size<RootDesc>(40);
            Size<ComputeDesc>(56); Size<GraphicsDesc>(656); Size<TextureLocation>(48);
            if (Marshal.OffsetOf(typeof(GraphicsDesc), "Formats").ToInt32() != 580)
                throw new InvalidOperationException("Graphics pipeline ABI mismatch.");
        }
        private static void Size<T>(int expected)
        {
            if (Marshal.SizeOf(typeof(T)) != expected) throw new InvalidOperationException(typeof(T).Name + " ABI size mismatch: " + Marshal.SizeOf(typeof(T)));
        }
    }
}
