// Client for the mtdrop register proxy (driver/proxy). Diagnostic tool for chip bring-up.
//
//   regtool info      proxy version and BAR0 size
//   regtool chipid    spike S1 of docs/adr/0002: read the chip ID through the L1 remap window
//
// "chipid" performs exactly two register WRITES, both to the host-side remap window register
// (MT_HIF_REMAP_L1): one to point the window at the chip-ID block, one to put the old value back.
// Everything else is a read. It does not touch power state, DMA, interrupts or firmware.
// Register addresses: Linux mt76 (BSD-3-Clause-Clear), mt7921/regs.h and mt792x_regs.h.

using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

return Run(args);

static int Run(string[] args)
{
    string command = args.Length > 0 ? args[0] : "";
    if (command is not ("info" or "chipid"))
    {
        Console.Error.WriteLine("usage: regtool info | chipid");
        return 2;
    }
    try
    {
        using Proxy proxy = Proxy.Open();
        Proxy.Info info = proxy.GetInfo();
        Console.WriteLine($"proxy version {info.Version}, BAR0 length 0x{info.BarLength:X}, direct mapped: {info.DirectMapped != 0}");
        if (info.BarLength < 0x100000)
        {
            Console.WriteLine("RESULT: FAIL - BAR0 is smaller than 1 MiB; this is not the expected MT7921 register window");
            return 1;
        }
        return command == "info" ? 0 : ChipId(proxy);
    }
    catch (Exception e) when (e is Win32Exception or InvalidOperationException)
    {
        Console.WriteLine($"RESULT: FAIL - {e.Message}");
        return 1;
    }
}

static int ChipId(Proxy proxy)
{
    const uint RemapL1 = 0xFE24C;           // MT_HIF_REMAP_L1: low 16 bits select the 64 KiB block shown at 0x40000
    const uint RemapWindow = 0x40000;       // MT_HIF_REMAP_BASE_L1
    const uint ChipIdBlock = 0x7001;        // MT_HW_CHIPID 0x70010200 >> 16

    Console.WriteLine("-- direct reads (no writes)");
    (string Name, uint Offset)[] direct =
    [
        ("MT_HIF_REMAP_L1", RemapL1),
        ("MT_CONN_ON_LPCTL (own/sleep state)", 0xE0010),
        ("MT_CONN_ON_MISC (firmware state)", 0xE00F0),
        ("MT_WFDMA0_GLO_CFG (DMA enable bits)", 0xD4208),
        ("MT_WFDMA0_HOST_INT_ENA", 0xD4204),
        ("MT_PCIE_MAC_INT_ENABLE", 0x10188),
    ];
    uint[] values = proxy.Read([.. direct.Select(d => d.Offset)]);
    for (int i = 0; i < direct.Length; i++)
        Console.WriteLine($"   0x{direct[i].Offset:X5} = 0x{values[i]:X8}  {direct[i].Name}");
    uint savedRemap = values[0];
    if ((values[3] & 0x5) != 0)
        Console.WriteLine("   note: TX/RX DMA enable bits are set (left as found; this tool never touches DMA)");

    Console.WriteLine("-- remap window -> chip ID block (2 writes: set, restore)");
    // One batch, executed in order inside the driver: set window, read back, read 3 registers, restore, read back.
    Proxy.Op[] ops =
    [
        Proxy.Op.Write(RemapL1, (savedRemap & 0xFFFF0000) | ChipIdBlock),
        Proxy.Op.Read(RemapL1),
        Proxy.Op.Read(RemapWindow + 0x200),   // MT_HW_CHIPID
        Proxy.Op.Read(RemapWindow + 0x204),   // MT_HW_REV
        Proxy.Op.Read(RemapWindow + 0x020),   // MT_HW_BOUND
        Proxy.Op.Write(RemapL1, savedRemap),
        Proxy.Op.Read(RemapL1),
    ];
    proxy.Batch(ops);
    uint chip = ops[2].Value, rev = ops[3].Value, bound = ops[4].Value, restored = ops[6].Value;
    Console.WriteLine($"   remap readback  = 0x{ops[1].Value:X8}");
    Console.WriteLine($"   MT_HW_CHIPID    = 0x{chip:X8}");
    Console.WriteLine($"   MT_HW_REV       = 0x{rev:X8}");
    Console.WriteLine($"   MT_HW_BOUND     = 0x{bound:X8}");
    Console.WriteLine($"   remap restored  = 0x{restored:X8} (was 0x{savedRemap:X8})");

    if (restored != savedRemap)
        Console.WriteLine("   WARNING: remap register did not return to its previous value");
    uint asic = (chip << 16) | (rev & 0xFF);
    Console.WriteLine($"   ASIC revision as Linux prints it: {asic:x8} (Linux on this laptop: 79610010)");

    if (chip == 0x7961)
    {
        Console.WriteLine(asic == 0x79610010
            ? "RESULT: PASS - chip ID 0x7961 and ASIC revision match the Linux reading"
            : "RESULT: PASS - chip ID 0x7961 (revision differs from the Linux reading; record it)");
        return 0;
    }
    Console.WriteLine(chip is 0 or 0xFFFFFFFF
        ? "RESULT: FAIL - chip ID block not readable (all zeros/ones). The chip may be in a low-power or firmware-owned state."
        : $"RESULT: FAIL - unexpected chip ID 0x{chip:X}");
    return 1;
}

