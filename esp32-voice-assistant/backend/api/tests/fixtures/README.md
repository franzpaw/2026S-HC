# Voice Smoke Fixtures

Reusable WAV fixtures for real-provider chat stream smoke tests.

- `dieblichberg-weather-question.wav`: German weather/web-search prompt for Dieblichberg.
- `pigeoncode-context-question.wav`: asks Pi to read local context and answer with the `pigeoncode`.
- `barge-in-long-task.wav`: starts a deliberately long task so a second stream can interrupt it.
- `barge-in-pigeoncode-question.wav`: second overlapping prompt; asks for the `pigeoncode` after interruption.

These are committed test assets. Do not place OAuth files, generated responses, or private recordings in this directory.
