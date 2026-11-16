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
// The owner and flags parameters preserve the legacy dialog call shape.
int ReportWarning(void *owner, const char *message, const char *caption,
                  unsigned int flags);

// Fatal initialization failure: log the line, write it to standard error and
// terminate with a non-zero exit code.
[[noreturn]] void ReportFatal(void *owner, const char *message,
                              const char *caption, unsigned int flags);

// Status text that used to be pushed to the window caption.
void PublishStatus(const char *line);

} // namespace platform

#endif
