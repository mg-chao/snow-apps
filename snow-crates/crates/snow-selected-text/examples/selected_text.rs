use std::time::Duration;

use snow_selected_text::{CaptureOptions, CaptureStrategy, SelectedTextService, SelectionOutcome};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let service = SelectedTextService::new()?;
    let mut options = CaptureOptions::default();
    for argument in std::env::args().skip(1) {
        match argument.as_str() {
            "--no-copy" => options.copy_fallback = false,
            "--accessibility" => options.strategy = CaptureStrategy::Accessibility,
            "--native-control" => options.strategy = CaptureStrategy::NativeControl,
            "--clipboard" => options.strategy = CaptureStrategy::Clipboard,
            _ => {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::InvalidInput,
                    format!("unknown argument: {argument}"),
                )
                .into());
            }
        }
    }
    eprintln!("Select text in another application within five seconds.");
    eprintln!(
        "Strategy: {:?}; Copy fallback: {}. Copy may change the clipboard.",
        options.strategy, options.copy_fallback
    );
    std::thread::sleep(Duration::from_secs(5));
    let request = service.start_capture(options)?;
    match request.wait().as_ref() {
        Ok(SelectionOutcome::Selected(text)) => {
            eprintln!(
                "Method: {:?}; clipboard: {:?}",
                text.method, text.clipboard_status
            );
            println!("{}", text.text);
        }
        Ok(SelectionOutcome::NoSelection) => eprintln!("No text is selected."),
        Ok(SelectionOutcome::Unsupported) => {
            eprintln!("The application does not expose a supported selection.")
        }
        Err(error) => {
            eprintln!("Clipboard: {:?}", error.clipboard_status);
            return Err(error.clone().into());
        }
    }
    Ok(())
}
