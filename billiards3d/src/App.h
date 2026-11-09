#pragma once

#include "Ai.h"
#include "Camera.h"
#include "Gl.h"
#include "Physics.h"
#include "Prediction.h"
#include "Renderer.h"
#include "Resources.h"
#include "Rules.h"
#include "Settings.h"
#include "Sound.h"
#include "Strings.h"
#include "TableView.h"
#include "Text.h"
#include "UiPainter.h"

#include <SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// The game's window: it runs the matches, the computer players and the interface, and draws them.
class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    int Run();

private:
    enum Mode { VsComputer, TwoPlayers, PracticeMode, Demo };
    enum AimLines { AimFull, AimShort, AimNone };

    // Between strokes the game is ready for one; a computer thinks, then shows its aim; the cue strikes; the balls
    // roll; or the game is over.
    enum class Stage { Ready, Thinking, Presenting, Stroke, Rolling, Over };

    // SDL from the first to the last moment of the game; everything else lives within it.
    struct SdlSession {
        SdlSession();
        ~SdlSession();
        SdlSession(const SdlSession&) = delete;
        SdlSession& operator=(const SdlSession&) = delete;
    };

    struct WindowDeleter {
        void operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
    };
    struct CursorDeleter {
        void operator()(SDL_Cursor* cursor) const { SDL_FreeCursor(cursor); }
    };

    // What the mouse did since the last frame, in pixels.
    struct Pointer {
        float x = -1, y = -1;
        bool leftDown = false, rightDown = false;
        bool leftPressed = false, leftReleased = false, rightPressed = false, rightReleased = false;
        float wheel = 0;
        bool consumed = false; // taken by the interface
    };

    enum class Gesture { None, Strike, PlaceBall, TurnCamera };

    // The card shown over the game, which takes the mouse and the keys while it is open.
    enum class Dialog { None, Help, About };

    // What the pointer does to a widget in this frame.
    struct Interaction {
        bool hover = false, pressed = false, clicked = false;
    };

    struct Toast {
        std::string text;
        float age = 0;
    };

    struct Snapshot {
        billiards::Match match;
        billiards::Simulation sim;
        std::size_t history = 0;
        int shots = 0;
    };

    struct HistoryLine {
        std::string text;
        int kind = 0; // 0 a stroke, 1 a foul, 2 the end of the game
    };

    // The window's size when it is neither maximized nor full screen, in window units.
    struct WindowSize {
        int width = 0, height = 0;
    };

    // A line of the help: the keys, and what they do.
    struct HelpLine {
        std::string_view keys;
        Str action;
    };
    static constexpr int HelpRows = 13; // in each of the help's two columns

    // The help's card: its two columns as wide as the desktop's font needs them, and its rows closer together where
    // the window is too low for them.
    struct HelpLayout {
        float width = 0, height = 0;    // of the card
        float second = 0;               // from the card's padding to the second column
        std::array<float, 2> keys{};    // the width of the keys in each column, before the actions
        float rowHeight = 0;
        std::vector<std::string> rules; // the rules of the game on the table, in lines
    };

    void CreateMainWindow(const resources::Image& icon);
    void HandleEvent(const SDL_Event& event);
    void HandleWindowEvent(const SDL_WindowEvent& event);
    void UpdateSize();
    void ShowLoading();
    void BuildFonts();
    void Frame();
    void Update(float seconds);
    bool NeedsAnimation() const;
    render::Scene BuildScene() const;

    // The game.
    billiards::GameType Type() const { return static_cast<billiards::GameType>(m_game); }
    const billiards::Simulation& Shown() const { return m_stage == Stage::Rolling ? m_live : m_sim; }
    bool IsHuman(int player) const;
    bool HumanToShoot() const;
    bool Aiming() const { return m_stage == Stage::Ready && HumanToShoot() && m_match.MayStrike(m_sim, m_struck); }
    bool OnEight() const { return m_match.OnEight(m_sim, m_match.toPlay); }
    void SetupTable(billiards::TableKind kind);
    bool LoadMatch(const std::string& text);
    void NewGame();
    void StartTurn();
    void DefaultAim();
    billiards::Shot MakeShot() const;
    double SpeedFor(float power) const;
    float PowerFor(double speed) const;
    // Starts the cue's stroke: from where it is drawn back, or first drawing it back.
    void Shoot(const billiards::Shot& shot, bool backswing);
    void Strike();
    void FinishShot();
    void Undo();
    void StopComputer();
    void UpdateComputer(float seconds);
    void UpdateStroke(float seconds);
    void UpdateRolling(float seconds);
    void UpdatePrediction();
    void TurnAim(float radians);
    void FollowAim(bool instant = false);
    void SetGame(int game);
    void SetMode(int mode);
    void SetLevel(int level);
    void SetTheme(int theme);
    void SetAimLines(int lines);
    void SetCamera(int camera);
    void SetSpin(float side, float vertical);
    void SetPower(float power);
    void ToggleSound();
    void ToggleLanguage();
    void ToggleFullScreen();
    float PanAt(math::Vec3 point) const;
    void OpenLink(const char* url);
    void Save();
    void LogShot(int shooter, const billiards::ShotRecord& record, const billiards::Verdict& verdict);
    void AddHistory(std::string text, int kind);

    // Input.
    void PointerAt(int windowX, int windowY);
    void HandleTablePointer();
    void HandleKey(const SDL_KeyboardEvent& event);
    bool PointerRay(math::Vec3& origin, math::Vec3& direction) const;
    std::optional<billiards::V2> PointerOnPlane(float height) const;
    int BallUnderPointer() const;
    int PocketUnderPointer() const;
    void PlaceCueBall(billiards::V2 wanted);

    // The interface.
    float Scale() const { return m_scale; }
    bool DialogOpen() const { return m_dialog != Dialog::None; }
    ui::Rect PanelRect() const;
    ui::Rect FreeArea() const;
    std::vector<render::FrostedPanel> FrostedPanels() const;
    // The text in lines no wider than `width`, broken at the spaces.
    std::vector<std::string> WrapText(ui::FontId font, std::string_view text, float width) const;
    std::array<HelpLine, 2 * HelpRows> HelpLines() const;
    HelpLayout LayOutHelp() const;
    // A card of the size over the game: beside the panel, or over it where the window is too narrow for that.
    ui::Rect CenteredCard(float width, float height) const;
    ui::Rect HelpCard() const;
    ui::Rect AboutCard() const;
    ui::Rect BannerCard() const;
    void DrawInterface();
    void DrawPanel();
    void DrawStatus(ui::Rect area);
    void DrawPlayers(ui::Rect area);
    void DrawStroke(ui::Rect area);
    void DrawHistory(ui::Rect area);
    void DrawBanner();
    // The background of a card over the game.
    void DrawCard(ui::Rect card);
    void DrawHelp();
    void DrawAbout();
    void DrawToasts();
    void DrawBallIcon(float x, float y, float radius, int ball);
    void ToggleDialog(Dialog dialog);
    // A press outside the open card closes it, unless it is the click that opened it.
    bool PressedOutside(ui::Rect card) const;
    // Hovering, pressing and clicking; a click sounds.
    Interaction Interact(int id, ui::Rect rect, bool enabled);
    bool Button(int id, ui::Rect rect, std::string_view label, bool enabled = true, bool accent = false);
    // A label with a value stepped by two arrows, past the width of the longest label of the column; returns -1 or 1
    // when an arrow is clicked, else 0.
    int Selector(int id, ui::Rect rect, std::string_view label, std::string_view value, float labelWidth);
    void ShowToast(std::string text);
    void ShowBanner(std::string title, std::string detail, float seconds);
    std::string_view T(Str id) const { return Tr(id, m_language); }
    std::string Name(int player) const;
    std::string StatusTitle() const;
    std::string StatusDetail() const;
    std::string WinnerTitle() const;
    std::string BallName(int ball) const;
    std::string_view FoulText(billiards::Foul foul) const;
    math::Vec3 Accent() const;

    // Declared in the order of their making; destroyed the other way round, the window after what draws in it.
    SdlSession m_sdl;
    SoundBank m_sound;
    std::unique_ptr<SDL_Window, WindowDeleter> m_window;
    std::unique_ptr<SDL_Cursor, CursorDeleter> m_arrowCursor, m_handCursor;
    std::unique_ptr<gl::Context> m_context;
    std::unique_ptr<render::Renderer> m_renderer;
    std::unique_ptr<ui::Painter> m_painter;
    std::array<std::unique_ptr<text::Font>, 3> m_fonts;

    Settings m_settings;
    Language m_language = Language::English;
    int m_width = 0, m_height = 0;             // of the picture, in pixels
    int m_windowWidth = 0, m_windowHeight = 0; // in window units, which the mouse reports
    float m_textScale = 1;                     // the desktop's
    float m_scale = 1;                         // of the interface: the desktop's, in pixels
    float m_fontScale = 0;                     // that the fonts were made for
    bool m_running = true;
    bool m_dirty = true; // something to draw since the last frame
    bool m_fullScreen = false;
    bool m_maximizedBeforeFullScreen = false;
    WindowSize m_normalSize, m_previousNormalSize;
    std::uint32_t m_normalSizeTime = 0; // when the window last took a size of its own, in SDL's milliseconds
    bool m_cursorHand = false, m_cursorShownHand = false;
    std::chrono::steady_clock::time_point m_lastFrame;
    float m_time = 0;

    // The game.
    billiards::TableSpec m_table; // the simulations point at it
    billiards::Match m_match;
    billiards::Simulation m_sim;  // the balls at rest between strokes
    billiards::Simulation m_live; // the stroke being played
    std::vector<billiards::Event> m_events;
    std::size_t m_heard = 0; // events already sounded
    billiards::Shot m_shot;  // the stroke being played or about to be
    Stage m_stage = Stage::Ready;
    int m_mode = VsComputer;
    int m_level = 3;
    int m_theme = 0;
    int m_game = 0;
    int m_aimLines = AimFull;
    int m_breaker = 0; // who breaks the next game
    std::mt19937_64 m_random;
    std::vector<Snapshot> m_undo;
    std::vector<HistoryLine> m_history;
    int m_shots = 0;
    float m_rollClock = 0;
    float m_overClock = 0;
    TableView m_view;

    // The aim.
    int m_struck = 0;
    billiards::V2 m_aim{1, 0};
    float m_power = 0.4f; // 0 to 1
    float m_side = 0, m_vertical = 0;
    int m_called = -1;
    bool m_calledFixed = false;
    billiards::Prediction m_prediction;
    bool m_predictionDirty = true;
    float m_pull = 0;   // the cue drawn back from the ball, metres
    float m_charge = 0; // the power while the mouse draws the cue back

    // The stroke.
    float m_strokeClock = 0;
    float m_strokeFrom = 0, m_strokeTo = 0;
    float m_strokeBack = 0, m_strokeHold = 0, m_strokeForward = 0.08f;
    float m_follow = 10;        // seconds since the cue met the ball
    billiards::V2 m_strikeSpot; // where the struck ball was

    // The computer.
    billiards::Worker<billiards::Shot> m_computer;
    float m_thinkClock = 0;
    std::optional<billiards::Shot> m_computerShot;
    float m_presentClock = 0;
    billiards::V2 m_presentSpot, m_presentAim;
    float m_presentPower = 0, m_presentSide = 0, m_presentVertical = 0;

    // The view and the input.
    TableCamera m_camera;
    Pointer m_pointer;
    math::Vec2 m_drawnPointer{-1, -1}; // where the pointer was when the last frame was drawn
    Gesture m_gesture = Gesture::None;
    float m_pressX = 0, m_pressY = 0;
    float m_dragX = 0, m_dragY = 0;
    float m_aimX = -1, m_aimY = -1;   // the pointer when it last aimed
    float m_turnX = -1, m_turnY = -1; // the pointer when the turn began
    bool m_pointerAims = false;       // it has moved since, and the aim follows it
    billiards::V2 m_grabOffset;
    int m_hoverBall = -1;
    int m_hoverPocket = -1;

    // The interface.
    int m_activeWidget = 0;
    bool m_panelVisible = true;
    float m_panelSlide = 1;
    Dialog m_dialog = Dialog::None;
    bool m_dialogOpening = false; // opened in this frame
    float m_historyScroll = 0;
    bool m_followHistory = true;
    float m_bannerFade = 0;
    float m_bannerTime = 0; // left to show; below zero it stays
    std::string m_bannerTitle, m_bannerDetail;
    std::vector<Toast> m_toasts;
};
