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

#include "pch.hpp"
#include <musikcore/support/Lyrics.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace musik { namespace core { namespace support { namespace lyrics {

static inline bool isSpace(char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\v' || c == '\f';
}

static std::string trimCopy(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) { ++b; }
    while (e > b && isSpace(s[e - 1])) { --e; }
    return s.substr(b, e - b);
}

static bool isHeaderTag(const std::string& line);
static std::string stripTimestamps(std::string line);

bool HasLyrics(const std::string& s) {
    for (char c : s) {
        if (!isSpace(c)) {
            return true;
        }
    }
    return false;
}

/* cmus-lyric parity: some providers emit full-width brackets/colons,
which break timestamp parsing. normalize them to half-width. */
static std::string normalizeFullWidth(std::string s) {
    static const char* kBrackets[][2] = {
        { "\xEF\xBC\xBB", "[" }, /* ［ U+FF3B */
        { "\xEF\xBC\xBD", "]" }, /* ］ U+FF3D */
        { "\xE3\x80\x90", "[" }, /* 【 U+300C-ish open */
        { "\xE3\x80\x91", "]" }, /* 】 close */
    };
    for (auto& pair : kBrackets) {
        const std::string from = pair[0];
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), pair[1]);
            pos += 1;
        }
    }
    /* full-width colon (EF BC 9A) only when flanked by digits: 00：05.00 */
    const std::string colon = "\xEF\xBC\x9A";
    {
        size_t pos = 0;
        while ((pos = s.find(colon, pos)) != std::string::npos) {
            bool beforeDigit = pos > 0 && std::isdigit((unsigned char) s[pos - 1]);
            bool afterDigit = pos + colon.size() < s.size() &&
                std::isdigit((unsigned char) s[pos + colon.size()]);
            if (beforeDigit && afterDigit) {
                s.replace(pos, colon.size(), ":");
                pos += 1;
            }
            else {
                pos += colon.size();
            }
        }
    }
    return s;
}

static std::string normalizeLineEndings(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < s.size() && s[i + 1] == '\n') {
                ++i;
            }
        }
        else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::string stripBom(std::string s) {
    if (s.size() >= 3 &&
        (unsigned char) s[0] == 0xEF &&
        (unsigned char) s[1] == 0xBB &&
        (unsigned char) s[2] == 0xBF)
    {
        s = s.substr(3);
    }
    return s;
}

/* parses the INSIDE of a [..]/<..> stamp ("mm:ss", "mm:ss.xx",
"mm:ss:xx", "mm:ss,xxx"). returns -1 when not a timestamp. */
static int64_t parseTimestampMs(const std::string& inside) {
    size_t colon = inside.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= inside.size()) {
        return -1;
    }
    for (size_t i = 0; i < inside.size(); i++) {
        if (i == colon) {
            continue;
        }
        char c = inside[i];
        if (!std::isdigit((unsigned char) c) && c != '.' && c != ',') {
            return -1;
        }
    }
    try {
        int minutes = std::stoi(inside.substr(0, colon));
        std::string rest = inside.substr(colon + 1);
        int seconds = 0, millis = 0;
        size_t dot = rest.find_first_of(".,");
        if (dot == std::string::npos) {
            seconds = std::stoi(rest);
        }
        else {
            seconds = std::stoi(rest.substr(0, dot));
            std::string frac = rest.substr(dot + 1);
            if (frac.empty() || frac.size() > 3) {
                return -1;
            }
            for (char c : frac) {
                if (!std::isdigit((unsigned char) c)) {
                    return -1;
                }
            }
            while (frac.size() < 3) {
                frac.push_back('0');
            }
            millis = std::stoi(frac);
        }
        if (seconds < 0 || seconds >= 100 || minutes < 0) {
            return -1;
        }
        return (int64_t) minutes * 60000 + (int64_t) seconds * 1000 + millis;
    }
    catch (...) {
        return -1;
    }
}

