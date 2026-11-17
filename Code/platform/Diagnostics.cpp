#include "Diagnostics.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace platform
{
namespace
{

DiagnosticSink g_diagnosticSink = nullptr;
DiagnosticSink g_statusSink = nullptr;

void WriteLine(const char *line)
{
	std::fputs(line != nullptr ? line : "", stderr);
	std::fputc('\n', stderr);
	std::fflush(stderr);
}

std::string FormatLine(const char *message, const char *caption)
{
	std::string line;

	if (caption != nullptr && caption[0] != '\0')
	{
		line += caption;
		line += ": ";
	}

	line += message != nullptr ? message : "";
	return line;
}

void Emit(const char *message, const char *caption)
{
	const std::string line = FormatLine(message, caption);

	WriteLine(line.c_str());

	if (g_diagnosticSink != nullptr)
		g_diagnosticSink(line.c_str());
}

} // namespace

void SetDiagnosticSink(DiagnosticSink sink)
{
	g_diagnosticSink = sink;
}

void SetStatusSink(DiagnosticSink sink)
{
	g_statusSink = sink;
}

int ReportWarning(const char *message, const char *caption)
{
	Emit(message, caption);
	return 1; // IDOK
}

void ReportFatal(const char *message, const char *caption)
{
	Emit(message, caption);
	std::exit(EXIT_FAILURE);
}

void PublishStatus(const char *line)
{
	if (g_statusSink != nullptr)
	{
		g_statusSink(line != nullptr ? line : "");
		return;
	}

	WriteLine(line);
}

bool StandardInputIsTerminal()
{
#ifdef _WIN32
	return _isatty(_fileno(stdin)) != 0;
#else
	return isatty(fileno(stdin)) != 0;
#endif
}

bool StandardOutputIsTerminal()
{
#ifdef _WIN32
	return _isatty(_fileno(stdout)) != 0;
#else
	return isatty(fileno(stdout)) != 0;
#endif
}

} // namespace platform
