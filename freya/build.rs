const LIBC_SOURCE: &str = "../kernel/common/libc.cpp";
const HEAP_SOURCE: &str = "heap.cpp";
const HEL_INCLUDE: &str = "../hel/include";

/// Returns the `-I` directories that the given pkg-config module needs.
///
/// The pkg-config crate is not used here because it wants to be told explicitly
/// that cross-compilation is allowed; the recipe already points PKG_CONFIG at the
/// right target-aware wrapper, so asking it directly is both simpler and more
/// faithful to how the rest of the build finds its headers.
fn include_dirs(module: &str) -> Vec<String> {
    let pkg_config = std::env::var("PKG_CONFIG").unwrap_or_else(|_| "pkg-config".into());
    let output = std::process::Command::new(&pkg_config)
        .args(["--cflags-only-I", module])
        .output()
        .unwrap_or_else(|e| panic!("failed to run {pkg_config}: {e}"));
    assert!(
        output.status.success(),
        "{pkg_config} --cflags-only-I {module} failed: {}",
        String::from_utf8_lossy(&output.stderr),
    );

    String::from_utf8(output.stdout)
        .expect("pkg-config emitted invalid UTF-8")
        .split_whitespace()
        .map(|flag| flag.trim_start_matches("-I").to_owned())
        .collect()
}

fn main() {
    // `compiler_builtins` only ships memcpy and friends for targets that have no
    // libc of their own; on the managarm targets they come from mlibc, which
    // freya does not link. thor and eir have the same problem and already solve
    // it with a freestanding implementation, so freya builds the same file.
    //
    // -ffreestanding and -fno-tree-loop-distribute-patterns stop the optimiser
    // rewriting the bodies below into calls to themselves; the userspace
    // compiler, unlike the kernel one, knows these symbols as libc builtins.
    // Exceptions and RTTI are off because there is no C++ runtime to support
    // them.
    cc::Build::new()
        .cpp(true)
        // The file is C++ only in syntax and uses nothing out of the standard
        // library, so suppress the -lstdc++ that cc would otherwise add: only a
        // shared libstdc++ exists, and freya links statically.
        .cpp_link_stdlib(None)
        .file(LIBC_SOURCE)
        .flag("-ffreestanding")
        .flag("-fno-tree-loop-distribute-patterns")
        .flag("-fno-exceptions")
        .flag("-fno-rtti")
        .compile("freestanding-libc");

    // The heap itself: frigg's slab pool over helAllocateMemory. Built separately
    // from the file above because it needs frigg's headers and none of the
    // freestanding flags.
    let mut heap = cc::Build::new();
    heap.cpp(true)
        .cpp_link_stdlib(None)
        .file(HEAP_SOURCE)
        .include(HEL_INCLUDE)
        // frigg uses concepts and other recent language features; this is the
        // same standard the rest of managarm sets in its top-level meson.build.
        .std("gnu++26")
        .flag("-fno-exceptions")
        .flag("-fno-rtti");
    for dir in include_dirs("frigg") {
        heap.include(dir);
    }
    heap.compile("freya-heap");

    // freya provides superAnonAllocate and superGetProcessData to the servers it
    // launches, so it cannot itself depend on mlibc: mlibc's managarm sysdeps issue
    // superGetProcessData during startup, which would deadlock against the very
    // service freya is trying to bring up. Link with neither the C runtime startup
    // files nor libc.
    //
    // Dropping libc also drops PT_INTERP, which is what makes the image loadable by
    // thor's minimal static loader (no rtdl, no relocation processing).
    //
    // These are link args rather than RUSTFLAGS because they must not reach the
    // other workspace members, which are ordinary mlibc-linked binaries.
    println!("cargo::rustc-link-arg-bins=-nostdlib");
    println!("cargo::rustc-link-arg-bins=-static");
    println!("cargo::rustc-link-arg-bins=-no-pie");

    println!("cargo::rerun-if-changed={LIBC_SOURCE}");
    println!("cargo::rerun-if-changed={HEAP_SOURCE}");
    println!("cargo::rerun-if-changed=build.rs");
}