/* queue of leading stamps stripped from a line; returns the remainder.
also used to detect multi-stamp lines ("[t1][t2]text"). */
static std::vector<int64_t> takeLeadingStamps(std::string& line) {
    std::vector<int64_t> stamps;
    line = trimCopy(line);
    while (!line.empty()) {
        char open = line[0];
        char expectedClose = (open == '[') ? ']' : (open == '<' ? '>' : 0);
        if (!expectedClose) {
            break;
        }
        size_t close = line.find(expectedClose);
        if (close == std::string::npos || close < 3 || close > 12) {
            break;
        }
        int64_t ms = parseTimestampMs(line.substr(1, close - 1));
        if (ms < 0) {
            break;
        }
        stamps.push_back(ms);
        line = trimCopy(line.substr(close + 1));
    }
    return stamps;
}

static int parseOffsetMs(const std::string& line) {
    /* matches "[offset:+500]" / "[offset: -250]" (case-insensitive) */
    std::string t = trimCopy(line);
    if (t.size() < 10 || t.front() != '[') {
        return 0;
    }
    size_t close = t.find(']');
    if (close == std::string::npos) {
        return 0;
    }
    std::string inside = trimCopy(t.substr(1, close - 1));
    size_t colon = inside.find(':');
    if (colon == std::string::npos) {
        return 0;
    }
    std::string name = trimCopy(inside.substr(0, colon));
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) {
        lower.push_back((char) std::tolower((unsigned char) c));
    }
    if (lower != "offset") {
        return 0;
    }
    try {
        return std::stoi(trimCopy(inside.substr(colon + 1)));
    }
    catch (...) {
        return 0;
    }
}

ParsedLyrics ParseSynced(const std::string& raw) {
    ParsedLyrics result;
    if (raw.empty()) {
        return result;
    }

    std::string fixed = stripBom(normalizeFullWidth(normalizeLineEndings(raw)));

    std::stringstream in(fixed);
    std::string line;
    int64_t lastMs = 0;
    bool sawOffset = false;

    struct RawLine { int64_t ms; std::string text; bool stamped; };
    std::vector<RawLine> pending;

    while (std::getline(in, line, '\n')) {
        std::string t = trimCopy(line);
        if (t.empty()) {
            continue;
        }
        if (!sawOffset) {
            int off = parseOffsetMs(t);
            if (off != 0 || t.size() >= 9) {
                /* parseOffsetMs returns 0 both for "[offset:0]" and for
                non-offset lines; only consume the line as an offset
                declaration when it actually is one. */
                std::string low = t;
                for (char& c : low) { c = (char) std::tolower((unsigned char) c); }
                if (low.find("[offset:") == 0) {
                    result.offsetMs = off;
                    sawOffset = true;
                    continue;
                }
            }
        }
        if (isHeaderTag(t)) {
            continue;
        }
        std::string rest = t;
        auto stamps = takeLeadingStamps(rest);
        if (stamps.empty()) {
            /* unstamped lyric line: continuation of the previous stamp
            (credit lines, translations without their own time). */
            pending.push_back({ lastMs, t, false });
        }
        else {
            for (int64_t ms : stamps) {
                pending.push_back({ ms, rest, true });
                lastMs = ms;
            }
            result.synced = true;
        }
    }

    if (pending.empty()) {
        return result;
    }

    /* stable sort keeps file order for identical timestamps */
    std::stable_sort(pending.begin(), pending.end(),
        [](const RawLine& a, const RawLine& b) { return a.ms < b.ms; });

    result.lines.reserve(pending.size());
    for (auto& p : pending) {
        int64_t ms = p.ms + result.offsetMs;
        if (ms < 0) {
            ms = 0;
        }
        result.lines.push_back({ ms, p.text });
    }
    return result;
}

int FindCurrentLine(const std::vector<SyncedLine>& lines, int64_t positionMs) {
    int current = -1;
    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].ms <= positionMs) {
            current = (int) i;
        }
        else {
            break; /* lines are sorted */
        }
    }
    return current;
}

