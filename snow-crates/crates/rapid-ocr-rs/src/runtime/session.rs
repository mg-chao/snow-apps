use std::path::Path;
use std::thread;

use ndarray::{ArrayView2, ArrayView3, ArrayView4, ArrayViewD, Ix2, Ix3, Ix4};
use ort::{
    inputs,
    session::{Session, builder::GraphOptimizationLevel},
    value::{TensorElementType, TensorRef, ValueType},
};

use crate::{
    config::{RuntimeBackend, RuntimeConfig},
    error::{RapidOcrError, Result},
    model_source::ModelSource,
    runtime::provider::{
        ProviderResolution, ResolvedExecutionProvider, resolve_execution_providers,
    },
};

#[derive(Debug)]
pub struct OrtSession {
    session: Session,
    model_path: String,
    provider_resolution: ProviderResolution,
    pub output_names: Vec<String>,
    pub character_list: Option<Vec<String>>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionContract {
    Rec,
    Cls,
    Det,
}

impl OrtSession {
    pub fn new_from_source(
        source: ModelSource<'_>,
        runtime_cfg: &RuntimeConfig,
        contract: SessionContract,
    ) -> Result<Self> {
        if runtime_cfg.backend != RuntimeBackend::OnnxCpu {
            return Err(RapidOcrError::UnsupportedBackend(format!(
                "only `onnx_cpu` is supported in this release, got {:?}",
                runtime_cfg.backend
            )));
        }

        let provider_chain = resolve_execution_providers(
            &runtime_cfg.provider_preference,
            runtime_cfg.enable_cpu_mem_arena,
            runtime_cfg.fail_if_provider_unavailable,
        )?;
        let resolved_provider = provider_chain.resolution.resolved;

        let mut builder = Session::builder()?;
        builder = builder
            .with_optimization_level(GraphOptimizationLevel::Level3)
            .map_err(ort::Error::from)?;

        if resolved_provider == ResolvedExecutionProvider::DirectMl {
            builder = builder
                .with_memory_pattern(false)
                .map_err(ort::Error::from)?
                .with_parallel_execution(false)
                .map_err(ort::Error::from)?;
        }

        let (intra_threads, inter_threads) = derive_runtime_threads(runtime_cfg, resolved_provider);

        if let Some(intra) = intra_threads {
            builder = builder
                .with_intra_threads(intra)
                .map_err(ort::Error::from)?;
        }

        if let Some(inter) = inter_threads {
            builder = builder
                .with_inter_threads(inter)
                .map_err(ort::Error::from)?;
        }

        builder = builder
            .with_execution_providers(provider_chain.providers)
            .map_err(ort::Error::from)?;

        let model_name = source.name();
        let session = match source {
            ModelSource::File(path) => builder.commit_from_file(path)?,
            ModelSource::Memory { bytes, .. } => builder.commit_from_memory(bytes)?,
        };
        validate_model_io_contract(Path::new(&model_name), &session, contract)?;

        let output_names = session
            .outputs()
            .iter()
            .map(|outlet| outlet.name().to_string())
            .collect();

        let character_list = match session.metadata()?.custom("character") {
            Some(raw) if !raw.trim().is_empty() => {
                Some(raw.lines().map(|line| line.to_string()).collect())
            }
            _ => None,
        };

        Ok(Self {
            session,
            model_path: model_name,
            provider_resolution: provider_chain.resolution,
            output_names,
            character_list,
        })
    }

    pub fn provider_resolution(&self) -> ProviderResolution {
        self.provider_resolution
    }

    pub fn run_arrayd_view_with<T, F>(&mut self, input: ArrayView4<'_, f32>, f: F) -> Result<T>
    where
        F: for<'a> FnOnce(ArrayViewD<'a, f32>) -> Result<T>,
    {
        let input_tensor = TensorRef::from_array_view(input)?;
        let outputs = self.session.run(inputs![input_tensor])?;

        let output_name = self.output_names.first().ok_or_else(|| {
            RapidOcrError::Decode(format!(
                "ONNX session has no output names (model={})",
                self.model_path
            ))
        })?;
        let output = outputs.get(output_name.as_str()).ok_or_else(|| {
            RapidOcrError::Decode(format!(
                "ONNX session output `{output_name}` not found in run results (model={})",
                self.model_path
            ))
        })?;

        let arr = output.try_extract_array::<f32>().map_err(|e| {
            RapidOcrError::Decode(format!(
                "failed to extract output `{output_name}` as f32 tensor (model={}): {e}",
                self.model_path
            ))
        })?;
        f(arr.view())
    }

