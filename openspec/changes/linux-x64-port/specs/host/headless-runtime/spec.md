# Spec Delta

## ADDED Requirements

### Requirement: POSIX terminal and signal integration

On Linux the runtime SHALL detect whether stdin is a terminal, start the console reader only for a TTY or when the force-console option is set, and route SIGINT and SIGTERM through the same graceful shutdown path as the console quit command. It SHALL not require a Windows console or desktop session.

#### Scenario: Interactive console on Linux

- **WHEN** the Linux server starts with stdin attached to a terminal
- **THEN** it reads console commands and executes them on the event-loop thread in submission order

#### Scenario: Piped console commands

- **WHEN** stdin is a pipe and the force-console option is set
- **THEN** the server reads and executes commands from the pipe

#### Scenario: Interrupt from the terminal

- **WHEN** a Linux server receives SIGINT
- **THEN** it stops accepting, performs the graceful shutdown writes and exits successfully

#### Scenario: Termination from a service manager

- **WHEN** a Linux server receives SIGTERM
- **THEN** it follows the same graceful shutdown path as SIGINT and the quit command

#### Scenario: No interactive input

- **WHEN** stdin is not a terminal and the force-console option is unset
- **THEN** the console reader is not started and the server continues serving network traffic
