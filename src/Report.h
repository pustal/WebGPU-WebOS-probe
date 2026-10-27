/**
 * @file Report.h
 * @brief The probe's findings: a list of labelled lines grouped in sections,
 *        shown on screen and saved as plain text.
 */
#ifndef WGPUPROBE_REPORT_H
#define WGPUPROBE_REPORT_H

#include <string>
#include <vector>

namespace probe
{

/** How a finding should be read. */
enum class Status {
    Info, ///< Plain information.
    Pass, ///< Meets what the Luigi's Mansion port (Aurora) needs.
    Warn, ///< Works, but with a caveat (e.g. slower than ideal).
    Fail, ///< Does not meet what the port needs.
};

/** One line of the report. A line with an empty label is a section heading. */
struct Finding {
    Status status;
    std::string label;
    std::string value;
};

class Report
{
public:
    /** Starts a new section. */
    void Section(const std::string &title) { findings_.push_back({ Status::Info, std::string(), title }); }

    /** Adds a finding to the current section. */
    void Add(Status status, const std::string &label, const std::string &value)
    {
        findings_.push_back({ status, label, value });
    }

    /** Adds a multi-line detail (for example a shader compiler log), indented under the last finding. */
    void Detail(const std::string &text);

    const std::vector<Finding> &Findings() const { return findings_; }

    /** True when no finding failed. */
    bool AllPassed() const;

    /** The whole report as plain text, one finding per line. */
    std::string ToText() const;

private:
    std::vector<Finding> findings_;
};

/** Short tag for a status, as shown on screen and in the text report. */
const char *StatusTag(Status status);

} // namespace probe

#endif // WGPUPROBE_REPORT_H
