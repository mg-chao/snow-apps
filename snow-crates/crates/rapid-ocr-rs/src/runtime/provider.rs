#[cfg(feature = "cann-provider")]
use ort::ep::CANN;
#[cfg(feature = "cuda-provider")]
use ort::ep::CUDA;
#[cfg(feature = "directml-provider")]
use ort::ep::DirectML;
use ort::ep::{CPU, ExecutionProvider, ExecutionProviderDispatch};

use crate::{
    config::ProviderPreference,
    error::{RapidOcrError, Result},
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResolvedExecutionProvider {
    Cpu,
    Cuda,
    DirectMl,
    Cann,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ProviderResolution {
    pub requested: ProviderPreference,
    pub resolved: ResolvedExecutionProvider,
    pub fallback_used: bool,
}

#[derive(Debug)]
pub struct ProviderChain {
    pub providers: Vec<ExecutionProviderDispatch>,
    pub resolution: ProviderResolution,
}

pub fn resolve_execution_providers(
    preference: &ProviderPreference,
    enable_cpu_mem_arena: bool,
    fail_if_provider_unavailable: bool,
) -> Result<ProviderChain> {
    let cpu_provider = || {
        CPU::default()
            .with_arena_allocator(enable_cpu_mem_arena)
            .build()
    };

    match preference {
        ProviderPreference::Cpu => Ok(ProviderChain {
            providers: vec![cpu_provider()],
            resolution: ProviderResolution {
                requested: ProviderPreference::Cpu,
                resolved: ResolvedExecutionProvider::Cpu,
                fallback_used: false,
            },
        }),
        ProviderPreference::Auto { device_id } => resolve_directml_execution_providers(
            ProviderPreference::Auto {
                device_id: *device_id,
            },
            *device_id,
            cpu_provider(),
            false,
        ),
        ProviderPreference::Cuda { device_id } => resolve_cuda_execution_providers(
            *device_id,
            cpu_provider(),
            fail_if_provider_unavailable,
        ),
        ProviderPreference::DirectMl { device_id } => resolve_directml_execution_providers(
            ProviderPreference::DirectMl {
                device_id: *device_id,
            },
            *device_id,
            cpu_provider(),
            fail_if_provider_unavailable,
        ),
        ProviderPreference::Cann { device_id } => resolve_cann_execution_providers(
            *device_id,
            cpu_provider(),
            fail_if_provider_unavailable,
        ),
    }
}

fn device_id_to_i32(provider_name: &str, device_id: usize) -> Result<i32> {
    i32::try_from(device_id).map_err(|_| {
        RapidOcrError::Config(format!(
            "invalid {provider_name} device_id {device_id}: value exceeds i32 range"
        ))
    })
}

fn resolve_cuda_execution_providers(
    device_id: usize,
    cpu_provider: ExecutionProviderDispatch,
    fail_if_provider_unavailable: bool,
) -> Result<ProviderChain> {
    resolve_accelerator_execution_providers(
        "CUDA",
        ProviderPreference::Cuda { device_id },
        ResolvedExecutionProvider::Cuda,
        device_id,
        cpu_provider,
        fail_if_provider_unavailable,
        |id| {
            #[cfg(feature = "cuda-provider")]
            {
                let provider = CUDA::default().with_device_id(id);
                Ok((provider.is_available()?, provider.build()))
            }
            #[cfg(not(feature = "cuda-provider"))]
            {
                let _ = id;
                Ok((false, CPU::default().build()))
            }
        },
    )
}

fn resolve_directml_execution_providers(
    requested: ProviderPreference,
    device_id: usize,
    cpu_provider: ExecutionProviderDispatch,
    fail_if_provider_unavailable: bool,
) -> Result<ProviderChain> {
    resolve_accelerator_execution_providers(
        "DirectML",
        requested,
        ResolvedExecutionProvider::DirectMl,
        device_id,
        cpu_provider,
        fail_if_provider_unavailable,
        |id| {
            #[cfg(feature = "directml-provider")]
            {
                let provider = DirectML::default().with_device_id(id);
                Ok((provider.is_available()?, provider.build()))
            }
            #[cfg(not(feature = "directml-provider"))]
            {
                let _ = id;
                Ok((false, CPU::default().build()))
            }
        },
    )
}

fn resolve_cann_execution_providers(
    device_id: usize,
    cpu_provider: ExecutionProviderDispatch,
    fail_if_provider_unavailable: bool,
) -> Result<ProviderChain> {
    resolve_accelerator_execution_providers(
        "CANN",
        ProviderPreference::Cann { device_id },
        ResolvedExecutionProvider::Cann,
        device_id,
        cpu_provider,
        fail_if_provider_unavailable,
        |id| {
            #[cfg(feature = "cann-provider")]
            {
                let provider = CANN::default().with_device_id(id);
                Ok((provider.is_available()?, provider.build()))
            }
            #[cfg(not(feature = "cann-provider"))]
            {
                let _ = id;
                Ok((false, CPU::default().build()))
            }
        },
    )
}

fn resolve_accelerator_execution_providers<F>(
    provider_name: &str,
    requested: ProviderPreference,
    preferred: ResolvedExecutionProvider,
    device_id: usize,
    cpu_provider: ExecutionProviderDispatch,
    fail_if_provider_unavailable: bool,
    prepare_provider: F,
) -> Result<ProviderChain>
where
    F: FnOnce(i32) -> Result<(bool, ExecutionProviderDispatch)>,
{
    let device_id_i32 = device_id_to_i32(provider_name, device_id)?;
    let (is_available, provider_dispatch) = prepare_provider(device_id_i32)?;
    let resolution = decide_provider_resolution(
        requested,
        preferred,
        is_available,
        fail_if_provider_unavailable,
    )?;

    if resolution.fallback_used {
        Ok(ProviderChain {
            providers: vec![cpu_provider],
            resolution,
        })
    } else {
        let provider_dispatch = if fail_if_provider_unavailable {
            provider_dispatch.error_on_failure()
        } else {
            provider_dispatch
        };
        Ok(ProviderChain {
            providers: vec![provider_dispatch, cpu_provider],
            resolution,
        })
    }
}

fn decide_provider_resolution(
    requested: ProviderPreference,
    preferred: ResolvedExecutionProvider,
    preferred_is_available: bool,
    fail_if_provider_unavailable: bool,
) -> Result<ProviderResolution> {
    if preferred_is_available {
        return Ok(ProviderResolution {
            requested,
            resolved: preferred,
            fallback_used: false,
        });
    }

    if fail_if_provider_unavailable {
        return Err(RapidOcrError::Config(format!(
            "requested execution provider {} is unavailable and fail_if_provider_unavailable=true",
            format_provider_preference(requested)
        )));
    }

    Ok(ProviderResolution {
        requested,
        resolved: ResolvedExecutionProvider::Cpu,
        fallback_used: true,
    })
}

fn format_provider_preference(preference: ProviderPreference) -> String {
    match preference {
        ProviderPreference::Cpu => "cpu".to_string(),
        ProviderPreference::Auto { device_id } => format!("auto(device_id={device_id})"),
        ProviderPreference::Cuda { device_id } => format!("cuda(device_id={device_id})"),
        ProviderPreference::DirectMl { device_id } => {
            format!("directml(device_id={device_id})")
        }
        ProviderPreference::Cann { device_id } => format!("cann(device_id={device_id})"),
    }
}

#[cfg(test)]
mod tests {
    use super::{ResolvedExecutionProvider, decide_provider_resolution};
    use crate::config::ProviderPreference;

    fn assert_cpu_fallback(requested: ProviderPreference, preferred: ResolvedExecutionProvider) {
        let resolution = decide_provider_resolution(requested, preferred, false, false)
            .expect("provider fallback should succeed");
        assert_eq!(resolution.requested, requested);
        assert_eq!(resolution.resolved, ResolvedExecutionProvider::Cpu);
        assert!(resolution.fallback_used);
    }

    #[test]
    fn directml_preference_has_cpu_fallback() {
        assert_cpu_fallback(
            ProviderPreference::DirectMl { device_id: 0 },
            ResolvedExecutionProvider::DirectMl,
        );
    }

    #[test]
    fn auto_preference_has_cpu_fallback() {
        assert_cpu_fallback(
            ProviderPreference::Auto { device_id: 0 },
            ResolvedExecutionProvider::DirectMl,
        );
    }

    #[test]
    fn cuda_preference_has_cpu_fallback() {
        assert_cpu_fallback(
            ProviderPreference::Cuda { device_id: 0 },
            ResolvedExecutionProvider::Cuda,
        );
    }

    #[test]
    fn cann_preference_has_cpu_fallback() {
        assert_cpu_fallback(
            ProviderPreference::Cann { device_id: 0 },
            ResolvedExecutionProvider::Cann,
        );
    }

    #[test]
    fn strict_mode_errors_when_provider_is_unavailable() {
        let err = decide_provider_resolution(
            ProviderPreference::Cuda { device_id: 0 },
            ResolvedExecutionProvider::Cuda,
            false,
            true,
        )
        .expect_err("strict mode should reject unavailable provider");
        assert!(
            err.to_string()
                .contains("fail_if_provider_unavailable=true")
        );
    }

    #[test]
    fn non_strict_mode_falls_back_to_cpu_when_provider_is_unavailable() {
        let resolution = decide_provider_resolution(
            ProviderPreference::DirectMl { device_id: 2 },
            ResolvedExecutionProvider::DirectMl,
            false,
            false,
        )
        .expect("fallback should succeed");
        assert!(resolution.fallback_used);
        assert_eq!(resolution.resolved, ResolvedExecutionProvider::Cpu);
    }
}
