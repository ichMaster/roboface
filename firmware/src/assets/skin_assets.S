/* The drawn faces, linked in whole and read from memory-mapped flash.
 *
 * `.incbin` rather than a generated C array: 876 KB of hex text is five megabytes of source, and a
 * repository should not carry that to say the same thing. The symbol name follows the `ld` binary
 * convention so the generated header can name it without knowing this file exists.
 *
 * `.rodata` and 4-byte aligned: every entry in the blob is aligned to four by the converter, so a
 * `const uint16_t*` into it is never misaligned. The ESP32-S3 tolerates unaligned flash reads and
 * pays for them; nothing here is worth paying for.
 */
    .section .rodata.embedded, "a"
    .align 4
    .global _binary_src_assets_skin_assets_bin_start
    .global _binary_src_assets_skin_assets_bin_end
_binary_src_assets_skin_assets_bin_start:
    .incbin "src/assets/skin_assets.bin"
_binary_src_assets_skin_assets_bin_end:
    .byte 0
