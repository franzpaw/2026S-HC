pub mod smoke;

use std::ffi::OsString;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

use clap::{Args, Parser, Subcommand};
use siri_client::{
    AudioFileInput, ChatErrorCode, ChatInput, ChatMessage, ChatRequestOptions, ChatStreamEvent,
    Phase, Role, SiriClientError, validate_audio_content_type,
};

pub const DEFAULT_BACKEND_URL: &str = "http://localhost:8000";
pub const DEFAULT_TOKEN: &str = "test-token";

#[derive(Debug, Parser)]
#[command(name = "siri")]
#[command(about = "Use the local Siri backend through the Phase 1.4 chat stream contract.")]
pub struct Cli {
    #[arg(long, env = "VOICE_API_URL", default_value = DEFAULT_BACKEND_URL)]
    pub backend_url: String,
    #[arg(long, env = "VOICE_API_TOKEN", default_value = DEFAULT_TOKEN)]
    pub token: String,
    #[arg(long, global = true)]
    pub no_tts: bool,
    #[arg(long, global = true)]
    pub audio_output_dir: Option<PathBuf>,
    #[command(subcommand)]
    pub command: Command,
}

#[derive(Debug, Subcommand)]
pub enum Command {
    Health,
    Reset,
    Chat(ChatCommand),
    Smoke(SmokeCommand),
}

#[derive(Debug, Args)]
pub struct ChatCommand {
    #[command(subcommand)]
    pub command: ChatSubcommand,
}

#[derive(Debug, Subcommand)]
pub enum ChatSubcommand {
    Text(TextCommand),
    Voice(VoiceCommand),
}

#[derive(Debug, Args)]
pub struct TextCommand {
    #[arg(required = true, trailing_var_arg = true)]
    pub text: Vec<String>,
}

#[derive(Debug, Args)]
pub struct VoiceCommand {
    #[arg(long)]
    pub file: PathBuf,
    #[arg(long)]
    pub content_type: Option<String>,
}

#[derive(Debug, Args)]
pub struct SmokeCommand {
    #[arg(long, default_value = "../backend/api/tests/fixtures")]
    pub fixture_dir: PathBuf,
    #[arg(long, default_value = "../backend/deploy/.pi/agent/auth.json")]
    pub auth_file: PathBuf,
    #[arg(long, env = "VOICE_SMOKE_TIMEOUT_SECONDS", default_value_t = 120)]
    pub timeout_seconds: u64,
    #[arg(long, env = "VOICE_SMOKE_BARGE_IN_DELAY_MILLIS", default_value_t = 500)]
    pub barge_in_delay_millis: u64,
}

