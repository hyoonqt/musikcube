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

#include <cursespp/SingleLineEntry.h>

namespace musik { namespace cube {

    /* a centered lyric line with karaoke state. subclasses SingleLineEntry
    (rather than implementing IEntry) because SimpleScrollAdapter
    static_casts its entries to SingleLineEntry internally. */
    class LyricLineEntry : public cursespp::SingleLineEntry {
        public:
            enum class State {
                Upcoming, /* not sung yet: default terminal colors */
                Current,  /* now playing: accent color + marker */
                Past      /* already sung: dimmed */
            };

            LyricLineEntry(const std::string& value);

            void SetLyricState(State state);
            State GetLyricState() const { return state; }

            void SetWidth(size_t width) override;
            std::string GetLine(size_t line) override;

        private:
            State state { State::Upcoming };
            size_t entryWidth { 0 };
    };

} }
