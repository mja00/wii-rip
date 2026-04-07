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

const TOOL_ENV_OVERRIDES: [(&str, &str); 4] = [
    ("dolphin-tool", "WII_RIP_DOLPHIN_TOOL"),
    ("wit", "WII_RIP_WIT"),
    ("wii-banner-render", "WII_RIP_BANNER_RENDER"),
    ("ffmpeg", "WII_RIP_FFMPEG"),
];

struct Args {
    input: PathBuf,
    output: PathBuf,
    keep_temp: bool,
    video: bool,
    video_only: bool,
    video_aspect: VideoAspectSelection,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum VideoAspectSelection {
    Standard,
    Widescreen,
    Both,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum VideoAspect {
    Standard,
    Widescreen,
}

impl VideoAspectSelection {
    fn parse(value: &str) -> Result<Self> {
        match value {
            "4:3" => Ok(Self::Standard),
            "16:9" => Ok(Self::Widescreen),
            "both" => Ok(Self::Both),
            _ => bail!("unsupported --video-aspect '{value}'; expected one of: 4:3, 16:9, both"),
        }
    }

    fn variants(self) -> &'static [VideoAspect] {
        match self {
            Self::Standard => &[VideoAspect::Standard],
            Self::Widescreen => &[VideoAspect::Widescreen],
            Self::Both => &[VideoAspect::Standard, VideoAspect::Widescreen],
        }
    }

    fn keeps_legacy_filenames(self) -> bool {
        matches!(self, Self::Standard)
    }
}

impl VideoAspect {
    fn cli_value(self) -> &'static str {
        match self {
            Self::Standard => "4:3",
            Self::Widescreen => "16:9",
        }
    }

    fn suffix(self) -> &'static str {
        match self {
            Self::Standard => "4x3",
            Self::Widescreen => "16x9",
        }
    }
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

    rip(
        &args.input,
        &args.output,
        args.keep_temp,
        args.video,
        args.video_only,
        args.video_aspect,
    )
}

