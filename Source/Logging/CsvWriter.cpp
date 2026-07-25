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

    if (stream->writeText (headerLine, false, false, nullptr) == false)
    {
        writeErrorFlag = true;
        return false;
    }

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

        if (stream->getStatus().failed())
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
