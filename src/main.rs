mod bnr;
mod bns;

use std::collections::HashSet;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

use anyhow::{Context, Result, bail};
use lexopt::prelude::*;
use tempfile::TempDir;

const TOOL_ENV_OVERRIDES: [(&str, &str); 2] = [
    ("dolphin-tool", "WII_RIP_DOLPHIN_TOOL"),
    ("wit", "WII_RIP_WIT"),
];

struct Args {
    input: PathBuf,
    output: PathBuf,
    keep_temp: bool,
}

fn main() {
    if let Err(error) = run() {
        eprintln!("Error: {error:#}");
        process::exit(1);
    }
}

fn run() -> Result<()> {
    let args = parse_args()?;
    if !args.input.exists() {
        bail!("input file not found: {}", args.input.display());
    }

    let output_path = rip(&args.input, &args.output, args.keep_temp)?;
    println!("\nOutput: {}", output_path.display());
    Ok(())
}

fn parse_args() -> Result<Args> {
    let mut parser = lexopt::Parser::from_env();
    let mut input = None;
    let mut output = PathBuf::from("output");
    let mut keep_temp = false;

    while let Some(argument) = parser.next()? {
        match argument {
            Short('o') | Long("output") => {
                output = PathBuf::from(parser.value()?);
            }
            Long("keep-temp") => {
                keep_temp = true;
            }
            Short('h') | Long("help") => {
                print_help();
                process::exit(0);
            }
            Value(value) if input.is_none() => {
                input = Some(PathBuf::from(value));
            }
            Value(value) => {
                bail!(
                    "unexpected extra positional argument: {}",
                    PathBuf::from(value).display()
                );
            }
            _ => return Err(argument.unexpected().into()),
        }
    }

    let input = input.context("missing input ROM file (.rvz, .iso, or .wbfs)")?;
    Ok(Args {
        input,
        output,
        keep_temp,
    })
}

fn print_help() {
    println!(
        "wii-rip\n\nExtract Wii disc channel music from .rvz, .iso, and .wbfs images into a PCM WAV file.\n\nUSAGE:\n    wii-rip <input> [OPTIONS]\n\nOPTIONS:\n    -o, --output <dir>   Output directory (default: ./output)\n        --keep-temp      Save extracted sound.bin alongside the WAV\n    -h, --help           Show this help text\n"
    );
}

fn rip(input_path: &Path, output_dir: &Path, keep_temp: bool) -> Result<PathBuf> {
    let suffix = input_path
        .extension()
        .and_then(|extension| extension.to_str())
        .map(|extension| extension.to_ascii_lowercase())
        .unwrap_or_default();

    fs::create_dir_all(output_dir)
        .with_context(|| format!("failed to create output directory {}", output_dir.display()))?;

    let temp_dir =
        TempDir::with_prefix("wii_rip_").context("failed to create temporary directory")?;
    let temp_path = temp_dir.path();

    let iso_path = match suffix.as_str() {
        "rvz" => {
            let converted_iso = temp_path.join("game.iso");
            rvz_to_iso(input_path, &converted_iso)?;
            converted_iso
        }
        "iso" | "wbfs" => input_path.to_path_buf(),
        _ => bail!(
            "unsupported format '.{}'; expected .rvz, .iso, or .wbfs",
            if suffix.is_empty() { "<none>" } else { &suffix }
        ),
    };

    let extract_dir = temp_path.join("extracted");
    let bnr_path = extract_opening_bnr(&iso_path, &extract_dir)?;

    println!("Parsing opening.bnr...");
    let bnr_data =
        fs::read(&bnr_path).with_context(|| format!("failed to read {}", bnr_path.display()))?;
    let sound_bin = bnr::extract_sound_bin(&bnr_data).context("failed to parse opening.bnr")?;
    println!(
        "  Extracted sound.bin ({} bytes, format: {})",
        sound_bin.len(),
        magic_label(&sound_bin)
    );

    if keep_temp {
        let kept_path = output_dir.join("sound.bin");
        fs::write(&kept_path, &sound_bin)
            .with_context(|| format!("failed to write {}", kept_path.display()))?;
        println!("  Saved sound.bin to: {}", kept_path.display());
    }

    println!("Decoding audio...");
    let stem = input_path
        .file_stem()
        .context("input path does not have a valid file stem")?;
    let wav_path = output_dir.join(format!("{}_disc_channel.wav", stem.to_string_lossy()));
    let (sample_rate, channels, num_samples) =
        bns::decode_bns_to_wav(&sound_bin, &wav_path).context("failed to decode audio")?;

    let duration = num_samples as f64 / f64::from(sample_rate);
    println!(
        "  {}ch, {} Hz, {} samples ({duration:.1}s)",
        channels, sample_rate, num_samples
    );

    Ok(wav_path)
}

