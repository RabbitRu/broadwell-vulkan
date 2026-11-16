using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Threading;

namespace IrisProbe
{
    internal static partial class Wddm
    {
        [StructLayout(LayoutKind.Sequential)] private struct Open { internal N.Luid Luid; internal uint Adapter; }
        [StructLayout(LayoutKind.Sequential)] private struct Query { internal uint Adapter, Type; internal IntPtr Data; internal uint Size; }
        [StructLayout(LayoutKind.Sequential)] private struct Device
        {
            internal UIntPtr AdapterUnion;
            internal uint Flags, Handle;
            internal IntPtr Commands;
            internal uint CommandBytes;
            internal IntPtr Allocations;
            internal uint AllocationCount;
            internal IntPtr Patches;
            internal uint PatchCount;
        }
        [StructLayout(LayoutKind.Sequential)] private struct Context
        {
            internal uint Device, Node, Engine, Flags;
            internal IntPtr PrivateData;
            internal uint PrivateBytes, ClientHint, Handle;
            internal IntPtr Commands;
            internal uint CommandBytes;
            internal IntPtr Allocations;
            internal uint AllocationCount;
            internal IntPtr Patches;
            internal uint PatchCount;
            internal ulong CommandGpuAddress;
        }
        [DllImport("gdi32.dll")] private static extern int D3DKMTOpenAdapterFromLuid(ref Open data);
        [DllImport("gdi32.dll")] private static extern int D3DKMTCloseAdapter(ref uint adapter);
        [DllImport("gdi32.dll")] private static extern int D3DKMTQueryAdapterInfo(ref Query data);
        [DllImport("gdi32.dll")] private static extern int D3DKMTCreateDevice(ref Device data);
        [DllImport("gdi32.dll")] private static extern int D3DKMTDestroyDevice(ref uint device);
        [DllImport("gdi32.dll")] private static extern int D3DKMTCreateContext(ref Context data);
        [DllImport("gdi32.dll")] private static extern int D3DKMTDestroyContext(ref uint context);
        [StructLayout(LayoutKind.Explicit, Size = 384)] private struct Submit
        {
            [FieldOffset(0)] internal ulong Commands;
            [FieldOffset(8)] internal uint Bytes;
            [FieldOffset(24)] internal uint ContextCount;
            [FieldOffset(28)] internal uint Context;
            [FieldOffset(288)] internal IntPtr Private;
            [FieldOffset(296)] internal uint PrivateBytes;
        }
        [DllImport("gdi32.dll")] private static extern int D3DKMTSubmitCommand(ref Submit submit);
        [StructLayout(LayoutKind.Sequential)] private struct VirtualContext
        {
            internal uint Device, Node, Engine, Flags;
            internal IntPtr Private;
            internal uint PrivateBytes, ClientHint, Context;
        }
        [StructLayout(LayoutKind.Explicit, Size = 464)] private struct SignalEvent
        {
            [FieldOffset(0)] internal uint Context;
            [FieldOffset(136)] internal uint Flags;
            [FieldOffset(400)] internal IntPtr Event;
        }
        [DllImport("gdi32.dll")] private static extern int D3DKMTCreateContextVirtual(ref VirtualContext context);
        [DllImport("gdi32.dll")] private static extern int D3DKMTSignalSynchronizationObject2(ref SignalEvent signal);
        internal sealed class OwnContext : IDisposable
        {
            internal uint Handle;
            private readonly AutoResetEvent completed = new AutoResetEvent(false);
            internal OwnContext(uint device)
            {
                if (device == 0) throw new InvalidOperationException("No traced Intel render device handle.");
                var context = new VirtualContext { Device = device, ClientHint = 4 };
                N.Check(D3DKMTCreateContextVirtual(ref context), "Create our own WDDM virtual context (no private data, Vulkan client hint)");
                Handle = context.Context;
            }
            internal void Wait()
            {
                // EnqueueCpuEvent, with SignalAtSubmission unset: signal after GPU completion.
                var signal = new SignalEvent { Context = Handle, Flags = 2, Event = completed.SafeWaitHandle.DangerousGetHandle() };
                N.Check(D3DKMTSignalSynchronizationObject2(ref signal), "Enqueue CPU completion event on own WDDM context");
                uint wait = N.WaitForSingleObject(signal.Event, 15000);
                if (wait != 0) throw new TimeoutException("Own WDDM context completion wait: 0x" + wait.ToString("X8"));
            }
            public void Dispose()
            {
                if (Handle != 0) { int status = D3DKMTDestroyContext(ref Handle); Handle = 0; N.Check(status, "Destroy own WDDM context"); }
                completed.Dispose();
            }
        }
        internal static int SubmitBatch(uint context, ulong address, uint bytes, byte[] header)
        {
            if (header == null || header.Length != 80 || BitConverter.ToUInt32(header, 0) != 0x00171433)
                throw new InvalidOperationException("The captured Intel 20.19 submit header was not recognized.");
            IntPtr privateData = Marshal.AllocHGlobal(header.Length);
            try
            {
                Marshal.Copy(header, 0, privateData, header.Length);
                var submit = new Submit { Commands = address, Bytes = bytes, ContextCount = 1, Context = context, Private = privateData, PrivateBytes = (uint)header.Length };
                return D3DKMTSubmitCommand(ref submit);
            }
            finally { Marshal.FreeHGlobal(privateData); }
        }