#[derive(Debug, thiserror::Error)]
pub enum CliError {
    #[error("chat text input is required")]
    EmptyText,
    #[error("voice file does not exist: {0}")]
    MissingVoiceFile(PathBuf),
    #[error("could not infer audio content type for: {0}")]
    UnknownAudioContentType(PathBuf),
    #[error("voice file name is required: {0}")]
    MissingVoiceFilename(PathBuf),
    #[error("stream error {code:?}: {message}")]
    StreamEvent {
        code: ChatErrorCode,
        message: String,
    },
    #[error("failed to write audio file {path}: {source}")]
    WriteAudio { path: PathBuf, source: io::Error },
    #[error("smoke failed: {0}")]
    Smoke(String),
    #[error("client error: {0}")]
    Client(#[from] SiriClientError),
    #[error("output error: {0}")]
    Output(#[from] io::Error),
}

pub fn parse_cli_from<I, T>(args: I) -> Result<Cli, clap::Error>
where
    I: IntoIterator<Item = T>,
    T: Into<OsString> + Clone,
{
    Cli::try_parse_from(args)
}

pub fn chat_options_from_cli(cli: &Cli) -> Result<Option<ChatRequestOptions>, CliError> {
    let tts_enabled = !cli.no_tts;
    match &cli.command {
        Command::Chat(chat) => match &chat.command {
            ChatSubcommand::Text(command) => {
                let text = command.text.join(" ");
                if text.trim().is_empty() {
                    return Err(CliError::EmptyText);
                }
                Ok(Some(ChatRequestOptions {
                    input: ChatInput::Text(text),
                    tts_enabled,
                }))
            }
            ChatSubcommand::Voice(command) => {
                if !command.file.exists() {
                    return Err(CliError::MissingVoiceFile(command.file.clone()));
                }
                let filename = command
                    .file
                    .file_name()
                    .and_then(|name| name.to_str())
                    .ok_or_else(|| CliError::MissingVoiceFilename(command.file.clone()))?
                    .to_owned();
                let content_type = match &command.content_type {
                    Some(content_type) => content_type.clone(),
                    None => infer_audio_content_type(&command.file)
                        .ok_or_else(|| CliError::UnknownAudioContentType(command.file.clone()))?,
                };
                validate_audio_content_type(&content_type)?;
                Ok(Some(ChatRequestOptions {
                    input: ChatInput::AudioFile(AudioFileInput {
                        path: command.file.clone(),
                        filename,
                        content_type,
                    }),
                    tts_enabled,
                }))
            }
        },
        _ => Ok(None),
    }
}

pub fn infer_audio_content_type(path: &Path) -> Option<String> {
    let extension = path.extension()?.to_str()?.to_ascii_lowercase();
    let content_type = match extension.as_str() {
        "flac" => "audio/flac",
        "m4a" => "audio/m4a",
        "mp3" | "mpeg" => "audio/mpeg",
        "mp4" => "audio/mp4",
        "mpga" => "audio/mpga",
        "oga" | "ogg" => "audio/ogg",
        "wav" | "wave" => "audio/wav",
        "webm" => "audio/webm",
        _ => return None,
    };
    Some(content_type.to_owned())
}

pub async fn write_events<W: Write>(
    events: &[ChatStreamEvent],
    audio_output_dir: Option<&Path>,
    writer: &mut W,
) -> Result<Vec<PathBuf>, CliError> {
    let mut saved_audio = Vec::new();
    for event in events {
        match event {
            ChatStreamEvent::Message(message) => {
                writeln!(writer, "{}", format_message(message))?;
                if message.audio_base64.is_some() {
                    message.decoded_audio()?;
                }
                if message.audio_base64.is_some() && audio_output_dir.is_some() {
                    let output_dir = audio_output_dir.expect("checked above");
                    tokio::fs::create_dir_all(output_dir)
                        .await
                        .map_err(|source| CliError::WriteAudio {
                            path: output_dir.to_path_buf(),
                            source,
                        })?;
                    let audio = message.decoded_audio()?.unwrap_or_default();
                    let path = output_dir.join(
                        format!("{:03}-{:?}.mp3", message.sequence, message.phase)
                            .to_ascii_lowercase(),
                    );
                    tokio::fs::write(&path, audio).await.map_err(|source| {
                        CliError::WriteAudio {
                            path: path.clone(),
                            source,
                        }
                    })?;
                    writeln!(writer, "audio saved: {}", path.display())?;
                    saved_audio.push(path);
                }
            }
            ChatStreamEvent::Error(error) => {
                writeln!(writer, "error {:?}: {}", error.code, error.message)?;
                return Err(CliError::StreamEvent {
                    code: error.code.clone(),
                    message: error.message.clone(),
                });
            }
            ChatStreamEvent::Interrupted => {
                writeln!(writer, "interrupted")?;
            }
            ChatStreamEvent::Done => {
                writeln!(writer, "done")?;
            }
        }
    }
    Ok(saved_audio)
}

pub fn format_message(message: &ChatMessage) -> String {
    let role = match message.role {
        Role::User => "user",
        Role::Assistant => "assistant",
    };
    let phase = match message.phase {
        Phase::Input => "input",
        Phase::Commentary => "commentary",
        Phase::Final => "final",
    };
    format!("{role} {phase}: {}", message.text)
}

#[cfg(test)]
mod tests {
    use super::*;
    use siri_client::ChatError;
    use uuid::Uuid;

    #[test]
    fn parses_smoke_command_options() {
        let cli = parse_cli_from([
            "siri",
            "smoke",
            "--fixture-dir",
            "fixtures",
            "--auth-file",
            "auth.json",
            "--timeout-seconds",
            "240",
            "--barge-in-delay-millis",
            "250",
        ])
        .unwrap();

        match cli.command {
            Command::Smoke(command) => {
                assert_eq!(command.fixture_dir, PathBuf::from("fixtures"));
                assert_eq!(command.auth_file, PathBuf::from("auth.json"));
                assert_eq!(command.timeout_seconds, 240);
                assert_eq!(command.barge_in_delay_millis, 250);
            }
            _ => panic!("expected smoke command"),
        }
    }

    #[test]
    fn parses_global_options_and_text_chat_command() {
        let cli = parse_cli_from([
            "siri",
            "--backend-url",
            "http://localhost:9000",
            "--token",
            "secret",
            "--no-tts",
            "chat",
            "text",
            "Hallo",
            "User",
        ])
        .unwrap();

        assert_eq!(cli.backend_url, "http://localhost:9000");
        assert_eq!(cli.token, "secret");
        assert!(cli.no_tts);
        assert_eq!(
            chat_options_from_cli(&cli).unwrap(),
            Some(ChatRequestOptions {
                input: ChatInput::Text("Hallo User".to_owned()),
                tts_enabled: false,
            })
        );
    }

    #[test]
    fn builds_voice_chat_options_from_existing_audio_file() {
        let temp_dir = tempfile::tempdir().unwrap();
        let audio_path = temp_dir.path().join("prompt.webm");
        std::fs::write(&audio_path, b"webm").unwrap();
        let cli = parse_cli_from([
            OsString::from("siri"),
            OsString::from("chat"),
            OsString::from("voice"),
            OsString::from("--file"),
            audio_path.clone().into_os_string(),
        ])
        .unwrap();

        assert_eq!(
            chat_options_from_cli(&cli).unwrap(),
            Some(ChatRequestOptions {
                input: ChatInput::AudioFile(AudioFileInput {
                    path: audio_path,
                    filename: "prompt.webm".to_owned(),
                    content_type: "audio/webm".to_owned(),
                }),
                tts_enabled: true,
            })
        );
    }

    #[test]
    fn infers_supported_audio_content_types() {
        assert_eq!(
            infer_audio_content_type(Path::new("prompt.wav")),
            Some("audio/wav".to_owned())
        );
        assert_eq!(
            infer_audio_content_type(Path::new("prompt.mpeg")),
            Some("audio/mpeg".to_owned())
        );
        assert_eq!(
            infer_audio_content_type(Path::new("prompt.mpga")),
            Some("audio/mpga".to_owned())
        );
        assert_eq!(infer_audio_content_type(Path::new("prompt.txt")), None);
    }

    #[test]
    fn rejects_missing_voice_file_before_calling_backend() {
        let cli = parse_cli_from(["siri", "chat", "voice", "--file", "missing.wav"]).unwrap();

        assert!(matches!(
            chat_options_from_cli(&cli),
            Err(CliError::MissingVoiceFile(path)) if path == PathBuf::from("missing.wav")
        ));
    }

    #[tokio::test]
    async fn writes_visible_events_without_requiring_audio_output_dir() {
        let events = vec![ChatStreamEvent::Message(ChatMessage {
            role: Role::Assistant,
            phase: Phase::Final,
            sequence: 1,
            message_id: Uuid::nil(),
            turn_id: Uuid::nil(),
            text: "Antwort".to_owned(),
            audio_base64: Some("bXAz".to_owned()),
        })];
        let mut output = Vec::new();

        let saved = write_events(&events, None, &mut output).await.unwrap();

        assert!(saved.is_empty());
        let output = String::from_utf8(output).unwrap();
        assert!(output.contains("assistant final: Antwort"));
        assert!(!output.contains("audio saved:"));
    }

    #[tokio::test]
    async fn writes_visible_events_and_decoded_audio_segments() {
        let temp_dir = tempfile::tempdir().unwrap();
        let events = vec![
            ChatStreamEvent::Message(ChatMessage {
                role: Role::User,
                phase: Phase::Input,
                sequence: 0,
                message_id: Uuid::nil(),
                turn_id: Uuid::nil(),
                text: "Hallo".to_owned(),
                audio_base64: None,
            }),
            ChatStreamEvent::Message(ChatMessage {
                role: Role::Assistant,
                phase: Phase::Final,
                sequence: 1,
                message_id: Uuid::nil(),
                turn_id: Uuid::nil(),
                text: "Antwort".to_owned(),
                audio_base64: Some("bXAz".to_owned()),
            }),
            ChatStreamEvent::Interrupted,
            ChatStreamEvent::Done,
        ];
        let mut output = Vec::new();

        let saved = write_events(&events, Some(temp_dir.path()), &mut output)
            .await
            .unwrap();

        assert_eq!(saved.len(), 1);
        assert_eq!(std::fs::read(&saved[0]).unwrap(), b"mp3");
        let output = String::from_utf8(output).unwrap();
        assert!(output.contains("user input: Hallo"));
        assert!(output.contains("assistant final: Antwort"));
        assert!(output.contains("audio saved:"));
        assert!(output.contains("interrupted"));
        assert!(output.contains("done"));
    }

    #[tokio::test]
    async fn returns_error_when_stream_contains_backend_error_event() {
        let events = vec![ChatStreamEvent::Error(ChatError {
            code: ChatErrorCode::TtsFailed,
            message: "Text-to-speech failed".to_owned(),
        })];
        let mut output = Vec::new();

        let result = write_events(&events, None, &mut output).await;

        assert!(matches!(
            result,
            Err(CliError::StreamEvent {
                code: ChatErrorCode::TtsFailed,
                message,
            }) if message == "Text-to-speech failed"
        ));
        let output = String::from_utf8(output).unwrap();
        assert!(output.contains("error TtsFailed: Text-to-speech failed"));
    }
}
