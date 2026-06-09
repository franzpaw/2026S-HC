use std::fmt;
use std::path::{Path, PathBuf};
use std::time::Duration;

use futures_util::{StreamExt, pin_mut};
use siri_client::{
    AudioFileInput, ChatInput, ChatMessage, ChatRequestOptions, ChatStreamEvent, Client, Phase,
    Role, SiriClientError,
};
use tokio::sync::oneshot;
use tokio::time::{sleep, timeout};

const CONTEXT_FIXTURE: &str = "pigeoncode-context-question.wav";
const WEATHER_FIXTURE: &str = "dieblichberg-weather-question.wav";

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SmokeConfig {
    pub fixture_dir: PathBuf,
    pub auth_file: PathBuf,
    pub timeout_seconds: u64,
    pub barge_in_delay_millis: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SmokePreflight {
    pub context_audio: PathBuf,
    pub weather_audio: PathBuf,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StreamSummary {
    pub final_text: String,
    pub commentary_text: String,
    pub commentary_count: usize,
    pub user_text: Option<String>,
    pub assistant_audio_segments: usize,
    pub saw_done: bool,
    pub saw_interrupted: bool,
}

impl StreamSummary {
    fn empty() -> Self {
        Self {
            final_text: String::new(),
            commentary_text: String::new(),
            commentary_count: 0,
            user_text: None,
            assistant_audio_segments: 0,
            saw_done: false,
            saw_interrupted: false,
        }
    }
}

#[derive(Debug, thiserror::Error)]
pub enum SmokeError {
    #[error("OAuth auth file is missing: {0}")]
    MissingAuthFile(PathBuf),
    #[error("smoke fixture is missing: {0}")]
    MissingFixture(PathBuf),
    #[error("smoke step timed out after {seconds}s: {step}")]
    Timeout { step: &'static str, seconds: u64 },
    #[error("backend/client error during {step}: {source}")]
    Client {
        step: &'static str,
        #[source]
        source: SiriClientError,
    },
    #[error("backend emitted error event during {step}: {message}")]
    BackendEvent { step: &'static str, message: String },
    #[error("smoke assertion failed during {step}: {message}")]
    Assertion { step: &'static str, message: String },
    #[error("overlap task failed: {0}")]
    Join(String),
}

pub fn preflight(config: &SmokeConfig) -> Result<SmokePreflight, SmokeError> {
    if !config.auth_file.is_file() {
        return Err(SmokeError::MissingAuthFile(config.auth_file.clone()));
    }

    let context_audio = require_fixture(&config.fixture_dir, CONTEXT_FIXTURE)?;
    let weather_audio = require_fixture(&config.fixture_dir, WEATHER_FIXTURE)?;

    Ok(SmokePreflight {
        context_audio,
        weather_audio,
    })
}

fn require_fixture(fixture_dir: &Path, filename: &str) -> Result<PathBuf, SmokeError> {
    let path = fixture_dir.join(filename);
    if path.is_file() {
        Ok(path)
    } else {
        Err(SmokeError::MissingFixture(path))
    }
}

pub async fn run_smoke(client: Client, config: SmokeConfig) -> Result<(), SmokeError> {
    let preflight = preflight(&config)?;
    let timeout_duration = Duration::from_secs(config.timeout_seconds);

    println!("smoke: health");
    with_timeout("health", config.timeout_seconds, client.health()).await?;

    println!("smoke: reset");
    with_timeout("reset", config.timeout_seconds, client.reset_session()).await?;

    println!("smoke: text pigeoncode");
    let context_summary = stream_text(
        &client,
        "text pigeoncode",
        text_pigeoncode_prompt(),
        true,
        timeout_duration,
        config.timeout_seconds,
        None,
    )
    .await?;
    verify_context_read(&context_summary)?;
    verify_audio_present("text pigeoncode", &context_summary)?;
    println!(
        "smoke: text pigeoncode ok final={:?}",
        context_summary.final_text
    );

    println!("smoke: voice pigeoncode");
    let voice_summary = stream_audio(
        &client,
        "voice pigeoncode",
        preflight.context_audio,
        true,
        timeout_duration,
        config.timeout_seconds,
    )
    .await?;
    verify_voice_context(&voice_summary)?;
    verify_audio_present("voice pigeoncode", &voice_summary)?;
    println!(
        "smoke: voice pigeoncode ok user={:?} final={:?}",
        voice_summary.user_text, voice_summary.final_text
    );

    println!("smoke: weather/current info");
    let weather_summary = stream_audio(
        &client,
        "weather",
        preflight.weather_audio,
        true,
        timeout_duration,
        config.timeout_seconds,
    )
    .await?;
    verify_weather(&weather_summary)?;
    verify_audio_present("weather", &weather_summary)?;
    println!("smoke: weather ok final={:?}", weather_summary.final_text);

    println!("smoke: overlap interruption");
    run_overlap_smoke(
        client,
        timeout_duration,
        config.timeout_seconds,
        Duration::from_millis(config.barge_in_delay_millis),
    )
    .await?;
    println!("smoke: overlap ok");

    println!("Phase 1.4 Rust real-provider smoke passed");
    Ok(())
}

async fn with_timeout<T>(
    step: &'static str,
    seconds: u64,
    future: impl std::future::Future<Output = Result<T, SiriClientError>>,
) -> Result<T, SmokeError> {
    timeout(Duration::from_secs(seconds), future)
        .await
        .map_err(|_| SmokeError::Timeout { step, seconds })?
        .map_err(|source| SmokeError::Client { step, source })
}

async fn stream_text(
    client: &Client,
    step: &'static str,
    text: String,
    tts_enabled: bool,
    timeout_duration: Duration,
    timeout_seconds: u64,
    notify_first_event: Option<oneshot::Sender<()>>,
) -> Result<StreamSummary, SmokeError> {
    collect_stream(
        client,
        step,
        ChatRequestOptions {
            input: ChatInput::Text(text),
            tts_enabled,
        },
        timeout_duration,
        timeout_seconds,
        notify_first_event,
    )
    .await
}

async fn stream_audio(
    client: &Client,
    step: &'static str,
    path: PathBuf,
    tts_enabled: bool,
    timeout_duration: Duration,
    timeout_seconds: u64,
) -> Result<StreamSummary, SmokeError> {
    let filename = path
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or("audio.wav")
        .to_owned();
    collect_stream(
        client,
        step,
        ChatRequestOptions {
            input: ChatInput::AudioFile(AudioFileInput {
                path,
                filename,
                content_type: "audio/wav".to_owned(),
            }),
            tts_enabled,
        },
        timeout_duration,
        timeout_seconds,
        None,
    )
    .await
}

async fn collect_stream(
    client: &Client,
    step: &'static str,
    options: ChatRequestOptions,
    timeout_duration: Duration,
    timeout_seconds: u64,
    mut notify_first_event: Option<oneshot::Sender<()>>,
) -> Result<StreamSummary, SmokeError> {
    timeout(timeout_duration, async {
        let events = client
            .chat_stream_events(options)
            .await
            .map_err(|source| SmokeError::Client { step, source })?;
        pin_mut!(events);
        let mut summary = StreamSummary::empty();
        while let Some(event) = events.next().await {
            let event = event.map_err(|source| SmokeError::Client { step, source })?;
            if let Some(sender) = notify_first_event.take() {
                let _ = sender.send(());
            }
            apply_event(step, &mut summary, event)?;
        }
        Ok(summary)
    })
    .await
    .map_err(|_| SmokeError::Timeout {
        step,
        seconds: timeout_seconds,
    })?
}

fn apply_event(
    step: &'static str,
    summary: &mut StreamSummary,
    event: ChatStreamEvent,
) -> Result<(), SmokeError> {
    match event {
        ChatStreamEvent::Message(message) => apply_message(step, summary, message),
        ChatStreamEvent::Error(error) => Err(SmokeError::BackendEvent {
            step,
            message: format!("{:?}: {}", error.code, error.message),
        }),
        ChatStreamEvent::Interrupted => {
            summary.saw_interrupted = true;
            Ok(())
        }
        ChatStreamEvent::Done => {
            summary.saw_done = true;
            Ok(())
        }
    }
}

fn apply_message(
    step: &'static str,
    summary: &mut StreamSummary,
    message: ChatMessage,
) -> Result<(), SmokeError> {
    let decoded_audio = message
        .decoded_audio()
        .map_err(|source| SmokeError::Client { step, source })?;
    if message.role == Role::Assistant && decoded_audio.is_some() {
        let audio = decoded_audio.expect("checked above");
        if audio.is_empty() {
            return assertion(step, "assistant audio segment was empty".to_owned());
        }
        summary.assistant_audio_segments += 1;
    }
    match (message.role, message.phase) {
        (Role::User, Phase::Input) => summary.user_text = Some(normalize_space(&message.text)),
        (Role::Assistant, Phase::Commentary) => {
            summary.commentary_count += 1;
            push_text(&mut summary.commentary_text, &message.text);
        }
        (Role::Assistant, Phase::Final) => push_text(&mut summary.final_text, &message.text),
        _ => {}
    }
    Ok(())
}

pub fn verify_context_read(summary: &StreamSummary) -> Result<(), SmokeError> {
    require_done_and_final("context", summary)?;
    if !summary.final_text.contains("36") {
        return assertion(
            "context",
            format!(
                "final answer did not contain pigeoncode 36: {:?}",
                summary.final_text
            ),
        );
    }
    Ok(())
}

pub fn verify_voice_context(summary: &StreamSummary) -> Result<(), SmokeError> {
    verify_context_read(summary)?;
    let user = summary
        .user_text
        .as_deref()
        .unwrap_or_default()
        .to_ascii_lowercase();
    if !(user.contains("pigeon") || user.contains("code") || user.contains("36")) {
        return assertion(
            "voice pigeoncode",
            format!("user transcript does not look like the fixture prompt: {user:?}"),
        );
    }
    Ok(())
}

pub fn verify_weather(summary: &StreamSummary) -> Result<(), SmokeError> {
    require_done_and_final("weather", summary)?;
    if summary.commentary_count < 1 {
        return assertion(
            "weather",
            "expected at least one commentary event".to_owned(),
        );
    }
    let commentary = summary.commentary_text.to_ascii_lowercase();
    let search_words = [
        "such",
        "web",
        "internet",
        "aktuell",
        "pruef",
        "prüf",
        "wetterdaten",
    ];
    if !search_words.iter().any(|word| commentary.contains(word)) {
        return assertion(
            "weather",
            format!(
                "commentary did not mention web/search/current lookup: {:?}",
                summary.commentary_text
            ),
        );
    }
    let final_text = summary.final_text.to_ascii_lowercase();
    if !final_text.contains("dieblich") {
        return assertion(
            "weather",
            format!(
                "final answer did not mention Dieblichberg/Dieblich: {:?}",
                summary.final_text
            ),
        );
    }
    let weather_words = [
        "wetter",
        "grad",
        "temperatur",
        "regen",
        "wind",
        "bewoelkt",
        "bewölkt",
        "sonnig",
        "wolken",
    ];
    if !weather_words.iter().any(|word| final_text.contains(word)) {
        return assertion(
            "weather",
            format!(
                "final answer does not look like weather: {:?}",
                summary.final_text
            ),
        );
    }
    let refusal_words = [
        "keine live",
        "nicht auf aktuelle",
        "kann nicht",
        "keinen zugriff",
        "unable",
        "cannot",
    ];
    if refusal_words.iter().any(|word| final_text.contains(word)) {
        return assertion(
            "weather",
            format!(
                "final answer looks like a no-live-data refusal: {:?}",
                summary.final_text
            ),
        );
    }
    Ok(())
}

pub fn verify_audio_present(step: &'static str, summary: &StreamSummary) -> Result<(), SmokeError> {
    if summary.assistant_audio_segments == 0 {
        return assertion(
            step,
            "expected at least one decodable assistant audio segment".to_owned(),
        );
    }
    Ok(())
}

pub fn verify_overlap(old: &StreamSummary, new: &StreamSummary) -> Result<(), SmokeError> {
    if !old.saw_interrupted {
        return assertion("overlap", "old stream did not emit interrupted".to_owned());
    }
    verify_context_read(new)
}

async fn run_overlap_smoke(
    client: Client,
    timeout_duration: Duration,
    timeout_seconds: u64,
    barge_in_delay: Duration,
) -> Result<(), SmokeError> {
    let old_client = client.clone();
    let (old_started_sender, old_started_receiver) = oneshot::channel();
    let old_task = tokio::spawn(async move {
        stream_text(
            &old_client,
            "overlap old",
            long_text_prompt(),
            false,
            timeout_duration,
            timeout_seconds,
            Some(old_started_sender),
        )
        .await
    });

    timeout(
        Duration::from_secs(timeout_seconds.min(30)),
        old_started_receiver,
    )
    .await
    .map_err(|_| SmokeError::Timeout {
        step: "overlap old start",
        seconds: timeout_seconds.min(30),
    })?
    .map_err(|_| SmokeError::Assertion {
        step: "overlap old start",
        message: "old stream finished before emitting an initial event".to_owned(),
    })?;
    sleep(barge_in_delay).await;

    let new_summary = stream_text(
        &client,
        "overlap new",
        text_pigeoncode_prompt(),
        false,
        timeout_duration,
        timeout_seconds,
        None,
    )
    .await?;
    let old_summary = old_task
        .await
        .map_err(|error| SmokeError::Join(error.to_string()))??;
    verify_overlap(&old_summary, &new_summary)
}

fn require_done_and_final(step: &'static str, summary: &StreamSummary) -> Result<(), SmokeError> {
    if !summary.saw_done {
        return assertion(step, "stream did not emit done".to_owned());
    }
    if summary.final_text.trim().is_empty() {
        return assertion(step, "stream did not emit final text".to_owned());
    }
    Ok(())
}

fn push_text(target: &mut String, text: &str) {
    if !target.is_empty() {
        target.push(' ');
    }
    target.push_str(&normalize_space(text));
}

fn normalize_space(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}

fn assertion<T>(step: &'static str, message: String) -> Result<T, SmokeError> {
    Err(SmokeError::Assertion { step, message })
}

fn text_pigeoncode_prompt() -> String {
    "Lies die lokale Kontextdatei und nenne den pigeoncode. Antworte nur mit der Zahl.".to_owned()
}

fn long_text_prompt() -> String {
    "Schreibe eine sehr lange, strukturierte Antwort mit vielen Abschnitten ueber die Architektur von siri-v2. Beginne sofort und schreibe ausfuehrlich, damit diese Anfrage waehrend des Smoke-Tests durch eine neue Anfrage unterbrochen werden kann.".to_owned()
}

impl fmt::Display for StreamSummary {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "user={:?}, commentary_count={}, final={:?}, audio_segments={}, done={}, interrupted={}",
            self.user_text,
            self.commentary_count,
            self.final_text,
            self.assistant_audio_segments,
            self.saw_done,
            self.saw_interrupted
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn preflight_reports_missing_auth_file_before_fixtures() {
        let temp_dir = tempfile::tempdir().unwrap();
        let config = SmokeConfig {
            fixture_dir: temp_dir.path().join("fixtures"),
            auth_file: temp_dir.path().join("auth.json"),
            timeout_seconds: 120,
            barge_in_delay_millis: 500,
        };

        assert!(matches!(
            preflight(&config),
            Err(SmokeError::MissingAuthFile(path)) if path.ends_with("auth.json")
        ));
    }

    #[test]
    fn preflight_resolves_required_fixture_paths() {
        let temp_dir = tempfile::tempdir().unwrap();
        let fixture_dir = temp_dir.path().join("fixtures");
        std::fs::create_dir_all(&fixture_dir).unwrap();
        let auth_file = temp_dir.path().join("auth.json");
        std::fs::write(&auth_file, b"{}").unwrap();
        std::fs::write(fixture_dir.join(CONTEXT_FIXTURE), b"wav").unwrap();
        std::fs::write(fixture_dir.join(WEATHER_FIXTURE), b"wav").unwrap();

        let preflight = preflight(&SmokeConfig {
            fixture_dir: fixture_dir.clone(),
            auth_file,
            timeout_seconds: 120,
            barge_in_delay_millis: 500,
        })
        .unwrap();

        assert_eq!(preflight.context_audio, fixture_dir.join(CONTEXT_FIXTURE));
        assert_eq!(preflight.weather_audio, fixture_dir.join(WEATHER_FIXTURE));
    }

    #[test]
    fn verifies_context_final_answer_contains_pigeoncode() {
        let summary = StreamSummary {
            final_text: "Der pigeoncode ist 36.".to_owned(),
            saw_done: true,
            ..StreamSummary::empty()
        };

        verify_context_read(&summary).unwrap();
    }

    #[test]
    fn rejects_context_answer_without_pigeoncode() {
        let summary = StreamSummary {
            final_text: "Ich weiss es nicht.".to_owned(),
            saw_done: true,
            ..StreamSummary::empty()
        };

        assert!(matches!(
            verify_context_read(&summary),
            Err(SmokeError::Assertion {
                step: "context",
                ..
            })
        ));
    }

    #[test]
    fn verifies_weather_needs_commentary_current_lookup_and_weather_answer() {
        let summary = StreamSummary {
            final_text: "In Dieblichberg ist das Wetter bewoelkt bei 12 Grad.".to_owned(),
            commentary_text: "Ich pruefe aktuelle Wetterdaten im Web.".to_owned(),
            commentary_count: 1,
            saw_done: true,
            ..StreamSummary::empty()
        };

        verify_weather(&summary).unwrap();
    }

    #[test]
    fn verifies_overlap_requires_old_interrupted_and_new_context_answer() {
        let old = StreamSummary {
            saw_interrupted: true,
            ..StreamSummary::empty()
        };
        let new = StreamSummary {
            final_text: "36".to_owned(),
            saw_done: true,
            ..StreamSummary::empty()
        };

        verify_overlap(&old, &new).unwrap();
    }

    #[test]
    fn rejects_empty_assistant_audio_segment() {
        let mut summary = StreamSummary::empty();
        let result = apply_message(
            "audio",
            &mut summary,
            ChatMessage {
                role: Role::Assistant,
                phase: Phase::Final,
                sequence: 1,
                message_id: uuid::Uuid::nil(),
                turn_id: uuid::Uuid::nil(),
                text: "Antwort".to_owned(),
                audio_base64: Some(String::new()),
            },
        );

        assert!(matches!(
            result,
            Err(SmokeError::Assertion { step: "audio", message }) if message == "assistant audio segment was empty"
        ));
    }

    #[test]
    fn verifies_audio_presence() {
        let summary = StreamSummary {
            assistant_audio_segments: 1,
            ..StreamSummary::empty()
        };

        verify_audio_present("audio", &summary).unwrap();
    }
}
