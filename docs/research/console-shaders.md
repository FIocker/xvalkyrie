# Shaders that only carry console GPU code

Some retail titles ship shaders the host cannot use: the container is a normal `DXBC` one,
but its `DXIL` part has no bitcode. The host answers `CreateGraphicsPipelineState` with
`0x8007000e` and the title stops. Seen in Series titles and in an Xbox
One GDK title (see `gdk-titles.md`). A title whose shaders keep their bitcode
runs as it is.

What follows was read from one vertex shader of the Series title, dumped with
`XV_SHADER_DUMP=<file>` (the first shader of the first pipeline the host turns down). It is
one sample: nothing here was checked against a second shader, a pixel shader, or the Xbox
One form.

## The container

Parts, by name and size: `SFI0` 8, `ISG1` 176, `OSG1` 260, `PSV0` 456, `RTS0` 3428,
`STAT` 1868, `HASH` 20, `DXIL` 8496.

- `ISG1`, `OSG1`, `PSV0` and `RTS0` are the usual ones: the input and output signatures,
  the pipeline validation data, and the root signature. They describe the shader as
  Direct3D sees it.
- `STAT` is a DXIL program whose bitcode (1844 bytes) starts with the bitcode magic. By its
  size it holds declarations, not the shader's body. Not disassembled.
- `DXIL` has the program header, then says its bitcode is 4 bytes at offset 8484, and those
  bytes are `NOBC`. Between the header and that there is an `XDAT` block.

## XDAT

At offset 24 of the `DXIL` part. Version 3, then a table of sections as (type, offset,
size), offsets from the start of `XDAT`:

| Type | Offset | Size | What it is |
|---|---|---|---|
| 0x0e | 0x48 | 0x1dc8 | Low entropy, mostly zero: records of 56 bytes with a running index (0x21c, 0x21d, ...), and the text `XBCH`. Not understood. |
| 0x0d | 0x1e10 | 0x302 | A zlib stream (`78 9c`). |
| 0x07 | 0x2114 | 0x1c | Holds `NOBC`: the placeholder the program header points at. |

## The code

The zlib stream inflates to 3900 bytes that start with `FSSM`, version bytes `01 01`, and
a size (0xf2c). Unused fields are filled with `deadbeef`. It names the entry point (`main`,
near the end) and has a table of small records before that.

At 0x2f8 (the header has that offset at 0xc0) there are 39 words of machine code for the
console's GPU, AMD's RDNA instruction set, ending in `s_endpgm` (`bf810000`) at 0x390 and
padded with `s_code_end` (`bf9f0000`). Recognisable in it: scalar loads (`f4080001`,
`f4280000`), `s_waitcnt` (`bf8cc07f`, `bf8c0070`), vector moves (`7e000280`), and six
exports (`f80008cf`, `f800020f` ... `f800024f`), which fits a vertex shader that writes a
position and five parameters.

So the shader's real body is there, as machine code, and the Direct3D-level description of
its inputs, outputs and bindings is in the other parts.

## What running such a title needs

A translator from that machine code to something the host compiles: decode the
instructions, find how the shader gets at its inputs (which registers hold the vertex
attributes, the descriptor tables and the constants at entry), and emit a host shader with
the same signature. None of it exists here. Open questions, in the order they block:

1. The entry state: which registers carry what. The `FSSM` records and the `0x0e` section
   are the likely place; neither is decoded.
2. How resources are reached: the code loads descriptors through pointers, where a host
   shader names a register and space. `RTS0` has the root signature to map back to.
3. Coverage of the instruction set: control flow, the texture sampling forms, the packed
   conversions. One 39-word vertex shader says nothing about how large real ones are.
4. The Xbox One form (another GPU generation, another instruction set).

AMD's reference for the instruction set is public ("RDNA Shader ISA"), and LLVM's AMDGPU
target disassembles it (processor `gfx1013` is the closest to the Series GPU).

## What a translator has to know (read from two retail titles' shaders)

Checked against the disassembly LLVM gives for these shaders (`llvm-objdump -d
--triple=amdgcn --mcpu=gfx1013` on the code wrapped in an ELF file).

### The FSSM file

