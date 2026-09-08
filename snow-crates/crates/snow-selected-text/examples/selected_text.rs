use std::time::Duration;

use snow_selected_text::{CaptureOptions, SelectedTextService, SelectionOutcome};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let service = SelectedTextService::new()?;
    let copy_fallback = !std::env::args().any(|argument| argument == "--no-copy");
    eprintln!("Select text in another application within five seconds.");
    eprintln!("Copy fallback: {copy_fallback}. It may change the clipboard.");
    std::thread::sleep(Duration::from_secs(5));
    let request = service.start_capture(CaptureOptions {
        copy_fallback,
        ..Default::default()
    })?;
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
