# binance_trading_bot

Codex project initialized in WSL at /home/devuser/binance_trading_bot.

## Windows Development Setup

This workspace is configured for local Windows development in VS Code.

Recommended prerequisites:

1. Visual Studio 2022 Build Tools or Visual Studio Community with the C++ workload.
2. CMake 3.23 or newer.
3. Ninja, if you want to use the Ninja preset.
4. Git.

Open the repository in VS Code and choose one of the CMake presets:

1. `windows-msvc-debug` for the Visual Studio generator.
2. `windows-ninja-debug` for a single-config Ninja build.

The workspace also includes ready-made VS Code tasks for configure, build, and test, plus a debug launch configuration for the main executable.
