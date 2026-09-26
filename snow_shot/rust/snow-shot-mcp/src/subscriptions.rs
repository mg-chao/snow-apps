use super::{McpError, discovery};
use crate::app_client::{AppClient, AppEvent};
use rmcp::{
    RoleServer,
    model::*,
    service::{RequestContext, SubscriptionContext},
};
use std::{
    collections::{HashMap, HashSet},
    sync::Mutex,
    time::Duration,
};
use tokio::sync::{Semaphore, broadcast};
use tokio::task::AbortHandle;

pub(super) struct Subscriptions {
    legacy: Mutex<HashMap<String, AbortHandle>>,
    modern: Semaphore,
}
impl Default for Subscriptions {
    fn default() -> Self {
        Self {
            legacy: Mutex::new(HashMap::new()),
            modern: Semaphore::new(8),
        }
    }
}
impl Drop for Subscriptions {
    fn drop(&mut self) {
        if let Ok(subscriptions) = self.legacy.get_mut() {
            for (_, handle) in subscriptions.drain() {
                handle.abort();
            }
        }
    }
}
fn valid_uri(uri: &str) -> bool {
    discovery::resource_request(uri).is_ok()
}
pub(super) fn accepted(request: &SubscriptionFilter) -> SubscriptionFilter {
    let mut filter = SubscriptionFilter::default();
    filter.resource_subscriptions = Some(
        request
            .resource_subscriptions
            .as_deref()
            .unwrap_or_default()
            .iter()
            .filter(|uri| valid_uri(uri))
            .take(64)
            .cloned()
            .collect(),
    );
    filter
}
async fn batch(
    receiver: &mut broadcast::Receiver<AppEvent>,
    fallback: &[String],
) -> Option<HashSet<String>> {
    let mut uris = HashSet::new();
    match receiver.recv().await {
        Ok(event) => {
            if event.uri.is_empty() {
                uris.extend(fallback.iter().cloned());
            } else {
                uris.insert(event.uri);
            }
        }
        Err(broadcast::error::RecvError::Lagged(_)) => uris.extend(fallback.iter().cloned()),
        Err(broadcast::error::RecvError::Closed) => return None,
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
    // Bound draining so a continuously busy producer cannot starve delivery/cancellation.
    for _ in 0..128 {
        match receiver.try_recv() {
            Ok(event) => {
                if event.uri.is_empty() {
                    uris.extend(fallback.iter().cloned());
                } else {
                    uris.insert(event.uri);
                }
            }
            Err(broadcast::error::TryRecvError::Lagged(_)) => uris.extend(fallback.iter().cloned()),
            Err(_) => break,
        }
    }
    Some(uris)
}
impl Subscriptions {
    pub(super) async fn listen(
        &self,
        client: &AppClient,
        context: SubscriptionContext,
    ) -> Result<(), McpError> {
        let _permit = self
            .modern
            .try_acquire()
            .map_err(|_| McpError::invalid_params("Too many notification subscriptions", None))?;
        let mut receiver = client.subscribe_events();
        client
            .connect_events()
            .await
            .map_err(|e| McpError::internal_error(e.to_string(), None))?;
        let uris = context
            .accepted()
            .resource_subscriptions
            .as_deref()
            .unwrap_or_default();
        loop {
            tokio::select! {
                _=context.cancelled()=>return Ok(()),
                changed=batch(&mut receiver,uris)=>{
                    let Some(changed)=changed else{return Ok(())};
                    for uri in uris.iter().filter(|uri|changed.contains(*uri)) {
                        if context.sink().notify_resource_updated(uri.clone()).await.is_err(){return Ok(())}
                    }
                }
            }
        }
    }
    pub(super) async fn subscribe(
        &self,
        client: &AppClient,
        uri: String,
        context: RequestContext<RoleServer>,
    ) -> Result<(), McpError> {
        if !valid_uri(&uri) {
            return Err(McpError::invalid_params(
                "Invalid resource subscription URI",
                None,
            ));
        }
        let mut receiver = client.subscribe_events();
        client
            .connect_events()
            .await
            .map_err(|e| McpError::internal_error(e.to_string(), None))?;
        let mut registered = self
            .legacy
            .lock()
            .map_err(|_| McpError::internal_error("Subscription registry unavailable", None))?;
        if registered.contains_key(&uri) {
            return Ok(());
        }
        if registered.len() >= 64 {
            return Err(McpError::invalid_params(
                "Too many resource subscriptions",
                None,
            ));
        }
        let key = uri.clone();
        let task = tokio::spawn(async move {
            let accepted = vec![uri.clone()];
            while let Some(changed) = batch(&mut receiver, &accepted).await {
                if changed.contains(&uri)
                    && context
                        .peer
                        .notify_resource_updated(ResourceUpdatedNotificationParam::new(uri.clone()))
                        .await
                        .is_err()
                {
                    break;
                }
            }
        });
        registered.insert(key, task.abort_handle());
        Ok(())
    }
    pub(super) fn unsubscribe(&self, uri: &str) -> Result<(), McpError> {
        if let Some(handle) = self
            .legacy
            .lock()
            .map_err(|_| McpError::internal_error("Subscription registry unavailable", None))?
            .remove(uri)
        {
            handle.abort();
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn events_are_coalesced_and_overflow_invalidates_only_subscribed_resources() {
        let (sender, mut receiver) = broadcast::channel(2);
        for _ in 0..3 {
            let _ = sender.send(AppEvent {
                uri: "snow-shot://jobs/one".into(),
            });
        }
        let changed = batch(&mut receiver, &["snow-shot://jobs/two".into()])
            .await
            .unwrap();
        assert_eq!(changed.len(), 2);
        assert!(changed.contains("snow-shot://jobs/two"));
        let filter = accepted(
            &SubscriptionFilter::builder()
                .resource_subscriptions(["file:///secret", "snow-shot://jobs/one"])
                .build(),
        );
        assert_eq!(
            filter.resource_subscriptions.unwrap(),
            vec!["snow-shot://jobs/one"]
        );
    }
}
