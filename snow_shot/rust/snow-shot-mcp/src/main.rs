mod app_client;
mod schemas;
mod server;
mod wire;

use anyhow::Result;
use rmcp::{ServiceExt, transport::stdio};
use server::SnowShotMcp;

#[tokio::main(flavor = "multi_thread", worker_threads = 2)]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| tracing_subscriber::EnvFilter::new("warn")),
        )
        .with_writer(std::io::stderr)
        .with_ansi(false)
        .init();

    let service = SnowShotMcp::new().serve(stdio()).await?;
    service.waiting().await?;
    Ok(())
}
