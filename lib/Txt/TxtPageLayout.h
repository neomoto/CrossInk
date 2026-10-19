#pragma once

#include <Utf8.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace txtlayout {
namespace detail {

// The owned paragraph is the only scratch buffer: renderer probes must see the
// exact candidate string, including shaping and fallback, without copying it.
template <typename MeasureWidth>
int measurePrefix(std::string& paragraph, const size_t begin, const size_t end, MeasureWidth& measureWidth) {
  const char saved = paragraph[end];
  paragraph[end] = '\0';
  const int width = measureWidth(paragraph.data() + begin);
  paragraph[end] = saved;
  return width;
}

inline size_t nextCodepointEnd(const std::string& paragraph, const size_t begin, const size_t limit) {
  size_t end = begin + 1;
  while (end < limit && (static_cast<unsigned char>(paragraph[end]) & 0xC0) == 0x80) ++end;
  return end;
}

template <typename MeasureWidth>
size_t fitCodepoints(std::string& paragraph, const size_t begin, const size_t wordEnd, const int maxWidth,
                     MeasureWidth& measureWidth) {
  size_t fitEnd = begin;
  while (fitEnd < wordEnd) {
    const size_t end = nextCodepointEnd(paragraph, fitEnd, wordEnd);
    if (measurePrefix(paragraph, begin, end, measureWidth) > maxWidth) {
      // Even an oversized first codepoint must advance intact.
      if (fitEnd == begin) return end;
      return fitEnd;
    }
    fitEnd = end;
  }
  return fitEnd;
}

template <typename MeasureWidth>
size_t findWrapEnd(std::string& paragraph, const size_t begin, const bool complete, const int maxWidth,
                   MeasureWidth& measureWidth) {
  size_t fitEnd = begin;
  size_t candidateEnd = paragraph.find(' ', begin + 1);
  for (;;) {
    if (candidateEnd == std::string::npos) candidateEnd = paragraph.size();
    if (measurePrefix(paragraph, begin, candidateEnd, measureWidth) > maxWidth) {
      if (fitEnd > begin) return fitEnd;
      return fitCodepoints(paragraph, begin, candidateEnd, maxWidth, measureWidth);
    }
    if (candidateEnd == paragraph.size()) return complete ? candidateEnd : begin;
    fitEnd = candidateEnd;
    candidateEnd = paragraph.find(' ', candidateEnd + 1);
  }
}

template <typename MeasureWidth>
size_t wrapParagraph(std::string& paragraph, const bool complete, const int linesPerPage, const int maxWidth,
                     MeasureWidth& measureWidth, std::vector<std::string>& outLines) {
  if (paragraph.empty()) {
    if (complete) outLines.emplace_back();
    return 0;
  }

  size_t begin = 0;
  while (begin < paragraph.size() && static_cast<int>(outLines.size()) < linesPerPage) {
    const size_t end = findWrapEnd(paragraph, begin, complete, maxWidth, measureWidth);
    // Defer a fitting unknown tail after ordinary wrapped lines. If the whole
    // chunk fits (for example zero-width marks), force one bounded chunk line:
    // callers cannot fetch more data after a zero-byte page.
    if (end == begin) {
      if (begin == 0) {
        outLines.emplace_back(paragraph);
        return paragraph.size();
      }
      break;
    }
    outLines.emplace_back(paragraph.data() + begin, end - begin);
    begin = end;
    // Preserve the existing one-space wrap skip, not all following whitespace.
    if (begin < paragraph.size() && paragraph[begin] == ' ') ++begin;
  }
  return begin;
}

inline size_t displayLength(const uint8_t* buffer, size_t length, const bool complete) {
  if (!complete) {
    length =
        static_cast<size_t>(utf8SafeTruncateBuffer(reinterpret_cast<const char*>(buffer), static_cast<int>(length)));
  }
  // For a partial chunk, hold a possible CRLF's CR until the next read too.
  if (length > 0 && buffer[length - 1] == '\r') --length;
  return length;
}

}  // namespace detail

// A stale on-disk bookmark must never replace a position already retained by
// the live reader, even when a failed migration leaves the old index in place.
inline std::optional<size_t> preferredRestoreOffset(const std::optional<size_t>& pending,
                                                    const std::optional<size_t>& retained,
                                                    const std::optional<size_t>& persisted) {
  if (pending) return pending;
  if (retained) return retained;
  return persisted;
}

// A split four-byte codepoint plus its following CRLF needs at most five
// extra file bytes. Keep both units together without accumulating a paragraph.
constexpr size_t CHUNK_LOOKAHEAD_BYTES = 5;

inline size_t alignedChunkLength(const uint8_t* buffer, const size_t preferredSize, const size_t availableSize) {
  size_t end = std::min(preferredSize, availableSize);
  while (end < availableSize && (buffer[end] & 0xC0) == 0x80) ++end;
  if (end < availableSize && buffer[end] == '\r') ++end;
  if (end < availableSize && buffer[end] == '\n') ++end;
  return end;
}

// Forward greedy wrapping measures complete word candidates and stops at the
// first overflow. Contextual fonts may make later candidates narrower again;
// those later candidates intentionally do not change an already chosen break.
// Offsets count source bytes, including CRLF and the single skipped wrap space.
// Unrendered partial tails stay unread; an otherwise fitting chunk is forced
// into one line to guarantee bounded forward progress.
template <typename MeasureWidth>
size_t parsePage(const uint8_t* buffer, const size_t chunkSize, const size_t fileOffset, const size_t fileSize,
                 const int linesPerPage, const int maxWidth, MeasureWidth&& measureWidth,
                 std::vector<std::string>& outLines) {
  size_t pos = 0;
  while (pos < chunkSize && static_cast<int>(outLines.size()) < linesPerPage) {
    const auto* end = std::find(buffer + pos, buffer + chunkSize, static_cast<uint8_t>('\n'));
    const size_t lineEnd = static_cast<size_t>(end - buffer);
    const bool complete = lineEnd < chunkSize || fileOffset + lineEnd >= fileSize;
    if (!complete && !outLines.empty()) break;

    const size_t length = detail::displayLength(buffer + pos, lineEnd - pos, complete);
    std::string paragraph(reinterpret_cast<const char*>(buffer + pos), length);
    const size_t consumed = detail::wrapParagraph(paragraph, complete, linesPerPage, maxWidth, measureWidth, outLines);
    pos += consumed;
    if (!complete || consumed < paragraph.size()) break;

    pos = lineEnd;
    if (pos < chunkSize) ++pos;  // Consume only an actual newline, never an EOF/chunk sentinel.
  }
  return pos;
}

}  // namespace txtlayout
