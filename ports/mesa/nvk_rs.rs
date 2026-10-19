// The Rust half of NVK (ports/mesa/nvk.mk): NAK, nil and nvk_rs export the C functions that
// NVK's C code calls; linking them through one static library gives them one copy of std.

extern crate nak;
extern crate nil;
extern crate nvk_rs;