/* true for LRC header tags like [ti:...], [ar:...], [al:...], [by:...],
[length:...], [offset:...], [re:...], [ve:...]. timestamps ([00:12.34])
start with a digit and are NOT headers. */
static bool isHeaderTag(const std::string& line) {
    if (line.size() < 4 || line.front() != '[') {
        return false;
    }
    size_t close = line.find(']');
    if (close == std::string::npos) {
        return false;
    }
    std::string inside = trimCopy(line.substr(1, close - 1));
    if (inside.empty()) {
        return false;
    }
    /* timestamp? mm:ss(.xx) -- starts with digit and contains ':' */
    if (std::isdigit((unsigned char) inside[0]) && inside.find(':') != std::string::npos) {
        return false;
    }
    size_t colon = inside.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    std::string name = trimCopy(inside.substr(0, colon));
    if (name.empty()) {
        return false;
    }
    for (char c : name) {
        if (!std::isalpha((unsigned char) c)) {
            return false;
        }
    }
    /* ensure nothing but whitespace follows the tag on this line;
    header lines never carry lyric text. timestamped lyric lines DO. */
    std::string rest = trimCopy(line.substr(close + 1));
    if (!rest.empty()) {
        /* a line like "[ar:Foo] actual lyric" is degenerate; treat the
        bracket as a header only when it consumes the whole line. */
        if (rest.front() == '[') {
            return isHeaderTag(rest);
        }
        return false;
    }
    return true;
}

/* removes leading LRC timestamps: [mm:ss], [mm:ss.xx], [mm:ss:xx],
<mm:ss.xx>, plus repeated/adjacent stamps like
"[00:12.40][00:15.00]text". returns the remainder. */
static std::string stripTimestamps(std::string line) {
    line = trimCopy(line);
    bool stripped = true;
    while (stripped && !line.empty()) {
        stripped = false;
        char open = line[0];
        char expectedClose = (open == '[') ? ']' : (open == '<' ? '>' : 0);
        if (!expectedClose) {
            return line;
        }
        size_t close = line.find(expectedClose);
        if (close == std::string::npos || close < 3 || close > 12) {
            return line;
        }
        std::string inside = line.substr(1, close - 1);
        /* must look like [00:12.40]: digits + ':' + digits, optional .xx */
        size_t colon = inside.find(':');
        if (colon == std::string::npos) {
            return line;
        }
        bool ok = true;
        for (size_t i = 0; i < inside.size(); i++) {
            char c = inside[i];
            if (i == colon) {
                if (c != ':') { ok = false; break; }
            }
            else if (!(std::isdigit((unsigned char) c) || c == '.' || c == ',')) {
                ok = false; break;
            }
        }
        /* require at least m:ss digits around the colon */
        if (ok) {
            bool hasDigitBefore = false, hasDigitAfter = false;
            for (size_t i = 0; i < colon; i++) {
                if (std::isdigit((unsigned char) inside[i])) { hasDigitBefore = true; break; }
            }
            for (size_t i = colon + 1; i < inside.size(); i++) {
                if (std::isdigit((unsigned char) inside[i])) { hasDigitAfter = true; break; }
            }
            ok = hasDigitBefore && hasDigitAfter;
        }
        if (!ok) {
            return line;
        }
        line = trimCopy(line.substr(close + 1));
        stripped = true;
    }
    return line;
}

