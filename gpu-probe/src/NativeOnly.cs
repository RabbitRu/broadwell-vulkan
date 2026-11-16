using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Web.Script.Serialization;

namespace IrisProbe
{
    internal static partial class Program
    {
        private static N.AdapterDesc FindIris()
        {
            N.ValidateAbi(); IntPtr factory = IntPtr.Zero;
            try
            {
                Guid iid = N.FactoryIid; N.Check(N.CreateDXGIFactory1(ref iid, out factory), "Native-only DXGI enumeration");
                for (uint i = 0; ; i++)
                {
                    IntPtr adapter; int status = N.Fn<N.EnumAdapter>(factory, 12)(factory, i, out adapter);
                    if (status == unchecked((int)0x887a0002)) break;
                    N.Check(status, "Native-only EnumAdapters1");
                    try
                    {
                        N.AdapterDesc desc; N.Check(N.Fn<N.GetAdapterDesc>(adapter, 10)(adapter, out desc), "Native-only GetDesc1");
                        if (desc.Vendor == 0x8086 && desc.Device == 0x162b && (desc.Flags & 2) == 0) return desc;
                    }
                    finally { N.Release(ref adapter); }
                }
                throw new InvalidOperationException("Hardware Iris 6100 was not found.");
            }
            finally { N.Release(ref factory); }
        }
        private static int NativeMain(string output, string bootstrap)
        {
            var report = new Dictionary<string, object> { { "schema", 2 }, { "startedUtc", DateTime.UtcNow.ToString("o") }, { "nativeOnly", true }, { "isVulkanDriver", false }, { "usesCapturedIntelAllocationMetadata", true } };
            int exitCode = 0;
            try
            {
                N.AdapterDesc adapter = FindIris();
                Console.WriteLine("Native-only hardware: {0} ({1:X4}:{2:X4})", adapter.Description, adapter.Vendor, adapter.Device);
                report["adapter"] = new Dictionary<string, object> { { "name", adapter.Description }, { "vendorId", adapter.Vendor }, { "deviceId", adapter.Device }, { "software", false } };
                report["nativeMemoryStore"] = Wddm.NativeMemoryStoreTest(adapter.Luid, bootstrap, output);
                var modules = new List<string>(); bool d3d12Loaded = false;
                using (var process = Process.GetCurrentProcess())
                    foreach (ProcessModule module in process.Modules)
                    {
                        string name = module.ModuleName.ToLowerInvariant();
                        if (name.StartsWith("d3d12") || name.StartsWith("igd12")) { d3d12Loaded = true; modules.Add(module.ModuleName); }
                    }
                report["d3d12OrIntelD3d12UmdLoaded"] = d3d12Loaded;
                report["unexpectedD3d12Modules"] = modules;
                if (d3d12Loaded) throw new InvalidOperationException("Native-only independence check found D3D12 modules loaded.");
                report["rawIntelCommandSubmissionProven"] = true;
                Console.WriteLine("PASS: our device, context, allocation, GPU VA, submission, CPU event, readback and cleanup");
                Console.WriteLine("PASS: D3D12 runtime and Intel D3D12 UMD are not loaded in this process");
            }
            catch (Exception error) { report["error"] = error.ToString(); Console.Error.WriteLine(error); exitCode = 1; }
            report["overallPassed"] = exitCode == 0; report["finishedUtc"] = DateTime.UtcNow.ToString("o"); SaveReport(output, report); return exitCode;
        }
    }
}
