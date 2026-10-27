/**
 * @file Report.cpp
 * @brief Implementation of Report (see Report.h).
 */
#include "Report.h"

#include <algorithm>
#include <sstream>

namespace probe
{

const char *StatusTag(Status status)
{
    switch (status) {
        case Status::Pass: return "PASS";
        case Status::Warn: return "WARN";
        case Status::Fail: return "FAIL";
        case Status::Info: return "    ";
    }
    return "    ";
}

void Report::Detail(const std::string &text)
{
    // Keep only the first few non-empty lines: compiler logs can be long and the screen is not.
    constexpr int kMaxLines = 6;
    std::istringstream lines(text);
    std::string line;
    int shown = 0;
    while (std::getline(lines, line) && shown < kMaxLines) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        findings_.push_back({ Status::Info, "  |", line });
        ++shown;
    }
}

bool Report::AllPassed() const
{
    return std::none_of(findings_.begin(), findings_.end(), [](const Finding &f) { return f.status == Status::Fail; });
}

std::string Report::ToText() const
{
    std::ostringstream out;
    for (const Finding &f : findings_) {
        if (f.label.empty()) {
            out << "\n== " << f.value << " ==\n";
            continue;
        }
        out << '[' << StatusTag(f.status) << "] " << f.label << ": " << f.value << '\n';
    }
    return out.str();
}

} // namespace probe
