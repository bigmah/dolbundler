//! Debug aid: lists every advertising BLE peripheral, name or no name.
//!
//! The driver's own discovery filters on the local name (`NAME_HINTS` in
//! `transport/ble.rs`). This does not filter at all, so it distinguishes "the
//! controller never advertised" from "it advertised under a name we reject".

use std::collections::HashSet;
use std::time::Duration;

use btleplug::api::{Central, Manager as _, Peripheral as _, ScanFilter};
use btleplug::platform::Manager;

#[tokio::main(flavor = "current_thread")]
async fn main() -> anyhow::Result<()> {
    let secs: u64 = std::env::args()
        .nth(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(30);

    let manager = Manager::new().await?;
    let central = manager
        .adapters()
        .await?
        .into_iter()
        .next()
        .ok_or_else(|| anyhow::anyhow!("no Bluetooth adapter"))?;

    central.start_scan(ScanFilter::default()).await?;
    println!("scanning {secs}s — hold sync until the player LEDs chase\n");

    let deadline = tokio::time::Instant::now() + Duration::from_secs(secs);
    let mut seen: HashSet<String> = HashSet::new();

    while tokio::time::Instant::now() < deadline {
        for p in central.peripherals().await? {
            let Ok(Some(props)) = p.properties().await else {
                continue;
            };
            let id = p.id().to_string();
            if !seen.insert(id.clone()) {
                continue;
            }
            let name = props.local_name.unwrap_or_else(|| "<no name>".into());
            let rssi = props
                .rssi
                .map_or_else(|| "?".to_string(), |r| format!("{r} dBm"));
            println!("{name}  [{rssi}]");
            println!("  id: {id}");
            if let Some(mfr) = props.manufacturer_data.keys().next() {
                // 0x0553 is Nintendo's Bluetooth company identifier.
                println!("  manufacturer id: {mfr:#06x}");
            }
            for s in &props.services {
                println!("  service: {s}");
            }
        }
        tokio::time::sleep(Duration::from_millis(250)).await;
    }

    let _ = central.stop_scan().await;
    println!("\n{} peripheral(s) seen.", seen.len());
    if seen.is_empty() {
        println!("None at all — check that the terminal has Bluetooth permission in");
        println!("System Settings → Privacy & Security → Bluetooth.");
    }
    Ok(())
}
