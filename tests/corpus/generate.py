"""Generate inert PE32+ metadata. All addresses are synthetic; no executable code."""
import struct
import sys
from pathlib import Path


def fixture():
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 132, 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = 152
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<QII", data, opt + 24, 0x140000000, 4096, 512)
    struct.pack_into("<II", data, opt + 56, 8192, 512)
    struct.pack_into("<H", data, opt + 68, 3)
    struct.pack_into("<I", data, opt + 108, 16)
    struct.pack_into("<8sIIIIIIHHI", data, opt + 240, b".rdata", 512, 4096,
                     512, 512, 0, 0, 0, 0, 0x40000040)

    def put(offset, layout, *values):
        struct.pack_into("<" + layout, data, 512 + offset, *values)

    def directory(index, offset, length):
        struct.pack_into("<II", data, opt + 112 + 8 * index, 0x1000 + offset, length)

    directory(1, 0, 40)
    put(0, "5I", 0x1038, 0, 0, 0x1028, 0x1050)
    put(0x28, "12s", b"fixture.dll")
    for table in (0x38, 0x50):
        put(table, "3Q", 0x1068, (1 << 63) | 17, 0)
    put(0x68, "H6s", 7, b"Probe")

    directory(0, 0x80, 0x78)
    put(0x80, "IIHH7I", 0, 0, 0, 0, 0x10A8, 5, 2, 2, 0x10B8, 0x10C0, 0x10C8)
    put(0xA8, "10s", b"inert.dll")
    put(0xB8, "2I", 0x1180, 0x10E8)
    put(0xC0, "2I2H", 0x10CC, 0x10D8, 0, 0)
    put(0xCC, "8s", b"Alias")
    put(0xD8, "8s", b"Value")
    put(0xE8, "16s", b"fixture.Probe")

    directory(5, 0x100, 12)
    put(0x100, "IIHH", 0x1000, 12, (10 << 12) | 0x140, 0)
    directory(9, 0x110, 40)
    base = 0x140000000
    put(0x110, "QQQQII", base + 0x1180, base + 0x1188, base + 0x1178,
        base + 0x1140, 4, 0)
    put(0x140, "QQ", base + 0x1180, 0)
    directory(3, 0x158, 12)
    put(0x158, "III", 0x1180, 0x1188, 0x1190)
    return data


# DLL list of a Series template project, taken from the xbdepends readme.
# Only the names are public, so every DLL here has zero symbols.
PUBLISHED_SCARLETT_MODULES = [
    "api-ms-win-core-debug-l1-1-0.dll", "api-ms-win-core-errorhandling-l1-1-0.dll",
    "api-ms-win-core-handle-l1-1-0.dll", "api-ms-win-core-heap-l1-1-0.dll",
    "api-ms-win-core-interlocked-l1-1-0.dll", "api-ms-win-core-libraryloader-l1-2-0.dll",
    "api-ms-win-core-localization-l1-2-0.dll", "api-ms-win-core-memory-l1-1-0.dll",
    "api-ms-win-core-processthreads-l1-1-0.dll", "api-ms-win-core-processthreads-l1-1-1.dll",
    "api-ms-win-core-profile-l1-1-0.dll", "api-ms-win-core-psm-appnotify-l1-1-0.dll",
    "api-ms-win-core-psm-appnotify-l1-1-1.dll", "api-ms-win-core-registry-l1-1-0.dll",
    "api-ms-win-core-registry-l2-1-0.dll", "api-ms-win-core-rtlsupport-l1-1-0.dll",
    "api-ms-win-core-string-l1-1-0.dll", "api-ms-win-core-synch-l1-1-0.dll",
    "api-ms-win-core-sysinfo-l1-1-0.dll", "d3d12_xs.dll",
    "ext-ms-win-rtcore-ntuser-message-l1-1-0.dll",
    "ext-ms-win-rtcore-ntuser-window-ansi-l1-1-0.dll",
    "ext-ms-win-rtcore-ntuser-window-l1-1-0.dll", "PIXEvt.dll", "ucrtbased.dll",
    "VCRUNTIME140_1D.dll", "VCRUNTIME140D.dll",
]


