/*
  ==============================================================================
    CsvWriter.cpp
    Pink Noise Audio Monitor (pnam) — minimal RAII CSV writer implementation.

    MESSAGE THREAD ONLY.
  ==============================================================================
*/

#include "CsvWriter.h"

namespace pnam
{

//==============================================================================
CsvWriter::~CsvWriter()
{
    close();
}

//==============================================================================
juce::String CsvWriter::escapeCell (const juce::String& raw)
{
    const bool needsQuoting = raw.containsAnyOf (",\"\r\n");

    if (! needsQuoting)
        return raw;

    juce::String escaped = raw.replace ("\"", "\"\"");
    return "\"" + escaped + "\"";
}

//==============================================================================
bool CsvWriter::open (const juce::File& fileToOpen, const juce::StringArray& headerColumns)
{
    close();

    file = fileToOpen;

    // Ensure the parent directory exists.
    auto parent = file.getParentDirectory();
    if (! parent.exists() && ! parent.createDirectory())
    {
        writeErrorFlag = true;
        return false;
    }

    // Truncate any existing file first (FileOutputStream appends by default).
    if (file.existsAsFile())
        file.deleteFile();

    auto newStream = std::make_unique<juce::FileOutputStream> (file);

    if (! newStream->openedOk())
    {
        writeErrorFlag = true;
        stream.reset();
        return false;
    }

    stream = std::move (newStream);
    numColumns  = headerColumns.size();
    rowsWritten = 0;
    writeErrorFlag = false;

    juce::StringArray escapedHeader;
    for (auto& col : headerColumns)
        escapedHeader.add (escapeCell (col));

    auto headerLine = escapedHeader.joinIntoString (",") + "\n";

    expectedSize = (juce::int64) headerLine.getNumBytesAsUTF8();

    if (stream->writeText (headerLine, false, false, nullptr) == false)
    {
        writeErrorFlag = true;
        return false;
    }

    return true;
}

//==============================================================================
bool CsvWriter::reopenForAppend (const juce::StringArray& headerColumns)
{
    if (file == juce::File())
        return false;

    // Drop the failed stream first. Anything it still held is gone — the
    // caller accounts for those rows as lost.
    stream.reset();
    writeErrorFlag = true;

    auto parent = file.getParentDirectory();
    if (! parent.exists() && ! parent.createDirectory())
        return false;

    bool needsHeader = ! file.existsAsFile() || file.getSize() == 0;

    // A failed write can leave a partial row at the end of the file. It is cut
    // off (back to the end of the last complete row) rather than kept: a row
    // with half its cells breaks whatever reads the CSV. Only if no line end can
    // be found near the end is the fragment terminated and left in place.
    bool needsNewline = false;
    juce::int64 truncateAt = -1;

    if (! needsHeader)
    {
        juce::FileInputStream in (file);

        if (! in.openedOk())
            return false;

        const auto length = in.getTotalLength();
        const auto tailLength = (int) juce::jmin (length, (juce::int64) 1 << 20);
        juce::MemoryBlock tail ((size_t) tailLength);

        if (! in.setPosition (length - tailLength) || in.read (tail.getData(), tailLength) != tailLength)
            return false;

        const auto* bytes = static_cast<const char*> (tail.getData());

        if (bytes[tailLength - 1] != '\n')
        {
            needsNewline = true;

            for (int i = tailLength - 1; --i >= 0;)
            {
                if (bytes[i] == '\n')
                {
                    truncateAt = length - tailLength + i + 1;
                    needsNewline = false;
                    break;
                }
            }

            // Not one complete line in the whole file: all it holds is a
            // fragment of the header. Start it again.
            if (needsNewline && tailLength == length)
            {
                truncateAt = 0;
                needsNewline = false;
                needsHeader = true;
            }
        }
    }

    auto newStream = std::make_unique<juce::FileOutputStream> (file);   // appends

    if (! newStream->openedOk())
        return false;

    if (truncateAt >= 0 && (! newStream->setPosition (truncateAt) || newStream->truncate().failed()))
        return false;

    juce::String lead;

    if (needsHeader)
    {
        juce::StringArray escapedHeader;
        for (auto& col : headerColumns)
            escapedHeader.add (escapeCell (col));

        lead = escapedHeader.joinIntoString (",") + "\n";
    }
    else if (needsNewline)
    {
        lead = "\n";
    }

    const auto sizeBefore = file.getSize();

    if (lead.isNotEmpty() && ! newStream->writeText (lead, false, false, nullptr))
        return false;

    newStream->flush();

    // What is on disk now is the starting point for the size check in flush().
    // If the lead was cut short (see flush()), take the fragment back out.
    expectedSize = sizeBefore + (juce::int64) lead.getNumBytesAsUTF8();

    if (newStream->getStatus().failed() || file.getSize() < expectedSize)
    {
        if (newStream->setPosition (sizeBefore))
            newStream->truncate();

        return false;
    }

    stream = std::move (newStream);
    numColumns = headerColumns.size();
    writeErrorFlag = false;
    return true;
}

//==============================================================================
bool CsvWriter::writeRow (const juce::StringArray& cells)
{
    if (stream == nullptr || writeErrorFlag)
        return false;

    jassert (cells.size() == numColumns);

    juce::StringArray escapedCells;
    escapedCells.ensureStorageAllocated (cells.size());

    for (auto& cell : cells)
        escapedCells.add (escapeCell (cell));

    auto line = escapedCells.joinIntoString (",") + "\n";

    expectedSize += (juce::int64) line.getNumBytesAsUTF8();

    if (! stream->writeText (line, false, false, nullptr))
    {
        writeErrorFlag = true;
        return false;
    }

    ++rowsWritten;
    return true;
}

//==============================================================================
void CsvWriter::flush()
{
    if (stream != nullptr)
    {
        // juce::OutputStream::flush() returns void; FileOutputStream reports
        // failures via getStatus().
        stream->flush();

        // A write the disk only took part of does not fail the stream — it is
        // reported as "fewer bytes written", which FileOutputStream::flush()
        // discards. The file being shorter than it should be is the evidence.
        if (stream->getStatus().failed() || file.getSize() < expectedSize)
            writeErrorFlag = true;
    }
}

//==============================================================================
void CsvWriter::close()
{
    if (stream != nullptr)
    {
        stream->flush();
        stream.reset();
    }
}

//==============================================================================
bool CsvWriter::isOpen() const noexcept
{
    return stream != nullptr;
}

bool CsvWriter::hadWriteError() const noexcept
{
    return writeErrorFlag;
}

const juce::File& CsvWriter::getFile() const noexcept
{
    return file;
}

int64_t CsvWriter::getRowsWritten() const noexcept
{
    return rowsWritten;
}

} // namespace pnam
