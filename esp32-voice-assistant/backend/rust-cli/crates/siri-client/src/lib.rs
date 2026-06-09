use std::path::PathBuf;

use base64::Engine;
use futures_core::Stream;
use futures_util::{StreamExt, TryStreamExt, stream};
use reqwest::multipart;
use serde::Deserialize;
use url::Url;
use uuid::Uuid;

#[derive(Debug, Clone)]
pub struct Client {
    config: ClientConfig,
    http: reqwest::Client,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClientConfig {
    pub base_url: Url,
    pub token: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ChatInput {
    Text(String),
    AudioFile(AudioFileInput),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AudioFileInput {
    pub path: PathBuf,
    pub filename: String,
    pub content_type: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChatRequestOptions {
    pub input: ChatInput,
    pub tts_enabled: bool,
}

const SUPPORTED_AUDIO_TYPES: &[&str] = &[
    "audio/flac",
    "audio/m4a",
    "audio/mp3",
    "audio/mp4",
    "audio/mpeg",
    "audio/mpga",
    "audio/ogg",
    "audio/wav",
    "audio/wave",
    "audio/webm",
    "audio/x-m4a",
    "audio/x-wav",
    "video/mp4",
    "video/webm",
];

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ChatStreamEvent {
    Message(ChatMessage),
    Error(ChatError),
    Interrupted,
    Done,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Role {
    User,
    Assistant,
}

#[derive(Debug, Clone, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Phase {
    Input,
    Commentary,
    Final,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChatMessage {
    pub role: Role,
    pub phase: Phase,
    pub sequence: u32,
    pub message_id: Uuid,
    pub turn_id: Uuid,
    pub text: String,
    pub audio_base64: Option<String>,
}

impl ChatMessage {
    pub fn decoded_audio(&self) -> Result<Option<Vec<u8>>, SiriClientError> {
        match &self.audio_base64 {
            Some(audio) => base64::engine::general_purpose::STANDARD
                .decode(audio)
                .map(Some)
                .map_err(|_| SiriClientError::InvalidAudioBase64),
            None => Ok(None),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ChatError {
    pub code: ChatErrorCode,
    pub message: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ChatErrorCode {
    InvalidChatInput,
    SttFailed,
    SttTimeout,
    SttInvalidTranscript,
    TtsFailed,
    TtsTimeout,
    AgentFailed,
    AgentTimeout,
    Unknown(String),
}

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum SiriClientError {
    #[error("backend URL must not be empty")]
    EmptyBaseUrl,
    #[error("backend token must not be empty")]
    EmptyToken,
    #[error("text input is required")]
    EmptyTextInput,
    #[error("audio filename is required")]
    EmptyAudioFilename,
    #[error("audio content type is required")]
    EmptyAudioContentType,
    #[error("unsupported audio content type: {0}")]
    UnsupportedAudioContentType(String),
    #[error("invalid SSE event")]
    InvalidSseEvent,
    #[error("invalid SSE JSON: {0}")]
    InvalidSseJson(String),
    #[error("invalid SSE UTF-8")]
    InvalidSseUtf8,
    #[error("invalid audio base64")]
    InvalidAudioBase64,
    #[error("HTTP request failed: {0}")]
    Http(String),
    #[error("backend returned HTTP {status}: {body}")]
    BackendStatus { status: u16, body: String },
}

impl Client {
    pub fn new(base_url: &str, token: &str) -> Result<Self, SiriClientError> {
        Ok(Self {
            config: validate_config(base_url, token)?,
            http: reqwest::Client::new(),
        })
    }

    pub fn config(&self) -> &ClientConfig {
        &self.config
    }

    pub async fn health(&self) -> Result<(), SiriClientError> {
        let response = self
            .http
            .get(health_url(&self.config))
            .bearer_auth(&self.config.token)
            .send()
            .await
            .map_err(|error| SiriClientError::Http(error.to_string()))?;
        expect_success(response).await
    }

    pub async fn reset_session(&self) -> Result<(), SiriClientError> {
        let response = self
            .http
            .post(reset_url(&self.config))
            .bearer_auth(&self.config.token)
            .send()
            .await
            .map_err(|error| SiriClientError::Http(error.to_string()))?;
        expect_success(response).await
    }

    pub async fn chat_stream_events(
        &self,
        options: ChatRequestOptions,
    ) -> Result<impl Stream<Item = Result<ChatStreamEvent, SiriClientError>>, SiriClientError> {
        validate_request(&options)?;
        let response = self
            .http
            .post(chat_stream_url(&self.config))
            .bearer_auth(&self.config.token)
            .multipart(chat_multipart(options).await?)
            .send()
            .await
            .map_err(|error| SiriClientError::Http(error.to_string()))?;
        let status = response.status();
        if !status.is_success() {
            let body = response.text().await.unwrap_or_default();
            return Err(SiriClientError::BackendStatus {
                status: status.as_u16(),
                body,
            });
        }
        Ok(stream_sse_events(response.bytes_stream()))
    }

    pub async fn chat_stream(
        &self,
        options: ChatRequestOptions,
    ) -> Result<Vec<ChatStreamEvent>, SiriClientError> {
        self.chat_stream_events(options).await?.try_collect().await
    }
}

pub fn validate_config(base_url: &str, token: &str) -> Result<ClientConfig, SiriClientError> {
    if base_url.trim().is_empty() {
        return Err(SiriClientError::EmptyBaseUrl);
    }
    if token.trim().is_empty() {
        return Err(SiriClientError::EmptyToken);
    }
    let base_url = Url::parse(base_url.trim()).map_err(|_| SiriClientError::EmptyBaseUrl)?;
    Ok(ClientConfig {
        base_url,
        token: token.to_owned(),
    })
}

pub fn chat_stream_url(config: &ClientConfig) -> Url {
    config
        .base_url
        .join("/chat/stream")
        .expect("absolute path should join")
}

pub fn reset_url(config: &ClientConfig) -> Url {
    config
        .base_url
        .join("/voice/session/reset")
        .expect("absolute path should join")
}

pub fn health_url(config: &ClientConfig) -> Url {
    config
        .base_url
        .join("/health")
        .expect("absolute path should join")
}

pub fn validate_request(options: &ChatRequestOptions) -> Result<(), SiriClientError> {
    match &options.input {
        ChatInput::Text(text) if text.trim().is_empty() => Err(SiriClientError::EmptyTextInput),
        ChatInput::AudioFile(audio) if audio.filename.trim().is_empty() => {
            Err(SiriClientError::EmptyAudioFilename)
        }
        ChatInput::AudioFile(audio) if audio.content_type.trim().is_empty() => {
            Err(SiriClientError::EmptyAudioContentType)
        }
        ChatInput::AudioFile(audio) => validate_audio_content_type(&audio.content_type),
        _ => Ok(()),
    }
}

pub fn validate_audio_content_type(content_type: &str) -> Result<(), SiriClientError> {
    let content_type_base = content_type
        .split(';')
        .next()
        .unwrap_or_default()
        .trim()
        .to_ascii_lowercase();
    if SUPPORTED_AUDIO_TYPES.contains(&content_type_base.as_str()) {
        Ok(())
    } else {
        Err(SiriClientError::UnsupportedAudioContentType(
            content_type.to_owned(),
        ))
    }
}

pub fn parse_sse_events(input: &str) -> Result<Vec<ChatStreamEvent>, SiriClientError> {
    input
        .split("\n\n")
        .filter(|block| !block.trim().is_empty())
        .map(parse_sse_block)
        .collect()
}

pub fn stream_sse_events<S>(
    byte_stream: S,
) -> impl Stream<Item = Result<ChatStreamEvent, SiriClientError>>
where
    S: Stream<Item = Result<bytes::Bytes, reqwest::Error>> + Unpin,
{
    stream::try_unfold(
        (byte_stream, String::new()),
        |(mut byte_stream, mut buffer)| async move {
            loop {
                if let Some(boundary) = buffer.find("\n\n") {
                    let block = buffer[..boundary].to_owned();
                    buffer.drain(..boundary + 2);
                    if block.trim().is_empty() {
                        continue;
                    }
                    return parse_sse_block(&block)
                        .map(|event| Some((event, (byte_stream, buffer))));
                }

                match byte_stream.next().await {
                    Some(Ok(chunk)) => {
                        let text = std::str::from_utf8(&chunk)
                            .map_err(|_| SiriClientError::InvalidSseUtf8)?;
                        buffer.push_str(text);
                    }
                    Some(Err(error)) => return Err(SiriClientError::Http(error.to_string())),
                    None if buffer.trim().is_empty() => return Ok(None),
                    None => {
                        let event = parse_sse_block(&buffer)?;
                        return Ok(Some((event, (byte_stream, String::new()))));
                    }
                }
            }
        },
    )
}

async fn chat_multipart(options: ChatRequestOptions) -> Result<multipart::Form, SiriClientError> {
    let form = multipart::Form::new().text("tts_enabled", options.tts_enabled.to_string());
    match options.input {
        ChatInput::Text(text) => Ok(form.text("text", text)),
        ChatInput::AudioFile(audio) => {
            let bytes = tokio::fs::read(&audio.path)
                .await
                .map_err(|error| SiriClientError::Http(error.to_string()))?;
            let part = multipart::Part::bytes(bytes)
                .file_name(audio.filename)
                .mime_str(&audio.content_type)
                .map_err(|error| SiriClientError::Http(error.to_string()))?;
            Ok(form.part("audio", part))
        }
    }
}

async fn expect_success(response: reqwest::Response) -> Result<(), SiriClientError> {
    let status = response.status();
    if status.is_success() {
        return Ok(());
    }
    let body = response.text().await.unwrap_or_default();
    Err(SiriClientError::BackendStatus {
        status: status.as_u16(),
        body,
    })
}

fn parse_sse_block(block: &str) -> Result<ChatStreamEvent, SiriClientError> {
    let mut event_name: Option<&str> = None;
    let mut data_lines: Vec<&str> = Vec::new();

    for line in block.lines() {
        if let Some(value) = line.strip_prefix("event: ") {
            event_name = Some(value.trim());
        } else if let Some(value) = line.strip_prefix("data: ") {
            data_lines.push(value);
        }
    }

    let event_name = event_name.ok_or(SiriClientError::InvalidSseEvent)?;
    let data = data_lines.join("\n");
    match event_name {
        "message" => serde_json::from_str::<MessagePayload>(&data)
            .map(ChatStreamEvent::from)
            .map_err(|error| SiriClientError::InvalidSseJson(error.to_string())),
        "error" => serde_json::from_str::<ErrorPayload>(&data)
            .map(ChatStreamEvent::from)
            .map_err(|error| SiriClientError::InvalidSseJson(error.to_string())),
        "interrupted" => Ok(ChatStreamEvent::Interrupted),
        "done" => Ok(ChatStreamEvent::Done),
        _ => Err(SiriClientError::InvalidSseEvent),
    }
}

#[derive(Debug, Deserialize)]
struct MessagePayload {
    role: Role,
    phase: Phase,
    sequence: u32,
    message_id: Uuid,
    turn_id: Uuid,
    text: String,
    audio_base64: Option<String>,
}

#[derive(Debug, Deserialize)]
struct ErrorPayload {
    error_code: String,
    message: String,
}

impl From<MessagePayload> for ChatStreamEvent {
    fn from(value: MessagePayload) -> Self {
        ChatStreamEvent::Message(ChatMessage {
            role: value.role,
            phase: value.phase,
            sequence: value.sequence,
            message_id: value.message_id,
            turn_id: value.turn_id,
            text: value.text,
            audio_base64: value.audio_base64,
        })
    }
}

impl From<ErrorPayload> for ChatStreamEvent {
    fn from(value: ErrorPayload) -> Self {
        ChatStreamEvent::Error(ChatError {
            code: ChatErrorCode::from_backend_code(&value.error_code),
            message: value.message,
        })
    }
}

impl ChatErrorCode {
    fn from_backend_code(value: &str) -> Self {
        match value {
            "invalid_chat_input" => Self::InvalidChatInput,
            "stt_failed" => Self::SttFailed,
            "stt_timeout" => Self::SttTimeout,
            "stt_invalid_transcript" => Self::SttInvalidTranscript,
            "tts_failed" => Self::TtsFailed,
            "tts_timeout" => Self::TtsTimeout,
            "agent_failed" => Self::AgentFailed,
            "agent_timeout" => Self::AgentTimeout,
            other => Self::Unknown(other.to_owned()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const USER_ID: &str = "11111111-1111-1111-1111-111111111111";
    const TURN_ID: &str = "22222222-2222-2222-2222-222222222222";
    const COMMENTARY_ID: &str = "33333333-3333-3333-3333-333333333333";

    #[test]
    fn parses_message_error_interrupted_and_done_events() {
        let raw = format!(
            "event: message\ndata: {{\"type\":\"message\",\"role\":\"user\",\"phase\":\"input\",\"sequence\":0,\"message_id\":\"{USER_ID}\",\"turn_id\":\"{TURN_ID}\",\"text\":\"hallo\"}}\n\n\
             event: message\ndata: {{\"type\":\"message\",\"role\":\"assistant\",\"phase\":\"commentary\",\"sequence\":1,\"message_id\":\"{COMMENTARY_ID}\",\"turn_id\":\"{TURN_ID}\",\"text\":\"Ich suche.\",\"audio_base64\":\"bXAz\"}}\n\n\
             event: error\ndata: {{\"type\":\"error\",\"error_code\":\"tts_failed\",\"message\":\"Text-to-speech failed\"}}\n\n\
             event: interrupted\ndata: {{\"type\":\"interrupted\"}}\n\n\
             event: done\ndata: {{\"type\":\"done\"}}\n\n"
        );

        let events = parse_sse_events(&raw).expect("SSE should parse");

        assert_eq!(events.len(), 5);
        assert_eq!(
            events[0],
            ChatStreamEvent::Message(ChatMessage {
                role: Role::User,
                phase: Phase::Input,
                sequence: 0,
                message_id: Uuid::parse_str(USER_ID).unwrap(),
                turn_id: Uuid::parse_str(TURN_ID).unwrap(),
                text: "hallo".to_owned(),
                audio_base64: None,
            })
        );
        assert_eq!(
            events[1],
            ChatStreamEvent::Message(ChatMessage {
                role: Role::Assistant,
                phase: Phase::Commentary,
                sequence: 1,
                message_id: Uuid::parse_str(COMMENTARY_ID).unwrap(),
                turn_id: Uuid::parse_str(TURN_ID).unwrap(),
                text: "Ich suche.".to_owned(),
                audio_base64: Some("bXAz".to_owned()),
            })
        );
        assert_eq!(
            events[1].as_message().unwrap().decoded_audio().unwrap(),
            Some(b"mp3".to_vec())
        );
        assert_eq!(
            events[2],
            ChatStreamEvent::Error(ChatError {
                code: ChatErrorCode::TtsFailed,
                message: "Text-to-speech failed".to_owned(),
            })
        );
        assert_eq!(events[3], ChatStreamEvent::Interrupted);
        assert_eq!(events[4], ChatStreamEvent::Done);
    }

    #[test]
    fn validates_config_urls_and_request_options() {
        let config = validate_config("http://localhost:8000/api/ignored", "test-token").unwrap();

        assert_eq!(
            chat_stream_url(&config).as_str(),
            "http://localhost:8000/chat/stream"
        );
        assert_eq!(
            reset_url(&config).as_str(),
            "http://localhost:8000/voice/session/reset"
        );
        assert_eq!(health_url(&config).as_str(), "http://localhost:8000/health");
        assert_eq!(
            validate_config("   ", "test-token"),
            Err(SiriClientError::EmptyBaseUrl)
        );
        assert_eq!(
            validate_config("http://localhost:8000", ""),
            Err(SiriClientError::EmptyToken)
        );

        assert_eq!(
            validate_request(&ChatRequestOptions {
                input: ChatInput::Text("   ".to_owned()),
                tts_enabled: true,
            }),
            Err(SiriClientError::EmptyTextInput)
        );
        assert_eq!(
            validate_request(&ChatRequestOptions {
                input: ChatInput::AudioFile(AudioFileInput {
                    path: PathBuf::from("prompt.wav"),
                    filename: "prompt.wav".to_owned(),
                    content_type: "audio/wav".to_owned(),
                }),
                tts_enabled: false,
            }),
            Ok(())
        );
    }

    #[test]
    fn maps_backend_error_codes_without_losing_unknown_codes() {
        let events = parse_sse_events(
            "event: error\ndata: {\"type\":\"error\",\"error_code\":\"stt_timeout\",\"message\":\"Speech-to-text timed out\"}\n\n\
             event: error\ndata: {\"type\":\"error\",\"error_code\":\"new_backend_code\",\"message\":\"New backend message\"}\n\n",
        )
        .unwrap();

        assert_eq!(
            events,
            vec![
                ChatStreamEvent::Error(ChatError {
                    code: ChatErrorCode::SttTimeout,
                    message: "Speech-to-text timed out".to_owned(),
                }),
                ChatStreamEvent::Error(ChatError {
                    code: ChatErrorCode::Unknown("new_backend_code".to_owned()),
                    message: "New backend message".to_owned(),
                }),
            ]
        );
    }

    #[test]
    fn rejects_invalid_sse_blocks_and_json() {
        assert_eq!(
            parse_sse_events("data: {\"type\":\"done\"}\n\n"),
            Err(SiriClientError::InvalidSseEvent)
        );
        assert!(matches!(
            parse_sse_events("event: message\ndata: {not json}\n\n"),
            Err(SiriClientError::InvalidSseJson(_))
        ));
        assert_eq!(
            parse_sse_events("event: progress\ndata: {\"type\":\"progress\"}\n\n"),
            Err(SiriClientError::InvalidSseEvent)
        );
    }

    #[test]
    fn validates_audio_request_metadata() {
        assert_eq!(
            validate_request(&ChatRequestOptions {
                input: ChatInput::AudioFile(AudioFileInput {
                    path: PathBuf::from("prompt.wav"),
                    filename: "".to_owned(),
                    content_type: "audio/wav".to_owned(),
                }),
                tts_enabled: true,
            }),
            Err(SiriClientError::EmptyAudioFilename)
        );
        assert_eq!(
            validate_request(&ChatRequestOptions {
                input: ChatInput::AudioFile(AudioFileInput {
                    path: PathBuf::from("prompt.wav"),
                    filename: "prompt.wav".to_owned(),
                    content_type: "".to_owned(),
                }),
                tts_enabled: true,
            }),
            Err(SiriClientError::EmptyAudioContentType)
        );
        assert_eq!(
            validate_audio_content_type("audio/webm;codecs=opus"),
            Ok(())
        );
        assert_eq!(
            validate_audio_content_type("text/plain"),
            Err(SiriClientError::UnsupportedAudioContentType(
                "text/plain".to_owned()
            ))
        );
    }

    #[tokio::test]
    async fn streams_events_incrementally_from_byte_chunks() {
        let raw = format!(
            "event: message\ndata: {{\"type\":\"message\",\"role\":\"user\",\"phase\":\"input\",\"sequence\":0,\"message_id\":\"{USER_ID}\",\"turn_id\":\"{TURN_ID}\",\"text\":\"hallo\"}}\n\n\
             event: done\ndata: {{\"type\":\"done\"}}\n\n"
        );
        let chunks = vec![
            Ok(bytes::Bytes::from(raw[..32].to_owned())),
            Ok(bytes::Bytes::from(raw[32..].to_owned())),
        ];

        let events = stream_sse_events(futures_util::stream::iter(chunks))
            .try_collect::<Vec<_>>()
            .await
            .unwrap();

        assert_eq!(events.len(), 2);
        assert_eq!(events[1], ChatStreamEvent::Done);
    }

    #[test]
    fn rejects_invalid_audio_base64_payloads() {
        let message = ChatMessage {
            role: Role::Assistant,
            phase: Phase::Final,
            sequence: 1,
            message_id: Uuid::parse_str(COMMENTARY_ID).unwrap(),
            turn_id: Uuid::parse_str(TURN_ID).unwrap(),
            text: "Antwort.".to_owned(),
            audio_base64: Some("not base64!".to_owned()),
        };

        assert_eq!(
            message.decoded_audio(),
            Err(SiriClientError::InvalidAudioBase64)
        );
    }

    impl ChatStreamEvent {
        fn as_message(&self) -> Option<&ChatMessage> {
            match self {
                ChatStreamEvent::Message(message) => Some(message),
                _ => None,
            }
        }
    }
}
