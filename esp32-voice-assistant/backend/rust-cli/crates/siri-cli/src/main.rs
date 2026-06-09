use clap::Parser;
use futures_util::{TryStreamExt, pin_mut};
use siri_cli::{Cli, CliError, Command, chat_options_from_cli, smoke, write_events};
use siri_client::Client;

#[tokio::main]
async fn main() {
    dotenvy::dotenv().ok();
    let cli = Cli::parse();
    if let Err(error) = run(cli).await {
        eprintln!("error: {error}");
        std::process::exit(1);
    }
}

async fn run(cli: Cli) -> Result<(), CliError> {
    let client = Client::new(&cli.backend_url, &cli.token)?;
    match &cli.command {
        Command::Health => {
            client.health().await?;
            println!("backend ready: {}", cli.backend_url);
        }
        Command::Reset => {
            client.reset_session().await?;
            println!("session reset");
        }
        Command::Chat(_) => {
            let options =
                chat_options_from_cli(&cli)?.expect("chat command should produce options");
            let events = client.chat_stream_events(options).await?;
            pin_mut!(events);
            let mut stdout = std::io::stdout();
            while let Some(event) = events.try_next().await? {
                write_events(&[event], cli.audio_output_dir.as_deref(), &mut stdout).await?;
            }
        }
        Command::Smoke(command) => {
            smoke::run_smoke(
                client,
                smoke::SmokeConfig {
                    fixture_dir: command.fixture_dir.clone(),
                    auth_file: command.auth_file.clone(),
                    timeout_seconds: command.timeout_seconds,
                    barge_in_delay_millis: command.barge_in_delay_millis,
                },
            )
            .await
            .map_err(|error| CliError::Smoke(error.to_string()))?;
        }
    }
    Ok(())
}
