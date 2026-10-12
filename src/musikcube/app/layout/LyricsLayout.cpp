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
#include <musikcore/support/Preferences.h>
#include <app/util/PreferenceKeys.h>
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

    /* same transport bar as the library view: time slider, volume, mode.
    display-only here (FocusNone) so arrows keep scrolling lyrics. */
    this->transportView = std::make_shared<TransportWindow>(library, playback);
    this->transportView->SetFocus(TransportWindow::FocusNone);
    this->AddWindow(this->transportView);

    this->LoadLyricsForCurrentTrack();
}

void LyricsLayout::OnLayout() {
    LayoutBase::OnLayout();
    const int cx = this->GetContentWidth();
    const int cy = this->GetContentHeight();
#ifdef WIN32
    const int transportCy = 3;
#else
    const bool autoHideCommandBar = Preferences::ForComponent("settings")
        ->GetBool(prefs::keys::AutoHideCommandBar, false);
    const int transportCy = (autoHideCommandBar ? 2 : 3);
#endif
    const int listHeight = std::max(0, cy - transportCy);
    if (this->focusMode) {
        /* 3-line karaoke strip, vertically centered in the list area */
        const int stripHeight = 3;
        const int stripY = std::max(0, (listHeight - stripHeight) / 2);
        this->listView->MoveAndResize(0, stripY, cx, stripHeight);
    }
    else {
        this->listView->MoveAndResize(0, 0, cx, listHeight);
    }
    this->transportView->MoveAndResize(1, listHeight, cx - 2, transportCy);
    this->infoText->MoveAndResize(1, listHeight / 2, cx - 2, 1);
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
    size_t target;
    if (this->focusMode) {
        /* 3-line strip: previous/current/next. at the very start the
        strip stays pinned so lines 1+2 fill the first rows. */
        target = index <= 1 ? 0 : (size_t) index - 1;
    }
    else {
        size_t half = visible / 2;
        target = (size_t) index <= half ? 0 : (size_t) index - half;
    }
    size_t maxFirst = count - visible;
    if (target > maxFirst) {
        target = maxFirst;
    }
    this->listView->ScrollTo(target);
}

void LyricsLayout::ApplyEntryStates() {
    /* the cursor (manual navigation) always wins visually; otherwise the
    playback highlight shows. both are tracked independently. */
    const size_t count = this->adapter->GetEntryCount();
    for (size_t i = 0; i < count; i++) {
        auto entry = this->adapter->GetEntry(this->listView.get(), i);
        auto lyric = static_cast<LyricLineEntry*>(entry.get());
        if ((int) i == this->cursorIndex) {
            lyric->SetAttrs(Color(Color::ListItemHighlighted));
        }
        else if ((int) i == this->currentLineIndex) {
            lyric->SetLyricState(LyricLineEntry::State::Current);
        }
        else if (this->currentLineIndex >= 0 && (int) i < this->currentLineIndex) {
            lyric->SetLyricState(LyricLineEntry::State::Past);
        }
        else {
            lyric->SetLyricState(LyricLineEntry::State::Upcoming);
        }
    }
}

void LyricsLayout::EnsureCursorVisible() {
    const size_t count = this->adapter->GetEntryCount();
    if (!count) {
        return;
    }
    if (this->cursorIndex < 0) {
        this->cursorIndex = 0;
    }
    if (this->cursorIndex >= (int) count) {
        this->cursorIndex = (int) count - 1;
    }
    auto pos = this->listView->GetScrollPosition();
    if ((size_t) this->cursorIndex < pos.firstVisibleEntryIndex) {
        this->listView->ScrollTo((size_t) this->cursorIndex);
    }
    else if ((size_t) this->cursorIndex >= pos.firstVisibleEntryIndex + pos.visibleEntryCount) {
        size_t target = (size_t) this->cursorIndex + 1 - pos.visibleEntryCount;
        this->listView->ScrollTo(target);
    }
    else {
        this->listView->Invalidate();
    }
}

void LyricsLayout::MoveCursorTo(int index) {
    const size_t count = this->adapter->GetEntryCount();
    if (!count) {
        return;
    }
    if (index < 0) {
        index = 0;
    }
    if (index >= (int) count) {
        index = (int) count - 1;
    }
    this->followPlayback = false;
    this->cursorIndex = index;
    this->ApplyEntryStates();
    this->EnsureCursorVisible();
}

void LyricsLayout::MoveCursor(int delta) {
    this->MoveCursorTo(this->cursorIndex + delta);
}

void LyricsLayout::SeekToCursor() {
    const size_t count = this->adapter->GetEntryCount();
    if (!count || this->cursorIndex < 0 || this->cursorIndex >= (int) count) {
        return;
    }
    double seconds = -1.0;
    if (this->lyricsSynced && this->cursorIndex < (int) this->parsedLyrics.lines.size()) {
        seconds = (double) this->parsedLyrics.lines[(size_t) this->cursorIndex].ms / 1000.0;
    }
    else if (this->lastDuration > 0.0) {
        /* unsynced estimate: line position mapped onto the duration */
        seconds = (double) this->cursorIndex * this->lastDuration / (double) count;
    }
    if (seconds >= 0.0) {
        /* seeking jumps straight back into follow mode so the highlight
        picks up from the new position. */
        this->followPlayback = true;
        this->playback.SetPosition(seconds);
        this->UpdateHighlight((int64_t) (seconds * 1000.0), true);
    }
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
    /* while following, the cursor rides the highlight; manual navigation
    takes over (and pauses follow) until a seek, retry or new track. */
    if (this->followPlayback && index >= 0) {
        this->cursorIndex = index;
    }
    this->ApplyEntryStates();
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
    this->cursorIndex = 0;
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
    else if (kn == "KEY_ENTER") {
        /* karaoke seek: jump playback to the cursor's line */
        this->SeekToCursor();
        return true;
    }
    else if (kn == "z") {
        /* 3-line focus strip toggle */
        this->focusMode = !this->focusMode;
        this->OnLayout();
        this->UpdateHighlight((int64_t) (this->playback.GetPosition() * 1000.0), true);
        return true;
    }
    else if (kn == "KEY_UP") {
        this->MoveCursor(-1);
        return true;
    }
    else if (kn == "KEY_DOWN") {
        this->MoveCursor(1);
        return true;
    }
    else if (kn == "KEY_PPAGE") {
        const size_t visible = this->listView->GetScrollPosition().visibleEntryCount;
        this->MoveCursor(-(int) (visible ? visible : 10));
        return true;
    }
    else if (kn == "KEY_NPAGE") {
        const size_t visible = this->listView->GetScrollPosition().visibleEntryCount;
        this->MoveCursor((int) (visible ? visible : 10));
        return true;
    }
    else if (kn == "KEY_HOME") {
        this->MoveCursorTo(0);
        return true;
    }
    else if (kn == "KEY_END") {
        this->MoveCursorTo((int) this->adapter->GetEntryCount());
        return true;
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
        shortcuts->AddShortcut("z", _TSTR("shortcuts_lyrics_focus"));
        shortcuts->AddShortcut("KEY_ENTER", _TSTR("shortcuts_lyrics_seek"));
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