std::string NormalizeForDisplay(const std::string& raw) {
    if (raw.empty()) {
        return raw;
    }

    std::string fixed = stripBom(normalizeFullWidth(normalizeLineEndings(raw)));

    std::stringstream in(fixed);
    std::string line, out;
    bool seenLyric = false;
    std::string lineOut;
    while (std::getline(in, line, '\n')) {
        std::string t = trimCopy(line);
        if (t.empty()) {
            continue;
        }
        if (isHeaderTag(t)) {
            continue;
        }
        t = stripTimestamps(t);
        if (t.empty()) {
            continue; /* timestamp-only gap/marker line */
        }
        /* a leftover pure "[...]" metadata fragment (e.g. stray
        "[instrumental]") is kept -- it carries meaning. only the known
        "tag:" headers above are dropped. */
        if (!out.empty()) {
            out.push_back('\n');
        }
        out += t;
        seenLyric = true;
    }

    if (!seenLyric) {
        /* nothing looked like lyrics (no timestamps, no plain lines?);
        fall back to the trimmed raw input so we never blank out
        legitimately plain synced-remote lyrics. */
        lineOut = trimCopy(fixed);
        return lineOut;
    }
    return out;
}

std::string SidecarPathForAudioFile(const std::string& audioPath) {
    if (audioPath.empty()) {
        return "";
    }
    try {
        std::filesystem::path p(std::filesystem::u8path(audioPath));
        std::filesystem::path dir = p.parent_path();
        std::string stem = p.stem().u8string();
        if (stem.empty()) {
            return "";
        }
        std::filesystem::path candidate = stem + ".lrc";
        if (!dir.empty()) {
            candidate = dir / candidate;
        }
        return candidate.u8string();
    }
    catch (...) {
        /* manual fallback for odd encodings / platforms */
        size_t slash = audioPath.find_last_of("/\\");
        size_t dot = audioPath.find_last_of(".");
        size_t nameStart = (slash == std::string::npos) ? 0 : slash + 1;
        if (dot == std::string::npos || dot < nameStart) {
            return audioPath + ".lrc";
        }
        return audioPath.substr(0, dot) + ".lrc";
    }
}

std::string LoadSidecar(const std::string& audioPath) {
    std::string path = SidecarPathForAudioFile(audioPath);
    if (path.empty()) {
        return "";
    }

    auto readFile = [](const std::string& fn) -> std::string {
        try {
            std::filesystem::path p(std::filesystem::u8path(fn));
            std::error_code ec;
            auto size = std::filesystem::file_size(p, ec);
            if (ec || size == 0 || size > (1024 * 1024)) {
                if (ec || size > (1024 * 1024)) {
                    /* fall through to stream read for odd fs; the
                    stream read below is still size-capped */
                }
            }
            std::ifstream in(p, std::ios::in | std::ios::binary);
            if (!in.good()) {
                return "";
            }
            std::string content;
            content.reserve((size_t) std::min<uint64_t>(size, 1024 * 1024));
            char buf[8192];
            size_t total = 0;
            while (in.good() && total < (1024 * 1024)) {
                in.read(buf, sizeof(buf));
                std::streamsize got = in.gcount();
                if (got <= 0) {
                    break;
                }
                total += (size_t) got;
                content.append(buf, (size_t) got);
            }
            /* strip UTF-8 BOM */
            if (content.size() >= 3 &&
                (unsigned char) content[0] == 0xEF &&
                (unsigned char) content[1] == 0xBB &&
                (unsigned char) content[2] == 0xBF)
            {
                content = content.substr(3);
            }
            return content;
        }
        catch (...) {
            return "";
        }
    };

    std::string content = readFile(path);
    if (!content.empty()) {
        return content;
    }

    /* case-insensitive retry: "song.LRC" / "song.Lrc" */
    try {
        std::filesystem::path p(std::filesystem::u8path(path));
        std::filesystem::path dir = p.parent_path();
        std::string stem = p.stem().u8string();
        if (!stem.empty()) {
            static const char* kVariants[] = { ".LRC", ".Lrc" };
            for (auto ext : kVariants) {
                std::filesystem::path alt = stem + ext;
                if (!dir.empty()) {
                    alt = dir / alt;
                }
                content = readFile(alt.u8string());
                if (!content.empty()) {
                    return content;
                }
            }
        }
    }
    catch (...) {
    }
    return "";
}

} } } }
