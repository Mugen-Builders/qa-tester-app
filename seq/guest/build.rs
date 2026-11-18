// The engine symbols (application_engine_*, qatester_*) are resolved at the final link: by Alpine's
// g++ for the machine binary, or by the merged host archive when `QATESTER_LIB_DIR` names its
// directory (cargo test). The archive is never bundled into the guest staticlib.
fn main() {
    println!("cargo::rerun-if-env-changed=QATESTER_LIB_DIR");
    if let Ok(dir) = std::env::var("QATESTER_LIB_DIR") {
        // relink when the engine archive changes (cargo does not watch native libraries itself)
        println!("cargo::rerun-if-changed={dir}/libqatester-host.a");
        println!("cargo::rustc-link-search=native={dir}");
        println!("cargo::rustc-link-lib=static:+whole-archive=qatester-host");
        println!("cargo::rustc-link-lib=stdc++");
    }
}
