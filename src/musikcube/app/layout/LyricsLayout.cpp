//////////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2004-2023 musikcube team
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
#include <app/layout/LyricsLayout.h>
#include <app/layout/LyricLineEntry.h>
#include <app/util/Messages.h>
#include <musikcore/i18n/Locale.h>
#include <musikcore/support/Auddio.h>
#include <musikcore/support/Lyrics.h>
#include <musikcore/sdk/String.h>
#include <musikcore/library/query/LyricsQuery.h>
#include <cursespp/App.h>
#include <cursespp/Screen.h>
#include <cursespp/ToastOverlay.h>
#include <cursespp/SingleLineEntry.h>
#include <app/util/Hotkeys.h>
#include <app/util/Messages.h>

using namespace musik::cube;
using namespace musik::core;
using namespace musik::core::audio;
using namespace musik::core::library::query;
using namespace musik::core::runtime;
using namespace musik::core::sdk;
using namespace cursespp;

LyricsLayout::LyricsLayout(PlaybackService& playback, ILibraryPtr library)
: LayoutBase()
, currentTrackId(-1LL)
, library(library)
, playback(playback) {
    this->playback.TrackChanged.connect(this, &LyricsLayout::OnTrackChanged);
    this->playback.TimeChanged.connect(this, &LyricsLayout::OnTimeChanged);

    this->adapter = std::make_shared<SimpleScrollAdapter>();
    /* selection is meaningless in a karaoke view (and fights the follow
    highlight), so the highlight is driven purely by entry colors. */
    this->adapter->SetSelectable(false);

    this->listView = std::make_shared<ListWindow>(this->adapter);
    this->AddWindow(this->listView);
    this->listView->SetFocusOrder(0);

    this->infoText = std::make_shared<TextLabel>("", text::AlignCenter);
    this->AddWindow(this->infoText);

    this->LoadLyricsForCurrentTrack();
}

void LyricsLayout::OnLayout() {
    LayoutBase::OnLayout();
    const int cx = this->GetContentWidth();
    const int cy = this->GetContentHeight();
    this->listView->MoveAndResize(0, 0, cx, cy);
    this->infoText->MoveAndResize(1, cy / 2, cx - 2, 1);
    /* entry centering is width-dependent; rebuild when the width changes
    (also fixes stale ellipsis clipping after a resize). */
    if (cx != (int) this->lastLayoutWidth && this->currentLyrics.size()) {
        this->lastLayoutWidth = (size_t) cx;
        this->UpdateAdapter();
        this->UpdateHighlight((int64_t) (this->playback.GetPosition() * 1000.0), true);
    }
}

void LyricsLayout::OnTrackChanged(size_t index, TrackPtr track) {
    if (this->IsVisible()) {
        this->LoadLyricsForCurrentTrack();
    }
}

void LyricsLayout::OnTimeChanged(double seconds) {
    /* instant response for seeks (normal playback ticks come from the
    self-perpetuating LyricsProgress loop below, because TimeChanged
    only fires on seeks/mode changes -- not continuously). */
    if (this->IsVisible() && this->state == State::Loaded) {
        this->Post(message::LyricsProgress, (int64_t) (seconds * 1000.0));
    }
}

void LyricsLayout::ScheduleProgressTick() {
    /* TransportWindow parity: there is no continuous time signal, so poll
    GetPosition() on a short loop while the view is live. Debounce
    collapses repeats; the loop stops itself when hidden/unloaded. */
    if (this->IsVisible() && this->state == State::Loaded) {
        this->Debounce(message::LyricsProgress, 0, 0, 250);
    }
}

int LyricsLayout::LineIndexForPosition(int64_t positionMs) {
    const size_t count = this->adapter->GetEntryCount();
    if (!count) {
        return -1;
    }
    if (this->lyricsSynced) {
        return support::lyrics::FindCurrentLine(this->parsedLyrics.lines, positionMs);
    }
    /* unsynced fallback (cmus-lyric parity): estimate by progress ratio */
    if (this->lastDuration > 0.0) {
        double pos = (double) positionMs / 1000.0;
        int estimated = (int) (pos * (double) count / this->lastDuration);
        if (estimated < 0) {
            estimated = 0;
        }
        if (estimated >= (int) count) {
            estimated = (int) count - 1;
        }
        return estimated;
    }
    return -1;
}

void LyricsLayout::CenterOnLine(int index) {
    const size_t count = this->adapter->GetEntryCount();
    if (!count || index < 0) {
        return;
    }
    size_t visible = this->listView->GetScrollPosition().visibleEntryCount;
    if (!visible || visible >= count) {
        this->listView->ScrollTo(0);
        return;
    }
    size_t half = visible / 2;
    size_t target = (size_t) index <= half ? 0 : (size_t) index - half;
    size_t maxFirst = count - visible;
    if (target > maxFirst) {
        target = maxFirst;
    }
    this->listView->ScrollTo(target);
}

