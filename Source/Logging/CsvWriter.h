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
      - All I/O failures set an error flag (hadWriteError()) instead of
        throwing; SessionLogger checks it and raises a LoggingError event. The
        flag stays set (and writeRow() refuses rows) until reopenForAppend()
        succeeds — a failure is not allowed to end logging for the session.
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

    /** Forces buffered data to disk. Safe to call when closed (no-op). Sets the
        error flag if the stream reports a failure OR the file is shorter
        afterwards than everything written to it should have made it. */
    void flush();

    /** Recovery after a write/flush error: drops the failed stream and reopens
        the file given to open() for APPEND, keeping everything already in it.
        Writes the header only if the file is missing or empty, and first
        removes a half-written last line (the failed write's fragment) so the
        file holds complete rows only. Clears the error flag on success; getRowsWritten() carries on.
        @returns false (flag still set) if the file cannot even be reopened.

        A true result is PROVISIONAL: when the file needed neither header nor
        newline nothing was written, and reopening succeeds on a full disk. The
        caller has to write and flush a real row before treating the file as
        recovered. */
    bool reopenForAppend (const juce::StringArray& headerColumns);

    /** Flushes and closes the stream. Safe to call repeatedly. */
    void close();

    //==========================================================================
    bool isOpen() const noexcept;

    /** True if a write/flush has failed since open() or the last successful
        reopenForAppend(). */
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

    // Bytes the file must hold once everything handed to the stream has been
    // flushed. flush() checks the real size against it: a SHORT write (a full
    // disk taking part of the buffer) is not an error as far as the stream's
    // status is concerned, and would otherwise pass for a successful flush.
    juce::int64 expectedSize { 0 };
};

} // namespace pnam