fn rvz_to_iso(rvz_path: &Path, iso_path: &Path) -> Result<()> {
    let dolphin = check_tool("dolphin-tool")?;
    println!("Converting RVZ -> ISO (this may take a while)...");
    let output = Command::new(&dolphin)
        .args([
            "convert",
            "-f",
            "iso",
            "-i",
            rvz_path.to_str().context(
                "input path contains non-UTF-8 data unsupported by dolphin-tool invocation",
            )?,
            "-o",
            iso_path.to_str().context(
                "temporary ISO path contains non-UTF-8 data unsupported by dolphin-tool invocation",
            )?,
        ])
        .output()
        .with_context(|| format!("failed to run {}", dolphin.display()))?;

    if !output.status.success() {
        bail!(
            "dolphin-tool failed:\n{}",
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }

    println!("  ISO written to: {}", iso_path.display());
    Ok(())
}

fn extract_opening_bnr(image_path: &Path, extract_dir: &Path) -> Result<PathBuf> {
    let wit = check_tool("wit")?;
    println!("Extracting opening.bnr from disc image...");
    let output = Command::new(&wit)
        .args([
            "extract",
            image_path
                .to_str()
                .context("disc image path contains non-UTF-8 data unsupported by wit invocation")?,
            "--dest",
            extract_dir.to_str().context(
                "temporary extraction path contains non-UTF-8 data unsupported by wit invocation",
            )?,
            "--psel",
            "data",
            "--files",
            "+opening.bnr",
            "--overwrite",
            "--quiet",
        ])
        .output()
        .with_context(|| format!("failed to run {}", wit.display()))?;

    if !output.status.success() {
        bail!(
            "wit failed:\n{}",
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }

    let bnr_path = find_file_named(extract_dir, "opening.bnr")?.context(
        "opening.bnr not found after extraction; this disc may not have a disc channel banner",
    )?;
    let size = fs::metadata(&bnr_path)
        .with_context(|| format!("failed to inspect {}", bnr_path.display()))?
        .len();
    println!("  Found: {} ({} bytes)", bnr_path.display(), size);

    Ok(bnr_path)
}

fn check_tool(name: &str) -> Result<PathBuf> {
    let candidates = tool_candidates(name)?;
    for candidate in &candidates {
        if is_executable(candidate)? {
            return Ok(candidate.clone());
        }
    }

    bail!(missing_tool_message(name, &candidates)?);
}

fn tool_candidates(name: &str) -> Result<Vec<PathBuf>> {
    let mut candidates = Vec::new();

    if let Some(override_name) = tool_override_env(name)
        && let Some(path) = env::var_os(override_name)
    {
        candidates.push(PathBuf::from(path));
    }

    if let Some(tools_dir) = env::var_os("WII_RIP_TOOLS_DIR") {
        candidates.push(PathBuf::from(tools_dir).join(name));
    }

    if let Some(bundle_root) = bundle_root()? {
        candidates.push(bundle_root.join("tools").join(name));
        candidates.push(bundle_root.join(name));
    }

    candidates.extend(path_candidates(name));

    let mut seen = HashSet::new();
    let mut deduped = Vec::new();
    for candidate in candidates {
        let key = candidate.clone();
        if seen.insert(key) {
            deduped.push(candidate);
        }
    }

    Ok(deduped)
}

fn tool_override_env(name: &str) -> Option<&'static str> {
    TOOL_ENV_OVERRIDES
        .iter()
        .find_map(|(tool, env_name)| (*tool == name).then_some(*env_name))
}

fn bundle_root() -> Result<Option<PathBuf>> {
    let current_exe = env::current_exe().context("failed to resolve current executable path")?;
    Ok(current_exe.parent().map(Path::to_path_buf))
}

fn path_candidates(name: &str) -> Vec<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(path_value) = env::var_os("PATH") {
        for path in env::split_paths(&path_value) {
            candidates.push(path.join(name));
        }
    }
    candidates
}

fn missing_tool_message(name: &str, candidates: &[PathBuf]) -> Result<String> {
    let override_name = tool_override_env(name).context("unknown helper requested")?;
    let mut lines = vec![format!("required helper '{name}' was not found")];
    lines.push(format!("searched override: ${override_name}"));
    lines.push("searched override directory: $WII_RIP_TOOLS_DIR/<tool>".to_string());
    lines.push("searched executable-relative paths: ./tools/<tool> and ./<tool>".to_string());
    lines.push("searched PATH as a fallback".to_string());
    if !candidates.is_empty() {
        lines.push("probed candidates:".to_string());
        for candidate in candidates {
            lines.push(format!("  {}", candidate.display()));
        }
    }
    Ok(lines.join("\n"))
}

fn is_executable(path: &Path) -> Result<bool> {
    let metadata = match fs::metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(false),
        Err(error) => {
            return Err(error).with_context(|| format!("failed to inspect {}", path.display()));
        }
    };

    if !metadata.is_file() {
        return Ok(false);
    }

    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        Ok(metadata.permissions().mode() & 0o111 != 0)
    }

    #[cfg(not(unix))]
    {
        Ok(true)
    }
}

fn find_file_named(root: &Path, target_name: &str) -> Result<Option<PathBuf>> {
    if !root.exists() {
        return Ok(None);
    }

    let entries = fs::read_dir(root)
        .with_context(|| format!("failed to read directory {}", root.display()))?;
    for entry in entries {
        let entry =
            entry.with_context(|| format!("failed to traverse directory {}", root.display()))?;
        let path = entry.path();
        let file_type = entry
            .file_type()
            .with_context(|| format!("failed to inspect {}", path.display()))?;
        if file_type.is_dir() {
            if let Some(found) = find_file_named(&path, target_name)? {
                return Ok(Some(found));
            }
        } else if file_type.is_file()
            && entry
                .file_name()
                .to_string_lossy()
                .eq_ignore_ascii_case(target_name)
        {
            return Ok(Some(path));
        }
    }

    Ok(None)
}

fn magic_label(data: &[u8]) -> String {
    let magic = &data[..data.len().min(4)];
    if magic
        .iter()
        .all(|byte| byte.is_ascii_graphic() || *byte == b' ')
    {
        String::from_utf8_lossy(magic).into_owned()
    } else {
        let mut parts = Vec::with_capacity(magic.len());
        for byte in magic {
            parts.push(format!("{byte:02X}"));
        }
        parts.join("")
    }
}