void LyricsLayout::UpdateHighlight(int64_t positionMs, bool force) {
    if (!this->adapter->GetEntryCount()) {
        return;
    }
    int index = this->LineIndexForPosition(positionMs);
    if (!force && index == this->currentLineIndex) {
        return;
    }
    this->currentLineIndex = index;
    const size_t count = this->adapter->GetEntryCount();
    for (size_t i = 0; i < count; i++) {
        auto entry = this->adapter->GetEntry(this->listView.get(), i);
        auto lyric = static_cast<LyricLineEntry*>(entry.get());
        if ((int) i == index) {
            lyric->SetLyricState(LyricLineEntry::State::Current);
        }
        else if (index >= 0 && (int) i < index) {
            lyric->SetLyricState(LyricLineEntry::State::Past);
        }
        else {
            lyric->SetLyricState(LyricLineEntry::State::Upcoming);
        }
    }
    /* manual scrolling pauses the follow-scroll, but the highlight itself
    always tracks playback; a new track resumes following. */
    if (this->followPlayback) {
        this->CenterOnLine(index);
    }
    else {
        this->listView->Invalidate();
    }
}

void LyricsLayout::OnLyricsLoaded() {
    this->parsedLyrics = support::lyrics::ParseSynced(this->currentLyrics);
    this->lyricsSynced = this->parsedLyrics.synced;
    this->currentLineIndex = -1;
    this->followPlayback = true;

    auto track = this->playback.GetPlaying();
    if (track) {
        this->lastDuration = track->GetDouble("duration", 0.0);
        this->listView->SetFrameTitle(u8fmt(
            _TSTR("lyrics_list_title"),
            track->GetString("title").c_str(),
            track->GetString("artist").c_str()) +
            (this->lyricSource.size() ? " [" + this->lyricSource + "]" : ""));
    }

    this->UpdateAdapter();
    this->SetState(State::Loaded);
    this->UpdateHighlight((int64_t) (this->playback.GetPosition() * 1000.0), true);
    this->ScheduleProgressTick();
}

bool LyricsLayout::KeyPress(const std::string& kn) {
    if (Hotkeys::Is(Hotkeys::LyricsRetry, kn)) {
        this->followPlayback = true;
        this->LoadLyricsForCurrentTrack();
        return true;
    }
    else if (Hotkeys::Is(Hotkeys::NavigateLibraryPlayQueue, kn)) {
        this->Broadcast(message::JumpToPlayQueue);
        return true;
    }
    else if (Hotkeys::Is(Hotkeys::NavigateLibrary, kn)) {
        this->Broadcast(message::JumpToLibrary);
        return true;
    }
    else if (kn == " ") { /* ugh... need to generalize this maybe */
        playback.PauseOrResume();
        return true;
    }
    else if (kn == "KEY_UP" || kn == "KEY_DOWN" ||
             kn == "KEY_PPAGE" || kn == "KEY_NPAGE" ||
             kn == "KEY_HOME" || kn == "KEY_END")
    {
        /* manual scrolling pauses the follow-scroll (the highlight keeps
        tracking); a new track or retry resumes following. */
        this->followPlayback = false;
    }

    return LayoutBase::KeyPress(kn);
}

void LyricsLayout::OnVisibilityChanged(bool visible) {
    LayoutBase::OnVisibilityChanged(visible);
    if (visible) {
        this->LoadLyricsForCurrentTrack();
        /* same track still showing: resync the highlight to the live
        position and restart ticks (ticks only run while visible). */
        if (this->state == State::Loaded) {
            this->UpdateHighlight((int64_t) (this->playback.GetPosition() * 1000.0), true);
            this->ScheduleProgressTick();
        }
        this->FocusFirst();
    }
}

#include <iostream>

void LyricsLayout::ProcessMessage(musik::core::runtime::IMessage &m) {
    if (m.Type() == message::LyricsLoaded) {
        const auto state = static_cast<State>(m.UserData1());
        if (state == State::Loaded && this->currentLyrics.size()) {
            this->OnLyricsLoaded();
        }
        else {
            this->SetState(state);
        }
    }
    else if (m.Type() == message::LyricsProgress) {
        /* playback tick: poll the live position (the message itself
        carries no position -- seeks post one immediately, the Debounce
        loop below keeps it advancing during normal playback). */
        auto track = this->playback.GetPlaying();
        if (track && track->GetId() == this->currentTrackId) {
            this->UpdateHighlight((int64_t) (this->playback.GetPosition() * 1000.0), false);
        }
        this->ScheduleProgressTick();
    }
    else {
        LayoutBase::ProcessMessage(m);
    }
}

