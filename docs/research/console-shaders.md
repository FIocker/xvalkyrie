# Shaders that only carry console GPU code

Some retail titles ship shaders the host cannot use: the container is a normal `DXBC` one,
but its `DXIL` part has no bitcode. The host answers `CreateGraphicsPipelineState` with
`0x8007000e` and the title stops. Seen in Series titles and in an Xbox
One GDK title (see `gdk-titles.md`). A title whose shaders keep their bitcode
runs as it is.

What follows was read from one vertex shader of the Series title, dumped with
`XS_SHADER_DUMP=<file>` (the first shader of the first pipeline the host turns down). It is
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
