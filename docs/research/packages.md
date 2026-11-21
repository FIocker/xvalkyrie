# XVD / XVC packages

Games are distributed as XVC packages, a variant of the XVD container. There
is no official spec. What `xvboot inspect-package` reads comes from
community notes that document Xbox One era files:


Only the layout was used, no code.

## Header

The file starts with a 0x200 byte signature, then the header up to 0x1000.

| Offset | Size | Field |
|---|---|---|
| 0x200 | 8 | Magic `msft-xvd` |
| 0x208 | 4 | Volume flags (bit 1 = encryption disabled, bit 2 = integrity disabled) |
| 0x20C | 4 | Format version |
| 0x218 | 8 | Drive size |
| 0x220 | 16 | VDUID (content ID) |
| 0x280 | 4 | Type (fixed / dynamic) |
| 0x284 | 4 | Content type |
| 0x288 | 4 | Embedded XVD length |
| 0x28C | 4 | User data length |
| 0x290 | 4 | XVC info length |
| 0x294 | 4 | Dynamic header length |
| 0x298 | 4 | Block size |
| 0x38C | 16 | Sandbox ID |
| 0x39C | 16 | Product ID |
| 0x3AC | 16 | PDUID |
| 0x3BC | 8 | Package version |
| 0x471 | 1 | Platform mask (PC / gen8 / gen9). |
| 0x494 | 8 | Required system version |

## resources.pri

A manifest may give a name as a reference, `DefaultDisplayName="ms-resource:AppName"`.
The value is in `resources.pri`, next to the manifest. `formats/pri` reads its strings;
the layout below is what three unpacked Series titles have, checked against what
`makepri dump` of the Windows SDK prints for the same files.

- Header: `mrm_pri2`, then at 12 the file size, at 16 the offset of the table of
  contents, at 20 where the sections start, at 24 how many there are (u16).
- Table of contents, 32 bytes a section: its kind in 16 characters (`[mrm_decn_info]`,
  `[mrm_hschemaex]`, `[mrm_res_map2_]`, `[mrm_dataitem]`), then at 24 its offset from
  where the sections start and at 28 its length. Each section has a 32-byte header of
  its own before its contents.
- `[mrm_decn_info]`: counts of distinct qualifiers, qualifiers, qualifier sets,
  decisions and index entries (u16 each, then a u16 length), then the tables in that
  order reversed: decisions (first index, count), qualifier sets (first index, count),
  qualifiers (distinct qualifier, priority, score, 0), distinct qualifiers (12 bytes:
  type at 2, where 0 is the language; offset of its value at 8, in UTF-16 units), the
  index table, the values. A decision is a list of qualifier sets; a resource with that
  decision has one candidate per set, in that order.
- `[mrm_hschemaex]`: the names. After the two names of the schema itself, 12 bytes per
  scope or item: parent (u16, an entry number), at 6 the length of its name, at 7
  flags (0x10 scope, 0x20 the name is ASCII, low 4 bits the top of the name offset), at
  8 the name offset, at 10 its index among scopes or items. Then 8 bytes per scope and
  2 per item, then the UTF-16 names and the ASCII names. `ms-resource:AppName` is the
  item `AppName` in the scope `resources`.
- `[mrm_res_map2_]`: at 4 the section of the names, at 8 that of the decisions, at 10
  how many value types, groups, item info groups (u16) and item infos (u32). Then the
  value types (8 bytes, the type in the second half: 0 UTF-16 string, 3 ASCII, 4 UTF-8,
  1/5/6 paths, 2 data), the groups (first item, info group), the info groups (size,
  first info), the infos (decision, first candidate) and the candidates, 8 bytes each:
  kind (1: in a data section), value type, 0, item of the data section, data section.
- `[mrm_dataitem]`: at 4 the number of strings and of blobs, then (offset, length) u16
  pairs for the strings and u32 pairs for the blobs, then the bytes.

Maps with the "large" tables (more items than 16 bits count) are skipped.

## Limits

- Only the first 4 KiB are read. No signature or hash check, no decryption,
  no mounting.
- Package data is encrypted and the keys are not in the file. XValkyrie has no
  keys and will not ship any.
- The layout has only been tested against a header built by hand in
  `tests/xvd.cpp`. It has not been checked against a real Series package.
- Newer package variants are not recognized.
