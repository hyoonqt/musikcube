#pragma once

#include <cursespp/LayoutBase.h>
#include <cursespp/TextLabel.h>
#include <cursespp/ListWindow.h>
#include <cursespp/ITopLevelLayout.h>
#include <cursespp/SimpleScrollAdapter.h>
#include <app/window/TransportWindow.h>
#include <musikcore/audio/PlaybackService.h>
#include <musikcore/library/ILibrary.h>
#include <musikcore/support/Lyrics.h>

namespace musik { namespace cube {

    class LyricsLayout:
        public cursespp::LayoutBase,
        public cursespp::ITopLevelLayout,
        public sigslot::has_slots<>
    {
        public:
            DELETE_CLASS_DEFAULTS(LyricsLayout)

            LyricsLayout(
                musik::core::audio::PlaybackService& playback,
                musik::core::ILibraryPtr library);

            /* IWindow */
            void OnLayout() override;
            void SetShortcutsWindow(cursespp::ShortcutsWindow* w) override;
            bool KeyPress(const std::string& kn) override;
            void OnVisibilityChanged(bool visible) override;
            void ProcessMessage(musik::core::runtime::IMessage &message) override;

        private:
            enum class State: int { NotPlaying, Loading, Loaded, Failed };

            void OnTrackChanged(size_t index, musik::core::TrackPtr track);
            void OnTimeChanged(double seconds);
            void OnLyricsLoaded();

            void SetState(State state);
            void LoadLyricsForCurrentTrack();
            void UpdateAdapter();
            void UpdateHighlight(int64_t positionMs, bool force);
            void ScheduleProgressTick();
            void CenterOnLine(int index);
            void EnsureCursorVisible();
            void MoveCursor(int delta);
            void MoveCursorTo(int index);
            void SeekToCursor();
            void ApplyEntryStates();
            int LineIndexForPosition(int64_t positionMs);

            State state { State::NotPlaying };
            musik::core::ILibraryPtr library;
            musik::core::audio::PlaybackService& playback;
            std::shared_ptr<cursespp::SimpleScrollAdapter> adapter;
            std::shared_ptr<cursespp::ListWindow> listView;
            std::shared_ptr<cursespp::TextLabel> infoText;
            std::shared_ptr<TransportWindow> transportView;
            cursespp::ShortcutsWindow* shortcuts;
            int64_t currentTrackId;
            std::string currentLyrics;
            musik::core::support::lyrics::ParsedLyrics parsedLyrics;
            int currentLineIndex { -1 };
            int cursorIndex { 0 };
            bool followPlayback { true };
            bool focusMode { false };
            bool lyricsSynced { false };
            double lastDuration { 0.0 };
            std::string lyricSource;
            size_t lastLayoutWidth { 0 };
    };

} }