        internal static Dictionary<string, object> Probe(N.Luid luid)
        {
            if (Marshal.SizeOf(typeof(Device)) != 64 || Marshal.SizeOf(typeof(Context)) != 96 || Marshal.SizeOf(typeof(Query)) != 24)
                throw new InvalidOperationException("WDDM x64 structure layout mismatch.");
            var result = new Dictionary<string, object> { { "rawIntelCommandSubmissionProven", false } };
            var open = new Open { Luid = luid };
            int status = D3DKMTOpenAdapterFromLuid(ref open);
            result["openAdapterStatus"] = Hex(status);
            if (status < 0) return result;
            try
            {
                result["wddmVersion"] = QueryValue(open.Adapter, 13, 4);
                result["umdVersion"] = QueryValue(open.Adapter, 18, 8);
                var device = new Device { AdapterUnion = (UIntPtr)open.Adapter };
                status = D3DKMTCreateDevice(ref device);
                result["createDeviceStatus"] = Hex(status);
                if (status < 0) return result;
                try
                {
                    // One documented, zero-private-data request; no guessed blobs or commands.
                    var context = new Context { Device = device.Handle };
                    status = D3DKMTCreateContext(ref context);
                    result["createContextWithoutPrivateDataStatus"] = Hex(status);
                    if (status >= 0)
                    {
                        try
                        {
                            result["contextCommandBufferBytes"] = context.CommandBytes;
                            result["contextAllocationSlots"] = context.AllocationCount;
                            result["contextPatchSlots"] = context.PatchCount;
                            result["contextHasGpuAddress"] = context.CommandGpuAddress != 0;
                        }
                        finally { result["destroyContextStatus"] = Hex(D3DKMTDestroyContext(ref context.Handle)); }
                    }
                }
                finally { result["destroyDeviceStatus"] = Hex(D3DKMTDestroyDevice(ref device.Handle)); }
            }
            finally { result["closeAdapterStatus"] = Hex(D3DKMTCloseAdapter(ref open.Adapter)); }
            return result;
        }
        private static object QueryValue(uint adapter, uint type, int bytes)
        {
            IntPtr memory = Marshal.AllocHGlobal(bytes);
            try
            {
                for (int i = 0; i < bytes; i++) Marshal.WriteByte(memory, i, 0);
                var query = new Query { Adapter = adapter, Type = type, Data = memory, Size = (uint)bytes };
                int status = D3DKMTQueryAdapterInfo(ref query);
                if (status < 0) return Hex(status);
                if (bytes == 4) return Marshal.ReadInt32(memory);
                ulong version = unchecked((ulong)Marshal.ReadInt64(memory));
                return String.Format("{0}.{1}.{2}.{3}", version >> 48, (version >> 32) & 65535, (version >> 16) & 65535, version & 65535);
            }
            finally { Marshal.FreeHGlobal(memory); }
        }
        private static string Hex(int value) { return "0x" + value.ToString("X8"); }
    }
}
