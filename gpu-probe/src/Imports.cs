using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace IrisProbe
{
    internal static class Imports
    {
        // Read-only inventory of imports in this process; useful for choosing trace points.
        internal static List<Dictionary<string, object>> Snapshot()
        {
            var modules = new List<Dictionary<string, object>>();
            using (var process = Process.GetCurrentProcess())
            {
                foreach (ProcessModule module in process.Modules)
                {
                    string name = module.ModuleName.ToLowerInvariant();
                    if (!(name.StartsWith("igd") || name.StartsWith("igc") || name.StartsWith("d3d12") || name == "dxgi.dll" || name == "gdi32.dll" || name == "gdi32full.dll")) continue;
                    var details = new Dictionary<string, object> { { "name", module.ModuleName }, { "path", module.FileName }, { "version", module.FileVersionInfo.FileVersion } };
                    try { details["kernelThunks"] = KernelThunks(module); }
                    catch (Exception error) { details["error"] = error.Message; }
                    modules.Add(details);
                }
            }
            return modules;
        }
        private static List<Dictionary<string, object>> KernelThunks(ProcessModule module)
        {
            var result = new List<Dictionary<string, object>>();
            IntPtr address = module.BaseAddress;
            int size = module.ModuleMemorySize;
            if (Marshal.ReadInt16(address) != 0x5a4d) return result;
            int pe = Marshal.ReadInt32(address, 0x3c);
            if (pe < 0 || pe + 144 > size || Marshal.ReadInt32(address, pe) != 0x4550) return result;
            if (Marshal.ReadInt16(address, pe + 24) != 0x20b) return result;
            int importRva = Marshal.ReadInt32(address, pe + 24 + 120);
            if (importRva <= 0) return result;
            for (int entry = importRva; entry + 20 < size; entry += 20)
            {
                int lookupRva = Marshal.ReadInt32(address, entry), nameRva = Marshal.ReadInt32(address, entry + 12), iatRva = Marshal.ReadInt32(address, entry + 16);
                if (nameRva == 0) break;
                if (nameRva < 0 || nameRva >= size || lookupRva <= 0 || iatRva <= 0) continue;
                string library = Marshal.PtrToStringAnsi(IntPtr.Add(address, nameRva));
                for (int index = 0; lookupRva + index * 8 + 8 <= size && iatRva + index * 8 + 8 <= size; index++)
                {
                    ulong thunk = unchecked((ulong)Marshal.ReadInt64(address, lookupRva + index * 8));
                    if (thunk == 0) break;
                    if ((thunk >> 63) != 0 || thunk + 2 >= (ulong)size) continue;
                    string function = Marshal.PtrToStringAnsi(IntPtr.Add(address, checked((int)thunk + 2)));
                    if (!(function.StartsWith("D3DKMT", StringComparison.Ordinal) || function.StartsWith("NtGdiDdDDI", StringComparison.Ordinal))) continue;
                    result.Add(new Dictionary<string, object> { { "library", library }, { "function", function }, { "iatRva", iatRva + index * 8 } });
                }
            }
            return result;
        }
    }
}