| Offset | What |
|---|---|
| `0x00` | `FSSM`, then version `0x101` |
| `0xC0` | u64: where the code region starts |
| `0xC8` | u64: its size, `s_code_end` padding included |
| code region `+0x00` | four dwords. A compute shader: `COMPUTE_PGM_RSRC2` (user registers in bits 5:1, thread group id enables in 7 to 9, components of the thread id in 12:11), then the thread group's size X, Y, Z |
| code region `+0x10` | 24 bytes that are not code (a hash) |
| code region `+0x28` | the first instruction |
| after the code | records of 20 bytes (five dwords), ended by one whose third dword is `0xFFFFFFFF`, then the entry point's name |

The records say where the root signature's parameters are and what the shader uses:

- `(3, 0, 1, 0, parameter << 8 | register)`: root parameter `parameter` (a descriptor
  table) is in scalar register `register`. Graphics shaders start at `s8`, compute shaders
  at `s4`.
- `(0x300 | kind, offset, register, extra, parameter << 16 | scalar << 8 | words)`: a
  resource the shader uses. `kind` 0 a shader resource, 1 a constant buffer, 2 a sampler,
  3 an unordered access view; `offset` its place in the table in dwords; `register` its HLSL
  register; `words` the size of its descriptor (8 a texture, 4 a buffer or a sampler). For a
  buffer `extra` looks like the stride of its elements; for a texture it is 4.

### Registers a shader starts with

- Vertex: `s_swappc_b64 s[4:5], s[4:5]` first, a call to a fetch shader that is not in the
  file. After it input element of register `r` is in `v[4 + 4r]` and the three after it.
  `v0` is the vertex id and `v3` the instance id.
- Pixel: `v0` and `v1` are the barycentrics, used only by `v_interp_p1/p2`; `attrN` is the
  input of register N... of the inputs that are interpolated (see the code). The position's
  components that the shader reads follow in `v2` and on.
- Compute: after the user registers, the thread group's id (X, Y, Z as enabled) in the
  next scalar registers; the thread's id within the group in `v0`, `v1`, `v2`.
- A table's pointer is 32 bits: shaders build the pair with `s_mov_b32 hi, 0`.

### Frame pacing an engine depends on

Unreal holds its game thread until the frames before were shown. It asks
`GetFrameStatisticsX` for each token `WaitFrameEventX` gave, and waits for the objects it
gave `ScheduleFrameEventX`. Answering neither stops the title after a few frames with no
error at all, and its own hang detector then ends it with a write to address 3.

## The translator (`console_shader_rdna.cpp`)

Each register is a variable, each instruction a statement on 32-bit patterns, and the shader
runs for one lane of a wave: a mask of lanes is all ones or zero, and what the hardware does
for the lanes of EXEC is done if this lane's is set. Branches forward set `next` and the
blocks they jump over are guarded by it; a branch back makes a `for` loop from its target to
itself. The instruction table (`console_shader_rdna_ops.inc`) was read off LLVM's
disassembly of the shaders of two titles, so it has the instructions those use and no others.

State on the first title it was run on (82 shaders up to its title screen):
46 are translated and all of those compile; the title screen is drawn right. Not done:
cube maps sampled by face, the `ds_*` instructions (memory shared by a thread group),
buffer stores and atomics, `s_getpc_b64` tables, geometry shaders. A shader with any of
these gets the stand-in that draws nothing, and `console_shader_source` says which.

What had to be right on the host for translated shaders to draw:

- Samplers the title writes as hardware descriptors (S#) are made on the host from their
  four words. A sampler slot left empty hangs the host's GPU as soon as it is sampled with.
- Constant buffers the title writes as hardware descriptors (V#) carry the address the
  title has for the buffer, which is not the host's.
- Textures are read out of the title's memory with the console's own `xg_xs.dll`
  (`XGCreateTextureComputer` from the description the title gave, then
  `CopyFromSubresource`), since they are stored swizzled.

Tools used, outside the tree: a corpus harness that translates and compiles every shader of
a folder dumped with `XV_SHADER_CODE` and counts what stopped the others, and
`XV_SHADER_STAND_INS=<stages>` / `XV_SHADER_SKIP=<text>` to find which shader breaks a run.