    pub fn run_array2_view_with<T, F>(&mut self, input: ArrayView4<'_, f32>, f: F) -> Result<T>
    where
        F: for<'a> FnOnce(ArrayView2<'a, f32>) -> Result<T>,
    {
        let model_path = self.model_path.clone();
        self.run_arrayd_view_with(input, |arr| {
            let arr = arr.into_dimensionality::<Ix2>().map_err(|e| {
                RapidOcrError::Decode(format!(
                    "unexpected output rank for model {}: expected rank2: {e}",
                    model_path
                ))
            })?;
            f(arr)
        })
    }

    pub fn run_array3_view_with<T, F>(&mut self, input: ArrayView4<'_, f32>, f: F) -> Result<T>
    where
        F: for<'a> FnOnce(ArrayView3<'a, f32>) -> Result<T>,
    {
        let model_path = self.model_path.clone();
        self.run_arrayd_view_with(input, |arr| {
            let arr = arr.into_dimensionality::<Ix3>().map_err(|e| {
                RapidOcrError::Decode(format!(
                    "unexpected output rank for model {}: expected rank3: {e}",
                    model_path
                ))
            })?;
            f(arr)
        })
    }

    pub fn run_array4_view_with<T, F>(&mut self, input: ArrayView4<'_, f32>, f: F) -> Result<T>
    where
        F: for<'a> FnOnce(ndarray::ArrayView4<'a, f32>) -> Result<T>,
    {
        let model_path = self.model_path.clone();
        self.run_arrayd_view_with(input, |arr| {
            let arr = arr.into_dimensionality::<Ix4>().map_err(|e| {
                RapidOcrError::Decode(format!(
                    "unexpected output rank for model {}: expected rank4: {e}",
                    model_path
                ))
            })?;
            f(arr)
        })
    }
}

fn derive_runtime_threads(
    runtime_cfg: &RuntimeConfig,
    resolved_provider: ResolvedExecutionProvider,
) -> (Option<usize>, Option<usize>) {
    let mut intra = runtime_cfg.intra_threads.filter(|v| *v > 0);
    let mut inter = runtime_cfg.inter_threads.filter(|v| *v > 0);

    if runtime_cfg.auto_tune_threads {
        if resolved_provider == ResolvedExecutionProvider::DirectMl {
            // DirectML executes the graph sequentially. Keep ORT's CPU pool
            // small; Rayon handles the independent host-side work.
            intra.get_or_insert(1);
            inter.get_or_insert(1);
        } else {
            let available = runtime_cfg
                .thread_budget
                .filter(|threads| *threads > 0)
                .unwrap_or_else(auto_tuned_thread_budget);
            if intra.is_none() {
                intra = Some(available.max(1));
            }
            if inter.is_none() {
                inter = Some(1);
            }
        }
    }

    (intra, inter)
}

fn auto_tuned_thread_budget() -> usize {
    let physical_cores = num_cpus::get_physical().max(1);
    let available = thread::available_parallelism()
        .ok()
        .map(|v| v.get())
        .unwrap_or(1);
    available.clamp(1, physical_cores)
}

fn validate_model_io_contract(
    model_path: &Path,
    session: &Session,
    contract: SessionContract,
) -> Result<()> {
    let inputs = session.inputs();
    let outputs = session.outputs();

    if inputs.len() != 1 {
        return Err(RapidOcrError::Config(format!(
            "recognition model must expose exactly one input, got {} (model={})",
            inputs.len(),
            model_path.display()
        )));
    }
    if outputs.is_empty() {
        return Err(RapidOcrError::Config(format!(
            "recognition model must expose at least one output (model={})",
            model_path.display()
        )));
    }

    let input = &inputs[0];
    validate_tensor_spec(
        model_path,
        "input",
        input.name(),
        input.dtype(),
        Some(4),
        TensorElementType::Float32,
    )?;

    let output = &outputs[0];
    let output_rank = match contract {
        SessionContract::Rec => Some(3),
        SessionContract::Cls => Some(2),
        SessionContract::Det => Some(4),
    };
    validate_tensor_spec(
        model_path,
        "output",
        output.name(),
        output.dtype(),
        output_rank,
        TensorElementType::Float32,
    )?;

    Ok(())
}

fn validate_tensor_spec(
    model_path: &Path,
    io_kind: &str,
    io_name: &str,
    value_type: &ValueType,
    expected_rank: Option<usize>,
    expected_tensor_type: TensorElementType,
) -> Result<()> {
    if !value_type.is_tensor() {
        return Err(RapidOcrError::Config(format!(
            "model {io_kind} `{io_name}` must be a tensor, got `{value_type}` (model={})",
            model_path.display()
        )));
    }

    let actual_rank = value_type
        .tensor_shape()
        .map(|shape| shape.len())
        .unwrap_or_default();
    if let Some(expected_rank) = expected_rank
        && actual_rank != expected_rank
    {
        return Err(RapidOcrError::Config(format!(
            "model {io_kind} `{io_name}` rank mismatch: expected {expected_rank}, got {actual_rank} (type={value_type}, model={})",
            model_path.display()
        )));
    }

    let actual_type = value_type.tensor_type().ok_or_else(|| {
        RapidOcrError::Config(format!(
            "model {io_kind} `{io_name}` has no tensor element type (type={value_type}, model={})",
            model_path.display()
        ))
    })?;

    if actual_type != expected_tensor_type {
        return Err(RapidOcrError::Config(format!(
            "model {io_kind} `{io_name}` dtype mismatch: expected {:?}, got {:?} (model={})",
            expected_tensor_type,
            actual_type,
            model_path.display()
        )));
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use super::derive_runtime_threads;
    use crate::{
        config::{ProviderPreference, RuntimeConfig},
        runtime::provider::ResolvedExecutionProvider,
    };

    #[test]
    fn directml_auto_tuning_uses_single_ort_threads() {
        let config = RuntimeConfig {
            provider_preference: ProviderPreference::DirectMl { device_id: 0 },
            ..RuntimeConfig::default()
        };

        assert_eq!(
            derive_runtime_threads(&config, ResolvedExecutionProvider::DirectMl),
            (Some(1), Some(1))
        );
    }

    #[test]
    fn directml_auto_tuning_preserves_explicit_thread_settings() {
        let config = RuntimeConfig {
            intra_threads: Some(3),
            inter_threads: Some(2),
            provider_preference: ProviderPreference::DirectMl { device_id: 0 },
            ..RuntimeConfig::default()
        };

        assert_eq!(
            derive_runtime_threads(&config, ResolvedExecutionProvider::DirectMl),
            (Some(3), Some(2))
        );
    }

    #[test]
    fn disabled_auto_tuning_leaves_unspecified_threads_to_ort() {
        let config = RuntimeConfig {
            auto_tune_threads: false,
            provider_preference: ProviderPreference::DirectMl { device_id: 0 },
            ..RuntimeConfig::default()
        };

        assert_eq!(
            derive_runtime_threads(&config, ResolvedExecutionProvider::DirectMl),
            (None, None)
        );
    }

    #[test]
    fn auto_fallback_to_cpu_uses_the_cpu_thread_budget() {
        let config = RuntimeConfig {
            provider_preference: ProviderPreference::Auto { device_id: 0 },
            ..RuntimeConfig::default()
        };
        let (intra, inter) = derive_runtime_threads(&config, ResolvedExecutionProvider::Cpu);
        assert!(intra.is_some_and(|threads| threads >= 1));
        assert_eq!(inter, Some(1));
    }
}
