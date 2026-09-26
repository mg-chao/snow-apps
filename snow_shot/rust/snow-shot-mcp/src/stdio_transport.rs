//! Keep SDK framing/negotiation while bounding untrusted lines and slow-reader retention.
use rmcp::{
    RoleServer,
    service::{RxJsonRpcMessage, TxJsonRpcMessage},
    transport::Transport,
};
use std::{
    io,
    pin::Pin,
    sync::Arc,
    task::{Context, Poll},
    time::Duration,
};
use tokio::{
    io::{AsyncRead, ReadBuf},
    sync::Semaphore,
};
use tokio_util::sync::CancellationToken;

// Larger than the application's 1 MiB + 4096-byte request envelope, leaving
// space for JSON-RPC and modern metadata without shrinking any legacy input.
const MAX_INPUT_LINE: usize = 1024 * 1024 + 65536;
// A 64 MiB IPC frame may expand to base64 plus duplicated structured/text JSON.
const MAX_QUEUED_OUTPUT: usize = 128 * 1024 * 1024;

pub struct BoundedInput<R> {
    inner: R,
    line_bytes: usize,
    limit: usize,
    failed: bool,
}
impl<R> BoundedInput<R> {
    pub fn new(inner: R) -> Self {
        Self {
            inner,
            line_bytes: 0,
            limit: MAX_INPUT_LINE,
            failed: false,
        }
    }
}
impl<R: AsyncRead + Unpin> AsyncRead for BoundedInput<R> {
    fn poll_read(
        self: Pin<&mut Self>,
        cx: &mut Context<'_>,
        buffer: &mut ReadBuf<'_>,
    ) -> Poll<io::Result<()>> {
        let this = self.get_mut();
        if this.failed {
            return Poll::Ready(Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "MCP input line exceeds its byte limit",
            )));
        }
        let before = buffer.filled().len();
        match Pin::new(&mut this.inner).poll_read(cx, buffer) {
            Poll::Ready(Ok(())) => {
                for byte in &buffer.filled()[before..] {
                    if *byte == b'\n' {
                        this.line_bytes = 0;
                    } else {
                        this.line_bytes += 1;
                    }
                    if this.line_bytes > this.limit {
                        this.failed = true;
                        buffer.set_filled(before);
                        tracing::warn!("MCP input line exceeds its byte limit");
                        return Poll::Ready(Err(io::Error::new(
                            io::ErrorKind::InvalidData,
                            "MCP input line exceeds its byte limit",
                        )));
                    }
                }
                Poll::Ready(Ok(()))
            }
            other => other,
        }
    }
}

struct ByteCount(usize);
impl io::Write for ByteCount {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.0 = self.0.saturating_add(bytes.len());
        if self.0 > MAX_QUEUED_OUTPUT {
            return Err(io::Error::new(
                io::ErrorKind::OutOfMemory,
                "MCP output byte budget exceeded",
            ));
        }
        Ok(bytes.len())
    }
    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

pub struct BoundedTransport<T> {
    inner: T,
    bytes: Arc<Semaphore>,
    messages: Arc<Semaphore>,
    stopped: CancellationToken,
    write_timeout: Duration,
}
impl<T> BoundedTransport<T> {
    pub fn new(inner: T) -> Self {
        Self {
            inner,
            bytes: Arc::new(Semaphore::new(MAX_QUEUED_OUTPUT)),
            messages: Arc::new(Semaphore::new(16)),
            stopped: CancellationToken::new(),
            write_timeout: Duration::from_secs(10),
        }
    }
}
impl<T: Transport<RoleServer, Error = io::Error>> Transport<RoleServer> for BoundedTransport<T> {
    type Error = io::Error;
    fn send(
        &mut self,
        item: TxJsonRpcMessage<RoleServer>,
    ) -> impl Future<Output = io::Result<()>> + Send + 'static {
        let admission = (|| {
            let count = self
                .messages
                .clone()
                .try_acquire_owned()
                .map_err(io::Error::other)?;
            let mut size = ByteCount(1);
            serde_json::to_writer(&mut size, &item).map_err(io::Error::other)?;
            let bytes = self
                .bytes
                .clone()
                .try_acquire_many_owned(size.0 as u32)
                .map_err(io::Error::other)?;
            Ok::<_, io::Error>((count, bytes))
        })();
        let sending = admission.as_ref().ok().map(|_| self.inner.send(item));
        let stopped = self.stopped.clone();
        let deadline = self.write_timeout;
        async move {
            let result = async {
                let _permits = admission?;
                let sending = sending.ok_or_else(|| io::Error::other("MCP output unavailable"))?;
                tokio::select! {
                    _ = stopped.cancelled() => Err(io::Error::new(io::ErrorKind::BrokenPipe, "MCP transport closed")),
                    result = tokio::time::timeout(deadline, sending) => result.map_err(|_| io::Error::new(io::ErrorKind::TimedOut, "MCP output stalled"))?,
                }
            }.await;
            if result.is_err() {
                stopped.cancel();
            }
            result
        }
    }
    async fn receive(&mut self) -> Option<RxJsonRpcMessage<RoleServer>> {
        tokio::select! {
            _ = self.stopped.cancelled() => None,
            message = self.inner.receive() => message,
        }
    }
    async fn close(&mut self) -> io::Result<()> {
        self.stopped.cancel();
        self.inner.close().await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::io::AsyncReadExt;
    #[tokio::test]
    async fn input_limit_is_per_line_and_survives_partial_reads() {
        let mut input = BoundedInput::new(std::io::Cursor::new(b"1234\n1234\n"));
        input.limit = 4;
        let mut output = vec![];
        input.read_to_end(&mut output).await.unwrap();
        assert_eq!(output, b"1234\n1234\n");
        let mut input = BoundedInput::new(std::io::Cursor::new(b"12345"));
        input.limit = 4;
        let mut bytes = [0; 2];
        input.read_exact(&mut bytes).await.unwrap();
        input.read_exact(&mut bytes).await.unwrap();
        assert_eq!(
            input.read(&mut bytes).await.unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
    }
    struct Stalled;
    impl Transport<RoleServer> for Stalled {
        type Error = io::Error;
        fn send(
            &mut self,
            _: TxJsonRpcMessage<RoleServer>,
        ) -> impl Future<Output = io::Result<()>> + Send + 'static {
            std::future::pending()
        }
        async fn receive(&mut self) -> Option<RxJsonRpcMessage<RoleServer>> {
            std::future::pending().await
        }
        async fn close(&mut self) -> io::Result<()> {
            Ok(())
        }
    }
    fn message() -> TxJsonRpcMessage<RoleServer> {
        serde_json::from_value(serde_json::json!({"jsonrpc":"2.0","id":1,"result":{"content":[]}}))
            .unwrap()
    }
    #[tokio::test]
    async fn slow_reader_timeout_and_admission_failure_close_receive_without_replay() {
        let mut transport = BoundedTransport::new(Stalled);
        transport.write_timeout = Duration::from_millis(5);
        assert_eq!(
            transport.send(message()).await.unwrap_err().kind(),
            io::ErrorKind::TimedOut
        );
        assert!(transport.receive().await.is_none());
        let mut transport = BoundedTransport::new(Stalled);
        transport.bytes = Arc::new(Semaphore::new(1));
        assert!(transport.send(message()).await.is_err());
        assert!(transport.receive().await.is_none());
        assert_eq!(transport.messages.available_permits(), 16);
    }
}
