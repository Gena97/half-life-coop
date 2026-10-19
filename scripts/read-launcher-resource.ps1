Set-StrictMode -Version Latest

if (!('HalfLifeCoopBuild.ResourceReader' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
namespace HalfLifeCoopBuild
{
    public static class ResourceReader
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool FreeLibrary(IntPtr module);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint SizeofResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr LockResource(IntPtr resource);
        public static byte[] Read(string path, int id)
        {
            // Файл открывается только как данные, код EXE не выполняется
            IntPtr module = LoadLibraryExW(path, IntPtr.Zero, 0x60);
            if (module == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            try
            {
                IntPtr resource = FindResourceW(module, new IntPtr(id), new IntPtr(10));
                if (resource == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
                uint size = SizeofResource(module, resource);
                if (size == 0 || size > 134217728) throw new InvalidOperationException("Недопустимый размер ресурса");
                IntPtr memory = LockResource(LoadResource(module, resource));
                if (memory == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
                byte[] bytes = new byte[size];
                Marshal.Copy(memory, bytes, 0, (int)size);
                return bytes;
            }
            finally { FreeLibrary(module); }
        }
    }
}
'@
}
