# XVD / XVC packages

Games are distributed as XVC packages, a variant of the XVD container. There
is no official spec. What `xscarlet inspect-package` reads comes from
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

## Limits

- Only the first 4 KiB are read. No signature or hash check, no decryption,
  no mounting.
- Package data is encrypted and the keys are not in the file. XScarlet has no
  keys and will not ship any.
- The layout has only been tested against a header built by hand in
  `tests/xvd.cpp`. It has not been checked against a real Series package.
- Newer package variants are not recognized.
