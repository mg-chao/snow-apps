mod app_client;
mod schemas;
mod server;
mod stdio_transport;
mod wire;

use anyhow::Result;
use rmcp::{ServiceExt, transport::async_rw::AsyncRwTransport};
use server::SnowShotMcp;
use tracing_subscriber::prelude::*;

fn main() -> Result<()> {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()?;
    let result = runtime.block_on(run());
    // Tokio's stdin/stdout blocking helpers cannot cancel an OS pipe read/write.
    // A stalled peer must not prevent bounded transport shutdown indefinitely.
    runtime.shutdown_timeout(std::time::Duration::from_secs(1));
    result
}

async fn run() -> Result<()> {
    let mut launch_app = false;
    for argument in std::env::args().skip(1) {
        match argument.as_str() {
            "--launch-app" => launch_app = true,
            "--help" | "-h" => {
                eprintln!(
                    "snow-shot-mcp [--launch-app]\n  --launch-app  Start the adjacent Snow Shot app when unavailable; saved MCP enablement is respected."
                );
                return Ok(());
            }
            _ => anyhow::bail!("Unknown argument: {argument}"),
        }
    }
    tracing_subscriber::registry()
        .with(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("warn")),
        )
        .with(
            tracing_subscriber::fmt::layer()
                .with_writer(std::io::stderr)
                .with_ansi(false)
                // SDK events include request bodies and error.data even at WARN.
                // Only our payload-free diagnostics may reach stderr, regardless
                // of RUST_LOG, because invalid arguments can contain secrets too.
                .with_filter(tracing_subscriber::filter::filter_fn(|metadata| {
                    !metadata.target().starts_with("rmcp")
                })),
        )
        .init();

    let service = SnowShotMcp::new()
        .with_launch_app(launch_app)
        .serve(stdio_transport::BoundedTransport::new(
            AsyncRwTransport::new_server(
                stdio_transport::BoundedInput::new(tokio::io::stdin()),
                tokio::io::stdout(),
            ),
        ))
        // Initialize errors may retain and display the rejected request body.
        .await
        .map_err(|_| anyhow::anyhow!("MCP initialization failed"))?;
    service
        .waiting()
        .await
        .map_err(|_| anyhow::anyhow!("MCP service failed"))?;
    Ok(())
}