void LyricsLayout::LoadLyricsForCurrentTrack() {
    auto track = playback.GetPlaying();
    if (track && track->GetId() != this->currentTrackId) {
        this->currentTrackId = track->GetId();
        this->currentLyrics = "";
        this->parsedLyrics = support::lyrics::ParsedLyrics();
        this->currentLineIndex = -1;
        this->followPlayback = true;
        this->lyricsSynced = false;
        this->lastDuration = 0.0;
        this->lyricSource = "";
        this->SetState(State::Loading);
        const auto trackExternalId = track->GetString("external_id");
        /* capture up-front: the db callback runs async and the track may
        change underneath us; filename is needed for sidecar lookup. */
        const auto audioFilename = track->GetString("filename");
        auto lyricsDbQuery = std::make_shared<LyricsQuery>(trackExternalId);
        this->library->Enqueue(lyricsDbQuery, [this, lyricsDbQuery, track, audioFilename](auto q) {
            /* 1. embedded lyrics indexed from tags (FLAC/MP4/ID3v2 USLT/SYLT) */
            auto localLyrics = lyricsDbQuery->GetResult();
            if (support::lyrics::HasLyrics(localLyrics)) {
                this->currentLyrics = localLyrics;
                this->lyricSource = "embedded";
                this->Post(message::LyricsLoaded, static_cast<int64_t>(State::Loaded));
                return;
            }
            /* 2. sidecar "<song>.lrc" next to the audio file */
            auto sidecarLyrics = support::lyrics::LoadSidecar(audioFilename);
            if (support::lyrics::HasLyrics(sidecarLyrics)) {
                this->currentLyrics = sidecarLyrics;
                this->lyricSource = ".lrc";
                this->Post(message::LyricsLoaded, static_cast<int64_t>(State::Loaded));
                return;
            }
            /* 3. default: remote lookup (auddio.io, requires API token) */
            auddio::FindLyrics(track, [this](TrackPtr track, std::string remoteLyrics) {
                if (this->currentTrackId == track->GetId()) {
                    this->currentLyrics = remoteLyrics;
                    this->lyricSource = "remote";
                    const auto state = support::lyrics::HasLyrics(remoteLyrics)
                        ? State::Loaded
                        : State::Failed;
                    this->Post(message::LyricsLoaded, static_cast<int64_t>(state));
                }
            });
        });
    }
    else if (!track) {
        this->currentTrackId = -1LL;
        this->SetState(State::NotPlaying);
    }
}

void LyricsLayout::UpdateAdapter() {
    this->adapter->Clear();
    if (this->lyricsSynced) {
        /* synced path: one centered entry per timestamped line; the
        highlight follows playback (see UpdateHighlight). */
        for (auto& line : this->parsedLyrics.lines) {
            this->adapter->AddEntry(std::make_shared<LyricLineEntry>(line.text));
        }
        return;
    }
    /* unsynced path: plain readable lines, as before. */
    std::string fixed = support::lyrics::NormalizeForDisplay(this->currentLyrics);
    str::ReplaceAll(fixed, "\r\n", "\n");
    str::ReplaceAll(fixed, "\r", "\n");
    auto items = str::Split(fixed, "\n");
    for (auto& text : items) {
        this->adapter->AddEntry(std::make_shared<LyricLineEntry>(text));
    }
}

void LyricsLayout::SetState(State state) {
    this->state = state;
    switch (state) {
        case State::NotPlaying: {
                this->listView->Hide();
                this->infoText->Show();
                this->infoText->SetText(_TSTR("lyrics_not_playing"));
                this->currentTrackId = -1LL;
            }
            break;
        case State::Loading: {
                this->listView->Hide();
                this->infoText->Show();
                this->infoText->SetText(_TSTR("lyrics_loading"));
            }
            break;
        case State::Loaded: {
                this->infoText->Hide();
                this->listView->Show();
                if (this->IsVisible()) {
                    this->listView->Focus();
                }
            }
            break;
        case State::Failed: {
                this->listView->Hide();
                this->infoText->Show();
                this->infoText->SetText(u8fmt(
                    _TSTR("lyrics_lookup_failed"),
                    Hotkeys::Get(Hotkeys::LyricsRetry).c_str()));
                this->currentTrackId = -1LL;
            }
            break;
    }
}

void LyricsLayout::SetShortcutsWindow(ShortcutsWindow* shortcuts) {
    if (shortcuts) {
        shortcuts->AddShortcut(Hotkeys::Get(Hotkeys::NavigateLyrics), _TSTR("shortcuts_lyrics"));
        shortcuts->AddShortcut(Hotkeys::Get(Hotkeys::NavigateLibrary), _TSTR("shortcuts_library"));
        shortcuts->AddShortcut(Hotkeys::Get(Hotkeys::NavigateLibraryPlayQueue), _TSTR("shortcuts_play_queue"));
        shortcuts->AddShortcut(App::Instance().GetQuitKey(), _TSTR("shortcuts_quit"));

        shortcuts->SetChangedCallback([this](std::string key) {
            if (key == App::Instance().GetQuitKey()) {
                App::Instance().Quit();
            }
            else {
                this->KeyPress(key);
            }
        });

        shortcuts->SetActive(Hotkeys::Get(Hotkeys::NavigateLyrics));
    }
}
