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

#include <stdafx.h>
#include <app/layout/LyricLineEntry.h>
#include <cursespp/Text.h>

using namespace musik::cube;
using namespace cursespp;

LyricLineEntry::LyricLineEntry(const std::string& value)
: SingleLineEntry(value) {
    this->SetLyricState(State::Upcoming);
}

void LyricLineEntry::SetLyricState(State state) {
    this->state = state;
    switch (state) {
        case State::Current:
            this->SetAttrs(Color(Color::TextActive));
            break;
        case State::Past:
            this->SetAttrs(Color(Color::TextDisabled));
            break;
        case State::Upcoming:
        default:
            this->SetAttrs(Color(Color::Default));
            break;
    }
}

void LyricLineEntry::SetWidth(size_t width) {
    SingleLineEntry::SetWidth(width);
    this->entryWidth = width;
}

std::string LyricLineEntry::GetLine(size_t line) {
    std::string text = this->GetValue();
    if (!text.size()) {
        text = "..."; /* gap marker: timestamp with no words */
    }
    else if (this->state == State::Current) {
        text = "\xE2\x99\xAA " + text; /* "♪ " */
    }
    /* ellipsize first (display-width aware), then center the result so
    wide/CJK characters never break alignment. */
    return text::Align(
        text::Ellipsize(text, this->entryWidth),
        text::AlignCenter,
        this->entryWidth);
}