/// <summary>Handle to the proxy's device interface. Mirrors driver/proxy/mtdrop_proxy_ioctl.h.</summary>
sealed class Proxy : IDisposable
{
    private static readonly Guid Interface = new("6f0c2b0e-7d43-4c55-9a51-3e1f6d2a9b74");
    private const uint GetInfoCode = 0x222000;   // CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
    private const uint BatchCode = 0x222004;     // function 0x801
    private const int MaxOps = 4096;
    private readonly SafeFileHandle _handle;

    private Proxy(SafeFileHandle handle) => _handle = handle;

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct Info
    {
        public uint Version;
        public uint BarLength;
        public ulong BarPhysical;
        public uint DirectMapped;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct Op
    {
        public uint Offset;
        public uint Value;
        public byte Width;
        public byte Kind;      // 0 read, 1 write
        public ushort Reserved;

        public static Op Read(uint offset) => new() { Offset = offset, Width = 4, Kind = 0 };
        public static Op Write(uint offset, uint value) => new() { Offset = offset, Value = value, Width = 4, Kind = 1 };
    }

    public static Proxy Open()
    {
        Guid guid = Interface;
        int rc = CM_Get_Device_Interface_List_SizeW(out uint length, ref guid, null, 0);
        if (rc != 0 || length <= 1)
            throw new InvalidOperationException("the mtdrop proxy driver is not bound to any device (interface not present)");
        char[] buffer = new char[length];
        rc = CM_Get_Device_Interface_ListW(ref guid, null, buffer, length, 0);
        if (rc != 0) throw new InvalidOperationException($"CM_Get_Device_Interface_List failed ({rc})");
        string path = new string(buffer).Split('\0', StringSplitOptions.RemoveEmptyEntries).FirstOrDefault()
            ?? throw new InvalidOperationException("the mtdrop proxy driver is not bound to any device");
        SafeFileHandle handle = CreateFileW(path, 0xC0000000, 0, IntPtr.Zero, 3, 0, IntPtr.Zero);
        if (handle.IsInvalid)
        {
            int error = Marshal.GetLastWin32Error();
            throw new Win32Exception(error, error == 5
                ? "access denied opening the proxy device (run elevated)"
                : $"cannot open the proxy device: {new Win32Exception(error).Message}");
        }
        return new Proxy(handle);
    }

    public unsafe Info GetInfo()
    {
        Info info = default;
        if (!DeviceIoControl(_handle, GetInfoCode, null, 0, &info, (uint)sizeof(Info), out uint got, IntPtr.Zero) || got != sizeof(Info))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "GET_INFO failed");
        return info;
    }

    /// <summary>Runs the operations in order inside the driver; read results are stored back into <paramref name="ops"/>.</summary>
    public unsafe void Batch(Op[] ops)
    {
        if (ops.Length is 0 or > MaxOps) throw new ArgumentException("bad batch size");
        uint bytes = (uint)(ops.Length * sizeof(Op));
        Op[] result = new Op[ops.Length];
        fixed (Op* input = ops)
        fixed (Op* output = result)
        {
            if (!DeviceIoControl(_handle, BatchCode, input, bytes, output, bytes, out uint got, IntPtr.Zero) || got != bytes)
                throw new Win32Exception(Marshal.GetLastWin32Error(), "BATCH failed");
        }
        result.CopyTo(ops, 0);
    }

    public uint[] Read(uint[] offsets)
    {
        Op[] ops = [.. offsets.Select(Op.Read)];
        Batch(ops);
        return [.. ops.Select(o => o.Value)];
    }

    public void Dispose() => _handle.Dispose();

    [DllImport("CfgMgr32.dll", CharSet = CharSet.Unicode)]
    private static extern int CM_Get_Device_Interface_List_SizeW(out uint length, ref Guid interfaceClass, string? deviceId, uint flags);

    [DllImport("CfgMgr32.dll", CharSet = CharSet.Unicode)]
    private static extern int CM_Get_Device_Interface_ListW(ref Guid interfaceClass, string? deviceId, [Out] char[] buffer, uint length, uint flags);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern unsafe bool DeviceIoControl(SafeFileHandle device, uint code, void* input, uint inputSize, void* output, uint outputSize, out uint returned, IntPtr overlapped);
}
