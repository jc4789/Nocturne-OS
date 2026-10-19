# Rust programs (ports/rust/README.md), built when the standard library for
# x86_64-unknown-nocturne exists in tools/rust/sysroot (ports/rust/build-std.sh).
# Every ports/rust/apps/<name>/main.rs becomes /bin/<name>. Without the sysroot
# nothing here is built, so a normal `make` is unchanged.
#
# Needs BUILD, LIBC_RSP and the crt0 object of the libc (Makefile), so the
# Makefile includes this file after the libc link rules and adds $(RUST_FILES)
# to the initrd prerequisites.
RUST_SYSROOT ?= tools/rust/sysroot
RUST_LICENSES ?= tools/rust/licenses
ifneq ($(wildcard $(RUST_SYSROOT)/lib/rustlib/x86_64-unknown-nocturne/lib),)
RUST_APPS  := $(notdir $(wildcard ports/rust/apps/*))
RUST_BINS  := $(addprefix $(BUILD)/root/bin/,$(RUST_APPS))
RUST_FILES := $(RUST_BINS) $(addprefix $(BUILD)/root/usr/share/licenses/rust/,COPYRIGHT LICENSE-APACHE LICENSE-MIT)
$(RUST_BINS): $(BUILD)/root/bin/%: ports/rust/apps/%/main.rs $(LIBC_RSP) $(BUILD)/u/user/libc/crt0.asm.o user/user.ld ports/rust/rustc-nocturne.sh ports/rust/x86_64-unknown-nocturne.json $(wildcard $(RUST_SYSROOT)/lib/rustlib/x86_64-unknown-nocturne/lib/*.rlib)
	@mkdir -p $(dir $@)
	@echo "  RUST $@"
	@RUST_SYSROOT="$(RUST_SYSROOT)" BUILD="$(BUILD)" sh ports/rust/rustc-nocturne.sh $< $@
# The standard library is MIT or Apache-2.0: the licence texts ship with the programs.
$(BUILD)/root/usr/share/licenses/rust/%: $(RUST_LICENSES)/%
	@mkdir -p $(dir $@)
	@cp $< $@
endif