def module_profile(modules, delayed=()):
    """PE32+ with just an import table. Entries are (dll, [name or ordinal, ...])."""
    blob = bytearray()

    def reserve(size):
        blob.extend(bytes(-len(blob) % 8 + size))
        return 0x1000 + len(blob) - size

    def place(rva, layout, *values):
        struct.pack_into("<" + layout, blob, rva - 0x1000, *values)

    def text(value):
        rva = reserve(len(value) + 1)
        place(rva, f"{len(value)}s", value.encode())
        return rva

    def thunks(symbols):
        values = []
        for symbol in symbols:
            if isinstance(symbol, int):
                values.append((1 << 63) | symbol)
            else:
                values.append(reserve(2 + len(symbol) + 1))
                place(values[-1] + 2, f"{len(symbol)}s", symbol.encode())
        pair = []
        for _ in range(2):
            pair.append(reserve(8 * (len(values) + 1)))
            place(pair[-1], f"{len(values)}Q", *values)
        return pair

    imports = reserve(20 * (len(modules) + 1))
    delays = reserve(32 * (len(delayed) + 1))
    for i, (name, symbols) in enumerate(modules):
        lookup, iat = thunks(symbols)
        place(imports + 20 * i, "5I", lookup, 0, 0, text(name), iat)
    for i, (name, symbols) in enumerate(delayed):
        lookup, iat = thunks(symbols)
        place(delays + 32 * i, "8I", 1, text(name), 0, iat, lookup, 0, 0, 0)
    blob.extend(bytes(-len(blob) % 512))

    data = bytearray(512)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 60, 128)
    data[128:132] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 132, 0x8664, 1, 0, 0, 0, 240, 0x22)
    opt = 152
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<QII", data, opt + 24, 0x140000000, 4096, 512)
    struct.pack_into("<II", data, opt + 56, 0x1000 + (len(blob) + 4095) // 4096 * 4096, 512)
    struct.pack_into("<HH", data, opt + 48, 6, 0)
    struct.pack_into("<H", data, opt + 68, 2)
    struct.pack_into("<I", data, opt + 108, 16)
    struct.pack_into("<8sIIIIIIHHI", data, opt + 240, b".rdata", len(blob), 4096,
                     len(blob), 512, 0, 0, 0, 0, 0x40000040)
    struct.pack_into("<II", data, opt + 112 + 8, imports, 20 * (len(modules) + 1))
    if delayed:
        struct.pack_into("<II", data, opt + 112 + 8 * 13, delays, 32 * (len(delayed) + 1))
    return data + blob


def library():
    """The directories fixture marked as a DLL: ordinal 5 is exported as Alias and Value."""
    data = fixture()
    struct.pack_into("<H", data, 132 + 18, 0x2022)
    return data


VARIANTS = {
    "library": library,
    # imports from library.dll: two names it exports, one it does not, one ordinal
    "library-user": lambda: module_profile([("Library.dll", ["Alias", "Value", "Absent", 5])]),
    "directories": fixture,
    "scarlett-modules": lambda: module_profile([(m, []) for m in PUBLISHED_SCARLETT_MODULES]),
    # made up: the Xbox One Direct3D module next to an ordinary system DLL
    "durango-modules": lambda: module_profile(
        [("KERNEL32.dll", ["VirtualAlloc"]), ("d3d12_x.dll", ["D3D12XboxCreateDevice"])]),
    # made up: modules of an ERA title, one import each way the Xbox One runtime handles them
    "era-modules": lambda: module_profile(
        [("kernelx.dll", ["VirtualAlloc", "XMemAllocDefault"]), ("EtwPlus.dll", ["EtxRegister"]),
         ("WS2_32.dll", [1]), ("d3d11_x.dll", ["D3D11CreateDevice", "D3D11XCreateDeviceX"])]),
    # made up imports to test delay-load parsing and the API list
    "delay": lambda: module_profile(
        [("KERNEL32.dll", ["VirtualAlloc", "WinExec"])],
        [("dxcompiler_xs.dll", [1]), ("title-local.dll", ["Probe"])]),
}

if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--all":
        directory = Path(sys.argv[2])
        for name, make in VARIANTS.items():
            path = directory / ("synthetic.pe" if name == "directories" else name + ".pe")
            path.write_bytes(make())
    else:
        Path(sys.argv[1]).write_bytes(
            VARIANTS[sys.argv[2] if len(sys.argv) > 2 else "directories"]())
