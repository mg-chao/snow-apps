pub fn cargo_directives(line: &str) -> Vec<String> {
    let argument = line.trim();
    if argument.is_empty() || argument.starts_with('#') {
        return Vec::new();
    }

    if let Some(framework) = argument.strip_prefix("-framework ") {
        let framework = framework.trim();
        if !framework.is_empty() && !framework.contains(char::is_whitespace) {
            return vec![format!("cargo:rustc-link-lib=framework={framework}")];
        }
    }

    vec![format!("cargo:rustc-link-arg={argument}")]
}
