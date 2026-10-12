//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2004-2026 musikcube team
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of the author nor the names of other contributors may
//      be used to endorse or promote products derived from this software
//      without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//////////////////////////////////////////////////////////////////////////////

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace musik { namespace core { namespace support { namespace lyrics {

    /* returns true if the string contains non-whitespace content */
    bool HasLyrics(const std::string& s);

    /* strips LRC timestamps ([mm:ss.xx]) and header tags ([ti:], [ar:],
    [al:], [by:], [length:], [offset:], ...) so provider-supplied synced
    lyrics display as clean readable text. plain (unsynced) lyrics are
    returned unchanged apart from line-ending normalization. */
    std::string NormalizeForDisplay(const std::string& raw);

    /* resolves "<audio path with any extension>" -> "<same basename>.lrc"
    in the same directory. tries ".lrc" then ".LRC". returns "" if the
    audio path is empty. */
    std::string SidecarPathForAudioFile(const std::string& audioPath);

    /* reads the sidecar .lrc file for the given audio file, if present.
    returns "" when missing/unreadable/empty. handles UTF-8 BOM. */
    std::string LoadSidecar(const std::string& audioPath);

    struct SyncedLine {
        int64_t ms = 0;      /* timestamp in milliseconds */
        std::string text;    /* lyric text ("" for gap markers) */
    };

    struct ParsedLyrics {
        std::vector<SyncedLine> lines;
        bool synced = false; /* true when timestamped lines were found */
        int offsetMs = 0;    /* global [offset:] shift, already applied */
    };

    /* full parse that KEEPS timestamps (unlike NormalizeForDisplay).
    handles BOM/CRLF, full-width brackets/colons, [offset:], header tags,
    repeated timestamps ("[00:12.40][00:15.00]text") and gap markers.
    lines are sorted by timestamp with the offset applied. */
    ParsedLyrics ParseSynced(const std::string& raw);

    /* index of the last line with ms <= positionMs, or -1 when none
    (or when lines is empty). drives the karaoke highlight. */
    int FindCurrentLine(const std::vector<SyncedLine>& lines, int64_t positionMs);

} } } }