fn parse_args() -> Result<Args> {
    let mut parser = lexopt::Parser::from_env();
    let mut input = None;
    let mut output = PathBuf::from("output");
    let mut keep_temp = false;
    let mut video = false;
    let mut video_only = false;
    let mut video_aspect = VideoAspectSelection::Standard;

    while let Some(argument) = parser.next()? {
        match argument {
            Short('o') | Long("output") => {
                output = PathBuf::from(parser.value()?);
            }
            Long("keep-temp") => {
                keep_temp = true;
            }
            Long("video") => {
                video = true;
            }
            Long("video-only") => {
                video_only = true;
            }
            Long("video-aspect") => {
                let value = parser.value()?;
                let value = value
                    .to_str()
                    .context("--video-aspect must be valid UTF-8")?;
                video_aspect = VideoAspectSelection::parse(value)?;
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
        video,
        video_only,
        video_aspect,
    })
}

fn print_help() {
    println!(
        "wii-rip\n\nExtract Wii disc channel audio and/or banner animation from .rvz, .iso, and .wbfs images.\n\nUSAGE:\n    wii-rip <input> [OPTIONS]\n\nOPTIONS:\n    -o, --output <dir>         Output directory (default: ./output)\n        --keep-temp            Save extracted sound.bin alongside the WAV\n        --video                Also render the banner animation to an MP4\n        --video-only           Render banner animation only; skip audio extraction\n        --video-aspect <mode>  Banner aspect ratio: 4:3, 16:9, or both (default: 4:3)\n    -h, --help                 Show this help text\n\nEXTERNAL TOOLS:\n    dolphin-tool         Required for .rvz input (override: $WII_RIP_DOLPHIN_TOOL)\n    wit                  Required for disc image extraction (override: $WII_RIP_WIT)\n    wii-banner-render    Required for --video / --video-only (override: $WII_RIP_BANNER_RENDER)\n    ffmpeg               Optional; muxes audio + video into a single file (override: $WII_RIP_FFMPEG)\n"
    );
}

fn rip(
    input_path: &Path,
    output_dir: &Path,
    keep_temp: bool,
    video: bool,
    video_only: bool,
    video_aspect: VideoAspectSelection,
) -> Result<()> {
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

    let stem = input_path
        .file_stem()
        .context("input path does not have a valid file stem")?
        .to_string_lossy()
        .into_owned();

    let wav_path = if !video_only {
        println!("Parsing opening.bnr...");
        let bnr_data = fs::read(&bnr_path)
            .with_context(|| format!("failed to read {}", bnr_path.display()))?;
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
        let wav_path = output_dir.join(format!("{stem}_disc_channel.wav"));
        let (sample_rate, channels, num_samples) =
            bns::decode_bns_to_wav(&sound_bin, &wav_path).context("failed to decode audio")?;

        let duration = num_samples as f64 / f64::from(sample_rate);
        println!(
            "  {}ch, {} Hz, {} samples ({duration:.1}s)",
            channels, sample_rate, num_samples
        );

        Some(wav_path)
    } else {
        None
    };

    let banner_paths = if video || video_only {
        println!("Rendering banner animation...");
        let mut banner_paths = Vec::new();
        for aspect in video_aspect.variants() {
            let banner_video_path =
                output_dir.join(banner_output_name(&stem, *aspect, video_aspect));
            render_banner(&bnr_path, &banner_video_path, *aspect)?;
            banner_paths.push((*aspect, banner_video_path));
        }
        banner_paths
    } else {
        Vec::new()
    };

    if let Some(wav) = &wav_path {
        if !banner_paths.is_empty() {
            match try_find_tool("ffmpeg")? {
                Some(ffmpeg) => {
                    println!("Muxing audio + video...");
                    let mut muxed_paths = Vec::new();
                    for (aspect, banner_path) in &banner_paths {
                        let muxed_path =
                            output_dir.join(muxed_output_name(&stem, *aspect, video_aspect));
                        mux_audio_video(&ffmpeg, wav, banner_path, &muxed_path)?;
                        muxed_paths.push(muxed_path);
                    }
                    print_output_paths(&muxed_paths);
                }
                None => {
                    println!("\nffmpeg not found; audio and video saved as separate files.");
                    println!("  To mux manually:");
                    for (aspect, banner_path) in &banner_paths {
                        let muxed_path =
                            output_dir.join(muxed_output_name(&stem, *aspect, video_aspect));
                        println!(
                            "  ffmpeg -i \"{}\" -i \"{}\" -c:v copy -c:a aac -shortest -y \"{}\"",
                            banner_path.display(),
                            wav.display(),
                            muxed_path.display()
                        );
                    }

                    let mut outputs = vec![wav.clone()];
                    outputs.extend(banner_paths.iter().map(|(_, path)| path.clone()));
                    print_output_paths(&outputs);
                }
            }
        } else {
            print_output_paths(std::slice::from_ref(wav));
        }
    } else if !banner_paths.is_empty() {
        let outputs: Vec<PathBuf> = banner_paths.into_iter().map(|(_, path)| path).collect();
        print_output_paths(&outputs);
    } else {
        unreachable!("audio and video both skipped");
    }

    Ok(())
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

fn render_banner(bnr_path: &Path, output_path: &Path, aspect: VideoAspect) -> Result<()> {
    let renderer = check_tool("wii-banner-render")?;
    let output = Command::new(&renderer)
        .args([
            bnr_path.to_str().context(
                "opening.bnr path contains non-UTF-8 data unsupported by wii-banner-render invocation",
            )?,
            "-o",
            output_path.to_str().context(
                "output path contains non-UTF-8 data unsupported by wii-banner-render invocation",
            )?,
            "--aspect",
            aspect.cli_value(),
        ])
        .output()
        .with_context(|| format!("failed to run {}", renderer.display()))?;

    if !output.status.success() {
        bail!(
            "wii-banner-render failed:\n{}",
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }

    println!("  Banner video written to: {}", output_path.display());
    Ok(())
}

fn banner_output_name(stem: &str, aspect: VideoAspect, selection: VideoAspectSelection) -> String {
    if selection.keeps_legacy_filenames() && aspect == VideoAspect::Standard {
        format!("{stem}_disc_channel_banner.mp4")
    } else {
        format!("{stem}_disc_channel_banner_{}.mp4", aspect.suffix())
    }
}

fn muxed_output_name(stem: &str, aspect: VideoAspect, selection: VideoAspectSelection) -> String {
    if selection.keeps_legacy_filenames() && aspect == VideoAspect::Standard {
        format!("{stem}_disc_channel.mp4")
    } else {
        format!("{stem}_disc_channel_{}.mp4", aspect.suffix())
    }
}

fn print_output_paths(paths: &[PathBuf]) {
    match paths {
        [path] => println!("\nOutput: {}", path.display()),
        _ => {
            println!("\nOutputs:");
            for path in paths {
                println!("  {}", path.display());
            }
        }
    }
}

fn mux_audio_video(
    ffmpeg: &Path,
    wav_path: &Path,
    video_path: &Path,
    output_path: &Path,
) -> Result<()> {
    let output = Command::new(ffmpeg)
        .args([
            "-i",
            video_path
                .to_str()
                .context("video path contains non-UTF-8 data")?,
            "-i",
            wav_path
                .to_str()
                .context("audio path contains non-UTF-8 data")?,
            "-c:v",
            "copy",
            "-c:a",
            "aac",
            "-shortest",
            "-y",
            output_path
                .to_str()
                .context("output path contains non-UTF-8 data")?,
        ])
        .output()
        .with_context(|| format!("failed to run {}", ffmpeg.display()))?;

    if !output.status.success() {
        bail!(
            "ffmpeg failed:\n{}",
            String::from_utf8_lossy(&output.stderr).trim()
        );
    }

    println!("  Muxed output written to: {}", output_path.display());
    Ok(())
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

fn try_find_tool(name: &str) -> Result<Option<PathBuf>> {
    let candidates = tool_candidates(name)?;
    for candidate in &candidates {
        if is_executable(candidate)? {
            return Ok(Some(candidate.clone()));
        }
    }
    Ok(None)
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
