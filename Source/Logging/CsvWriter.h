#pragma once
/*
  ==============================================================================
    CsvWriter.h
    Pink Noise Audio Monitor (pnam) — minimal RAII CSV writer.

    MESSAGE THREAD ONLY. Never call from the audio thread (does file I/O).

    Behaviour contract for the .cpp author:
      - open() creates/truncates the file, writes the header row, and keeps a
        buffered juce::FileOutputStream open for the life of the session.
      - Quoting (RFC 4180): a cell is wrapped in double quotes if it contains
        a comma, a double quote, CR, or LF; embedded double quotes are doubled.
        Numeric cells produced by SessionLogger never need quoting but are run
        through the same escaper for uniformity.
      - Line ending: "\n". Encoding: UTF-8, no BOM.
      - writeRow() appends one line; it does NOT flush. flush() forces the
        stream to disk; SessionLogger calls it periodically (every few
        seconds) and on every event row, so a crash/power-cut loses at most a
        few measurement rows.
      - All I/O failures set a sticky error flag (hadWriteError()) instead of
        throwing; SessionLogger checks it and raises a LoggingError event.
      - Destructor flushes and closes (RAII).
  ==============================================================================
*/

#include <juce_core/juce_core.h>

namespace pnam
{

//==============================================================================
class CsvWriter
{
public:
    CsvWriter() = default;

    /** Flushes and closes if still open. */
    ~CsvWriter();

    CsvWriter (const CsvWriter&) = delete;
    CsvWriter& operator= (const CsvWriter&) = delete;

    //==========================================================================
    /** Creates (or truncates) `file`, writes `headerColumns` as the first row.
        @returns false on failure (file/stream could not be created); the
        writer is left closed. */
    bool open (const juce::File& file, const juce::StringArray& headerColumns);

    /** Appends one row. Cell count should match the header (asserted in debug
        builds, tolerated in release). @returns false if closed or if a write
        error occurred (error flag becomes sticky). */
    bool writeRow (const juce::StringArray& cells);

    /** Forces buffered data to disk. Safe to call when closed (no-op). */
    void flush();

    /** Flushes and closes the stream. Safe to call repeatedly. */
    void close();

    //==========================================================================
    bool isOpen() const noexcept;

    /** Sticky flag: true if any write/flush has failed since open(). */
    bool hadWriteError() const noexcept;

    /** The file passed to open() (invalid File if never opened). */
    const juce::File& getFile() const noexcept;

    /** Number of data rows written since open() (excludes header). */
    int64_t getRowsWritten() const noexcept;

    //==========================================================================
    /** RFC-4180 escaping of a single cell (see header comment). Pure helper,
        usable without an instance. */
    static juce::String escapeCell (const juce::String& raw);

private:
    std::unique_ptr<juce::FileOutputStream> stream;
    juce::File file;
    int        numColumns   { 0 };
    int64_t    rowsWritten  { 0 };
    bool       writeErrorFlag { false };
};

} // namespace pnam
