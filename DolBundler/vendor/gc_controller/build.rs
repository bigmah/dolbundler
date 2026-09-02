//! Builds the Objective-C shim that asks macOS for a faster BLE connection
//! interval. See `src/transport/ble_latency.m` for why it has to be ObjC.

fn main() {
    println!("cargo:rerun-if-changed=src/transport/ble_latency.m");
    println!("cargo:rerun-if-changed=build.rs");

    if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
        return;
    }

    cc::Build::new()
        .file("src/transport/ble_latency.m")
        .flag("-fobjc-arc")
        .warnings(false)
        .compile("ble_latency");

    println!("cargo:rustc-link-lib=framework=Foundation");
    println!("cargo:rustc-link-lib=framework=CoreBluetooth");
}
