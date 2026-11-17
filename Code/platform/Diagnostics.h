#ifndef _PLATFORM_DIAGNOSTICS_H_
#define _PLATFORM_DIAGNOSTICS_H_

namespace platform
{

// Receives diagnostic and status lines. The server registers a sink that
// writes to its log; without a sink the line goes to standard error.
using DiagnosticSink = void (*)(const char *line);

void SetDiagnosticSink(DiagnosticSink sink);
void SetStatusSink(DiagnosticSink sink);

// Non-fatal warning: log the line and continue.
int ReportWarning(const char *message, const char *caption);

// Fatal initialization failure: log the line, write it to standard error and
// terminate with a non-zero exit code.
[[noreturn]] void ReportFatal(const char *message, const char *caption);

// Status text that used to be pushed to the window caption.
void PublishStatus(const char *line);

// True when standard input is an interactive terminal. The headless hosts use
// this to decide whether to start the console command reader.
bool StandardInputIsTerminal();

// True when standard output is an interactive terminal. Used to decide whether
// status/log lines are echoed to the console.
bool StandardOutputIsTerminal();

} // namespace platform

#endif
