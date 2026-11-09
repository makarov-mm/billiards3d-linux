#include "App.h"

#include "Desktop.h"
#include "Themes.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <stdexcept>
#include <utility>

using namespace math;
using billiards::V2;
using ui::FontId;
using ui::Rect;
using ui::Rgba;

namespace {

constexpr float PanelWidth = 352, PanelMargin = 16;
constexpr float FullPull = 230;    // pixels the pointer travels to draw the cue back all the way
constexpr float MaxSpin = 0.6f;    // how far from the middle the cue may strike, as a share of the radius
constexpr float MinThink = 0.6f;   // seconds the computer takes over a stroke, at least
constexpr float FollowTime = 0.4f; // the cue stays in sight after striking

enum Widget {
    HelpButton = 1,
    AboutButton,
    NewGameButton,
    UndoButton,
    CameraButton,
    PowerBar,
    SpinBall,
    GameSelector,
    ModeSelector = GameSelector + 2,
    LevelSelector = ModeSelector + 2,
    AimSelector = LevelSelector + 2,
    ThemeSelector = AimSelector + 2,
    SoundButton = ThemeSelector + 2,
    LanguageButton,
    ModalWidgets = 100, // the widgets of the cards over the game
    HelpClose,
    AboutClose,
    AboutLinks = 110,
};

constexpr ui::Color TextColor = Rgba(236, 239, 245);
constexpr ui::Color MutedColor = Rgba(150, 158, 176);
constexpr ui::Color FoulColor = Rgba(255, 146, 124);

// The pool balls' colors, for the icons of the interface.
constexpr std::array<ui::Color, 8> PoolColors{Rgba(248, 184, 22), Rgba(30, 72, 196), Rgba(214, 38, 36), Rgba(104, 48, 164),
                                              Rgba(246, 114, 26), Rgba(26, 134, 68), Rgba(136, 36, 32), Rgba(26, 26, 30)};

V2 Rotate(V2 v, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    return {v.x * c - v.z * s, v.x * s + v.z * c};
}

double AngleOf(V2 v) { return std::atan2(v.z, v.x); }

// How far the cue is drawn back for a stroke of the given power.
float PullFor(float power) { return 0.02f + 0.2f * power; }

// The cue, its tip `pull` behind the point it strikes and raised to clear the cushion and the rail behind the ball.
Mat4 CueMatrix(const billiards::TableSpec& table, Vec3 center, V2 aim, float side, float vertical, float pull)
{
    const float R = static_cast<float>(table.ballRadius);
    const V2 n = billiards::Normalized(aim);
    const Vec3 d{static_cast<float>(n.x), 0, static_cast<float>(n.z)};
    const Vec3 up{0, 1, 0};
    const Vec3 right{-d.z, 0, d.x};
    const float depth = std::sqrt(std::max(0.0f, 1 - side * side - vertical * vertical)) * R;
    const Vec3 contact = center - d * depth + right * (side * R) + up * (vertical * R);

    // How far behind the ball the cushion's nose is.
    const float hl = static_cast<float>(table.length / 2), hw = static_cast<float>(table.width / 2);
    float behind = 10;
    if (d.x < -1e-5f)
        behind = std::min(behind, (hl - center.x) / -d.x);
    else if (d.x > 1e-5f)
        behind = std::min(behind, (center.x + hl) / d.x);
    if (d.z < -1e-5f)
        behind = std::min(behind, (hw - center.z) / -d.z);
    else if (d.z > 1e-5f)
        behind = std::min(behind, (center.z + hw) / d.z);
    behind = std::max(behind - depth, 0.01f);
    const float gap = 0.006f + pull;
    auto radius = [&](float h) { return 0.0065f + 0.006f * std::max(h - gap, 0.0f); };
    const float overCushion = (static_cast<float>(table.cushionHeight) + 0.004f + radius(behind) - contact.y) / behind;
    const float railAt = behind + static_cast<float>(table.cushionWidth);
    const float overRail = (static_cast<float>(table.railHeight) + 0.004f + radius(railAt) - contact.y) / railAt;
    const float elevation = std::clamp(std::atan(std::max(overCushion, overRail)), 0.07f, 0.8f);

    const Vec3 axis = Normalize(d * -std::cos(elevation) + up * std::sin(elevation));
    const Vec3 x = Normalize(Cross(up, axis));
    const Vec3 y = Cross(axis, x);
    const Vec3 tip = contact + axis * gap;
    Mat4 m = Mat4::Identity();
    auto column = [&](int index, Vec3 v) {
        m(0, index) = v.x;
        m(1, index) = v.y;
        m(2, index) = v.z;
    };
    column(0, x);
    column(1, y);
    column(2, axis);
    column(3, tip);
    return m;
}

ui::Color BallColor(int ball, bool russian)
{
    if (russian)
        return ball == 0 ? Rgba(232, 176, 84) : Rgba(236, 228, 206);
    if (ball == 0)
        return Rgba(240, 238, 230);
    return PoolColors[static_cast<std::size_t>((ball - 1) % 8)];
}

// The letter of a key for the shortcuts: by its label in Latin layouts (Z on a German keyboard is where it is
// marked), and by its place on a US keyboard in the others, such as the Russian one, as Windows does.
char ShortcutLetter(const SDL_Keysym& key)
{
    if (key.sym >= SDLK_a && key.sym <= SDLK_z)
        return static_cast<char>(key.sym);
    if (key.sym >= 0x80 && key.scancode >= SDL_SCANCODE_A && key.scancode <= SDL_SCANCODE_Z)
        return static_cast<char>('a' + (key.scancode - SDL_SCANCODE_A));
    return 0;
}

// The computer level of a digit key, on the main keys or the keypad, or 0.
int LevelKey(const SDL_Keysym& key)
{
    if (key.scancode >= SDL_SCANCODE_1 && key.scancode <= SDL_SCANCODE_5)
        return key.scancode - SDL_SCANCODE_1 + 1;
    if (key.scancode >= SDL_SCANCODE_KP_1 && key.scancode <= SDL_SCANCODE_KP_5)
        return key.scancode - SDL_SCANCODE_KP_1 + 1;
    return 0;
}

} // namespace

// Setup --------------------------------------------------------------------------------------------

App::SdlSession::SdlSession()
{
    SDL_SetHint(SDL_HINT_APP_NAME, "3D Billiards");
    // A game in a window rather than a full-screen one: the desktop goes on compositing it, the screen saver may
    // start, and leaving full screen for another window does not minimize it.
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
        throw std::runtime_error(std::format("The display could not be opened: {}", SDL_GetError()));
}

App::SdlSession::~SdlSession() { SDL_Quit(); }

App::App()
{
    m_settings = Settings::Load();
    m_language = m_settings.language;
    m_theme = m_settings.theme;
    m_level = m_settings.level;
    m_mode = m_settings.mode;
    m_game = m_settings.game;
    m_aimLines = m_settings.aim;
    m_breaker = m_settings.breaker;
    m_sound.SetEnabled(m_settings.sound);
    m_panelVisible = m_settings.panel;
    m_panelSlide = m_panelVisible ? 1.0f : 0.0f;
    m_random.seed((static_cast<std::uint64_t>(std::random_device{}()) << 32) ^
                  static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));

    const resources::Image icon = resources::Png("billiards3d.png");
    CreateMainWindow(icon);
    m_context = std::make_unique<gl::Context>(m_window.get());
    render::Program::SetVersion(m_context->Version());
    m_painter = std::make_unique<ui::Painter>();
    m_painter->SetIcon(icon);
    SDL_ShowWindow(m_window.get());
    if (m_settings.maximized)
        SDL_MaximizeWindow(m_window.get());
    UpdateSize();
    BuildFonts();
    ShowLoading();

    m_renderer = std::make_unique<render::Renderer>(m_context->Version());
    m_renderer->SetTheme(Themes()[static_cast<std::size_t>(m_theme)]);
    m_camera.SetMode(static_cast<CameraMode>(m_settings.camera));
    if (LoadMatch(m_settings.match)) {
        // The history of the game, as far as it was kept.
        std::string_view history = m_settings.history;
        while (!history.empty()) {
            const std::size_t end = std::min(history.find('|'), history.size());
            const std::string_view line = history.substr(0, end);
            if (line.size() > 1 && line[0] >= '0' && line[0] <= '2')
                m_history.push_back({std::string(line.substr(1)), line[0] - '0'});
            history.remove_prefix(std::min(end + 1, history.size()));
        }
        m_view.Reset(m_sim, false);
        StartTurn();
        FollowAim(true);
    }
    else {
        NewGame();
    }
    m_lastFrame = std::chrono::steady_clock::now();
}

App::~App()
{
    StopComputer();
}

void App::CreateMainWindow(const resources::Image& icon)
{
    gl::Context::RequestPixelFormat();
    // Hidden until it has its size, which depends on the desktop's scaling, which is known from a window.
    m_window.reset(SDL_CreateWindow(std::string(T(Str::Title)).c_str(), SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1280, 800,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_HIDDEN));
    if (!m_window)
        throw std::runtime_error(std::format("The window could not be created: {}", SDL_GetError()));
    SDL_Window* window = m_window.get();
    desktop::SetIcon(window, icon);
    SDL_StopTextInput(); // keys only: no input method in the way
    m_textScale = desktop::TextScale(window);

    // The last size, or most of the screen's work area, centered in it. Room is left for the frame, so that on a small
    // screen with a large scale the window can be seen whole even at its least size.
    const int display = std::max(0, SDL_GetWindowDisplayIndex(window));
    SDL_Rect work{0, 0, 1920, 1080};
    if (SDL_GetDisplayUsableBounds(display, &work) != 0)
        SDL_GetDisplayBounds(display, &work);
    const auto units = [&](int size) { return static_cast<int>(std::lround(static_cast<float>(size) * m_textScale)); };
    const int roomWidth = work.w * 92 / 100, roomHeight = work.h * 92 / 100;
    int width = m_settings.windowWidth, height = m_settings.windowHeight;
    if (width <= 0 || height <= 0) {
        width = std::min(units(1560), roomWidth);
        height = std::min(units(920), roomHeight);
    }
    width = std::min(width, work.w);
    height = std::min(height, work.h);
    SDL_SetWindowMinimumSize(window, std::min(units(960), roomWidth), std::min(units(680), roomHeight));
    SDL_SetWindowSize(window, width, height);
    SDL_SetWindowPosition(window, work.x + (work.w - width) / 2, work.y + (work.h - height) / 2);
    m_normalSize = m_previousNormalSize = {width, height};

    m_arrowCursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW));
    m_handCursor.reset(SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND));
}

void App::UpdateSize()
{
    SDL_GetWindowSize(m_window.get(), &m_windowWidth, &m_windowHeight);
    SDL_GL_GetDrawableSize(m_window.get(), &m_width, &m_height);
    const float pixelsPerUnit = m_windowWidth > 0 && m_width > 0 ? static_cast<float>(m_width) / static_cast<float>(m_windowWidth) : 1.0f;
    m_scale = std::clamp(m_textScale * pixelsPerUnit, 0.5f, 4.0f);
}

void App::BuildFonts()
{
    const float scale = Scale();
    m_fonts[0] = std::make_unique<text::Font>(15 * scale, text::Weight::Regular);
    m_fonts[1] = std::make_unique<text::Font>(19 * scale, text::Weight::SemiBold);
    m_fonts[2] = std::make_unique<text::Font>(31 * scale, text::Weight::SemiBold);
    m_painter->SetFonts(m_fonts[0].get(), m_fonts[1].get(), m_fonts[2].get());
    m_fontScale = scale;
}

void App::ShowLoading()
{
    gl::glBindFramebuffer(gl::GL_FRAMEBUFFER, 0);
    gl::glViewport(0, 0, m_width, m_height);
    gl::glClearColor(0.014f, 0.016f, 0.02f, 1);
    gl::glClear(gl::GL_COLOR_BUFFER_BIT);
    m_painter->Begin(m_width, m_height);
    const Rect screen{0, 0, static_cast<float>(m_width), static_cast<float>(m_height)};
    m_painter->TextCentered(FontId::Display, screen, T(Str::Title), TextColor);
    m_painter->Flush();
    m_context->SwapBuffers();
}

int App::Run()
{
    SDL_Event event;
    while (m_running) {
        // Nothing moving and nothing new, or nothing to be seen: wait for input instead of drawing the same picture.
        const bool hidden = (SDL_GetWindowFlags(m_window.get()) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
        if (hidden || (!m_dirty && !NeedsAnimation())) {
            if (SDL_WaitEvent(&event))
                HandleEvent(event);
            m_lastFrame = std::chrono::steady_clock::now(); // the idle time is not animation time
        }
        while (SDL_PollEvent(&event))
            HandleEvent(event);
        if (!m_running || hidden || (!m_dirty && !NeedsAnimation()))
            continue;

        m_dirty = false;
        const auto start = std::chrono::steady_clock::now();
        Frame();
        // Swapping the buffers waits for the screen; where it does not, the loop sleeps a little.
        if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(2))
            SDL_Delay(1);
    }
    Save();
    return 0;
}

void App::HandleEvent(const SDL_Event& event)
{
    m_dirty = true;
    switch (event.type) {
    case SDL_QUIT: m_running = false; break;
    case SDL_WINDOWEVENT: HandleWindowEvent(event.window); break;
    case SDL_MOUSEMOTION: PointerAt(event.motion.x, event.motion.y); break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        PointerAt(event.button.x, event.button.y);
        const bool down = event.type == SDL_MOUSEBUTTONDOWN;
        if (event.button.button == SDL_BUTTON_LEFT) {
            m_pointer.leftDown = down;
            (down ? m_pointer.leftPressed : m_pointer.leftReleased) = true;
        }
        else if (event.button.button == SDL_BUTTON_RIGHT) {
            m_pointer.rightDown = down;
            (down ? m_pointer.rightPressed : m_pointer.rightReleased) = true;
        }
        break;
    }
    case SDL_MOUSEWHEEL:
        // Away from the user is positive, in notches; touchpads scroll in fractions of one.
        m_pointer.wheel += event.wheel.preciseY;
        break;
    case SDL_KEYDOWN: HandleKey(event.key); break;
    default: break;
    }
}

void App::HandleWindowEvent(const SDL_WindowEvent& event)
{
    switch (event.event) {
    case SDL_WINDOWEVENT_SIZE_CHANGED:
        UpdateSize();
        if (!m_fullScreen && (SDL_GetWindowFlags(m_window.get()) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN)) == 0) {
            m_previousNormalSize = m_normalSize;
            m_normalSize = {m_windowWidth, m_windowHeight};
            m_normalSizeTime = event.timestamp;
        }
        break;
    case SDL_WINDOWEVENT_MAXIMIZED:
        // The new size may come a moment before the word that the window is maximized: it was not a size of its own.
        if (event.timestamp - m_normalSizeTime < 250)
            m_normalSize = m_previousNormalSize;
        break;
    case SDL_WINDOWEVENT_DISPLAY_CHANGED:
        m_textScale = desktop::TextScale(m_window.get());
        UpdateSize();
        break;
    case SDL_WINDOWEVENT_LEAVE:
        // So that nothing stays highlighted under a pointer that has gone.
        if (!m_pointer.leftDown && !m_pointer.rightDown)
            m_pointer.x = m_pointer.y = -1;
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        // A button let go of in another window is not heard here.
        if (m_pointer.leftDown)
            m_pointer.leftReleased = true;
        if (m_pointer.rightDown)
            m_pointer.rightReleased = true;
        m_pointer.leftDown = m_pointer.rightDown = false;
        break;
    default: break;
    }
}

void App::PointerAt(int windowX, int windowY)
{
    const float pixelsPerUnit = m_windowWidth > 0 ? static_cast<float>(m_width) / static_cast<float>(m_windowWidth) : 1.0f;
    m_pointer.x = static_cast<float>(windowX) * pixelsPerUnit;
    m_pointer.y = static_cast<float>(windowY) * pixelsPerUnit;
}

// The frame ----------------------------------------------------------------------------------------

void App::Frame()
{
    const auto now = std::chrono::steady_clock::now();
    // The balls roll on in real time even when frames are slow; other animations are kept from jumping.
    const float elapsed = std::min(std::chrono::duration<float>(now - m_lastFrame).count(), 0.25f);
    m_lastFrame = now;
    m_time += elapsed;
    if (m_width <= 0 || m_height <= 0)
        return;
    if (m_scale != m_fontScale)
        BuildFonts();
    m_renderer->Resize(m_width, m_height);

    if (m_stage == Stage::Rolling)
        UpdateRolling(elapsed);
    Update(std::min(elapsed, 0.1f));
    m_renderer->Render(BuildScene());

    m_cursorHand = false;
    m_painter->Begin(m_width, m_height);
    DrawInterface();
    m_painter->Flush();
    HandleTablePointer();

    // The widgets are drawn as they were before they heard the click, and the aim, the cue drawn back, the ball in hand,
    // the spin and the power follow the pointer after the frame is drawn: what they changed shows in the next frame.
    if (m_pointer.leftPressed || m_pointer.leftReleased || m_pointer.rightPressed || m_pointer.rightReleased || m_pointer.x != m_drawnPointer.x ||
        m_pointer.y != m_drawnPointer.y)
        m_dirty = true;
    m_drawnPointer = {m_pointer.x, m_pointer.y};
    m_dialogOpening = false;
    m_pointer.leftPressed = m_pointer.leftReleased = m_pointer.rightPressed = m_pointer.rightReleased = false;
    m_pointer.wheel = 0;
    m_pointer.consumed = false;
    if (m_cursorHand != m_cursorShownHand) {
        SDL_SetCursor(m_cursorHand ? m_handCursor.get() : m_arrowCursor.get());
        m_cursorShownHand = m_cursorHand;
    }
    m_context->SwapBuffers();
}

void App::Update(float seconds)
{
    const float slideTarget = m_panelVisible ? 1.0f : 0.0f;
    m_panelSlide += (slideTarget - m_panelSlide) * (1 - std::exp(-seconds * 12));
    if (std::abs(slideTarget - m_panelSlide) < 0.001f)
        m_panelSlide = slideTarget;
    m_camera.SetSpace(FreeArea().width / std::max(1.0f, static_cast<float>(m_width)));

    switch (m_stage) {
    case Stage::Thinking:
    case Stage::Presenting:
        UpdateComputer(seconds);
        break;
    case Stage::Stroke:
        UpdateStroke(seconds);
        break;
    case Stage::Over:
        m_overClock += seconds;
        if (m_mode == Demo && m_overClock > 6)
            NewGame();
        break;
    default:
        break;
    }
    // The cue settles back to the ball when no stroke draws it.
    if (m_stage == Stage::Ready && m_gesture != Gesture::Strike) {
        m_pull *= std::exp(-seconds * 14);
        if (m_pull < 1e-4f)
            m_pull = 0;
    }
    m_follow += seconds;
    m_view.Update(Shown(), seconds);
    UpdatePrediction();
    m_camera.Update(seconds);

    for (Toast& toast : m_toasts)
        toast.age += seconds;
    std::erase_if(m_toasts, [](const Toast& toast) { return toast.age > 2.8f; });
    float bannerTarget = 0;
    if (m_bannerTime < 0) {
        bannerTarget = 1;
    }
    else if (m_bannerTime > 0) {
        bannerTarget = 1;
        m_bannerTime = std::max(0.0f, m_bannerTime - seconds);
    }
    m_bannerFade += (bannerTarget - m_bannerFade) * (1 - std::exp(-seconds * 7));
    if (std::abs(bannerTarget - m_bannerFade) < 0.002f)
        m_bannerFade = bannerTarget;
}

bool App::NeedsAnimation() const
{
    return m_stage == Stage::Thinking || m_stage == Stage::Presenting || m_stage == Stage::Stroke || m_stage == Stage::Rolling ||
           (m_stage == Stage::Over && m_mode == Demo) || m_camera.Moving() || m_view.Animating() || !m_toasts.empty() || m_bannerTime > 0 ||
           (m_bannerFade > 0.001f && m_bannerFade < 0.999f) || std::abs(m_panelSlide - (m_panelVisible ? 1.0f : 0.0f)) > 0.001f ||
           m_follow < FollowTime || (m_pull > 0 && m_gesture != Gesture::Strike);
}

render::Scene App::BuildScene() const
{
    render::Scene scene;
    const float aspect = static_cast<float>(m_width) / std::max(1.0f, static_cast<float>(m_height));
    scene.view = m_camera.View();
    scene.projection = m_camera.Projection(aspect);
    scene.eye = m_camera.Eye();
    scene.time = m_time;
    const billiards::Simulation& sim = Shown();
    const float R = static_cast<float>(m_table.ballRadius);
    const bool aiming = !m_match.Over() && (Aiming() || m_stage == Stage::Presenting || m_stage == Stage::Stroke);
    const bool placing = m_stage == Stage::Presenting && m_shot.place && m_presentClock < 0.7f;

    // The glow at the rims: the ball in hand, the ball chosen to strike, the ball the aim meets.
    std::array<float, billiards::MaxBalls> highlight{};
    if (aiming) {
        if (m_match.inHand != billiards::InHand::None && m_stage != Stage::Stroke)
            highlight[0] = m_gesture == Gesture::PlaceBall || m_hoverBall == 0 ? 1.0f : 0.55f;
        if (Type() == billiards::GameType::Americanka && !m_match.breakShot) {
            highlight[static_cast<std::size_t>(m_struck)] = 0.7f;
            if (m_hoverBall >= 0 && m_hoverBall != m_struck)
                highlight[static_cast<std::size_t>(m_hoverBall)] = std::max(highlight[static_cast<std::size_t>(m_hoverBall)], 0.4f);
        }
        if (m_aimLines != AimNone && m_prediction.hit >= 0 && !placing)
            highlight[static_cast<std::size_t>(m_prediction.hit)] = std::max(highlight[static_cast<std::size_t>(m_prediction.hit)], 0.35f);
    }
    m_view.AddBalls(sim, highlight, scene.balls);

    // The lines of the aim and the ghost ball where the struck ball meets another.
    if (aiming && !placing && m_aimLines != AimNone && sim.balls[static_cast<std::size_t>(m_struck)].OnTable()) {
        const bool full = m_aimLines == AimFull;
        auto add = [&](const std::vector<V2>& points, int segments, double reach) {
            double length = 0;
            for (std::size_t i = 1; i < points.size() && segments > 0 && scene.guides.size() < 16; ++i, --segments) {
                V2 a = points[i - 1], b = points[i];
                const double segment = billiards::Length(b - a);
                if (segment < 1e-6)
                    continue;
                const bool last = length + segment >= reach;
                if (last)
                    b = a + (b - a) * ((reach - length) / segment);
                scene.guides.push_back({static_cast<float>(a.x), static_cast<float>(a.z), static_cast<float>(b.x), static_cast<float>(b.z)});
                length += segment;
                if (last)
                    break;
            }
        };
        const double scale = m_table.length / 2.54;
        add(m_prediction.path, full ? 8 : 3, (full ? 3.0 : 1.0) * scale);
        if (m_prediction.hit >= 0) {
            // Into a rack the lines after the contact would say little.
            if (!m_match.breakShot)
                add(m_prediction.object, full ? 4 : 1, full ? 1.6 * scale : 0.14 * scale);
            if (full && !m_match.breakShot)
                add(m_prediction.after, 4, 0.6 * scale);
            render::BallDraw ghost;
            ghost.model = BallMatrix({static_cast<float>(m_prediction.contact.x), R, static_cast<float>(m_prediction.contact.z)}, billiards::Quat{}, R);
            ghost.number = m_struck;
            ghost.russian = m_table.kind == billiards::TableKind::Pyramid;
            ghost.opacity = 0.3f;
            scene.balls.push_back(ghost);
        }
        scene.guideStrength = m_stage == Stage::Stroke ? 0.6f : m_stage == Stage::Presenting ? 0.75f : 1.0f;
    }

    // The cue.
    if (aiming && !placing && sim.balls[static_cast<std::size_t>(m_struck)].OnTable()) {
        const float pull = m_gesture == Gesture::Strike ? PullFor(m_charge) : m_pull;
        scene.cue = CueMatrix(m_table, m_view.Position(sim, m_struck), m_aim, m_side, m_vertical, pull);
    }
    else if (m_stage == Stage::Rolling && m_follow < FollowTime) {
        const float through = -std::min(m_follow * 0.5f, 0.045f) * (0.4f + PowerFor(m_shot.speed));
        const Vec3 at{static_cast<float>(m_strikeSpot.x), R, static_cast<float>(m_strikeSpot.z)};
        scene.cue = CueMatrix(m_table, at, m_shot.direction, static_cast<float>(m_shot.side), static_cast<float>(m_shot.vertical), through);
    }

    scene.showKitchen = aiming && m_match.inHand == billiards::InHand::Kitchen;
    // The pocket called for the eight, or the one under the pointer.
    if (aiming && OnEight()) {
        const int pocket = m_hoverPocket >= 0 ? m_hoverPocket : m_called;
        if (pocket >= 0) {
            const billiards::Pocket& p = m_table.pockets[static_cast<std::size_t>(pocket)];
            scene.pocketMark = {static_cast<float>(p.hole.x), static_cast<float>(p.hole.z), static_cast<float>(p.cupRadius) + 0.006f,
                                m_hoverPocket >= 0 && m_hoverPocket != m_called ? 0.55f : 1.0f};
        }
    }
    scene.panels = FrostedPanels();
    return scene;
}

// The game -----------------------------------------------------------------------------------------

bool App::IsHuman(int player) const
{
    switch (m_mode) {
    case VsComputer: return player == 0;
    case Demo: return false;
    default: return true;
    }
}

bool App::HumanToShoot() const
{
    return !m_match.Over() && IsHuman(m_match.toPlay);
}

void App::SetupTable(billiards::TableKind kind)
{
    m_table = billiards::TableSpec::Make(kind);
    m_sim = billiards::Simulation(m_table);
    m_live = billiards::Simulation(m_table);
    m_renderer->SetTable(m_table);
    m_view.SetTable(m_table);
    const float reach = static_cast<float>(m_table.length / 2 + m_table.cushionWidth + m_table.railWidth);
    m_camera.SetTable(static_cast<float>(m_table.width), reach);
}

bool App::LoadMatch(const std::string& text)
{
    if (text.empty())
        return false;
    for (const billiards::TableKind kind : {billiards::TableKind::Pool, billiards::TableKind::Pyramid}) {
        const billiards::TableSpec table = billiards::TableSpec::Make(kind);
        billiards::Simulation sim(table);
        billiards::Match match;
        if (!billiards::Deserialize(text, match, sim))
            continue;
        if (match.practice != (m_mode == PracticeMode))
            return false;
        SetupTable(kind);
        billiards::Deserialize(text, m_match, m_sim);
        m_game = static_cast<int>(m_match.type);
        return true;
    }
    return false;
}

void App::NewGame()
{
    StopComputer();
    const billiards::TableKind kind = billiards::TableFor(Type());
    if (m_table.segments.empty() || m_table.kind != kind)
        SetupTable(kind);
    m_match = billiards::Match{};
    m_match.type = Type();
    m_match.practice = m_mode == PracticeMode;
    m_match.toPlay = m_match.practice ? 0 : m_breaker;
    if (!m_match.practice)
        m_breaker = 1 - m_breaker;
    billiards::Rack(Type(), m_sim, m_random);
    m_undo.clear();
    m_history.clear();
    m_shots = 0;
    m_overClock = 0;
    m_gesture = Gesture::None;
    m_view.Reset(m_sim, true);
    m_sound.Play(SoundBank::Effect::Rack, 0.7f);

    static constexpr std::array<Str, 3> games{Str::GameEight, Str::GameNine, Str::GameAmericanka};
    std::string detail;
    const int first = m_match.toPlay + 1;
    if (m_match.practice)
        detail = T(Str::Practice);
    else if (m_mode == VsComputer)
        detail = T(m_match.toPlay == 0 ? Str::BreakYou : Str::BreakComputer);
    else if (m_mode == TwoPlayers)
        detail = std::vformat(T(Str::BreakPlayer), std::make_format_args(first));
    else
        detail = std::vformat(T(Str::BreakComputerN), std::make_format_args(first));
    ShowBanner(std::string(T(games[static_cast<std::size_t>(m_game)])), detail, 2.6f);
    StartTurn();
    FollowAim(true);
    Save();
}

void App::StartTurn()
{
    // The aim suggested for the turn stays until the pointer is moved.
    m_pointerAims = false;
    m_turnX = m_pointer.x;
    m_turnY = m_pointer.y;
    m_called = -1;
    m_calledFixed = false;
    m_side = m_vertical = 0;
    m_pull = m_charge = 0;
    if (m_gesture == Gesture::Strike || m_gesture == Gesture::PlaceBall)
        m_gesture = Gesture::None;
    m_predictionDirty = true;
    if (m_match.Over()) {
        m_stage = Stage::Over;
        return;
    }
    DefaultAim();
    FollowAim();
    if (IsHuman(m_match.toPlay)) {
        m_stage = Stage::Ready;
        return;
    }
    m_stage = Stage::Thinking;
    m_thinkClock = 0;
    m_computerShot.reset();
    const billiards::Simulation position = m_sim;
    const billiards::Match match = m_match;
    const int level = m_level;
    const std::uint64_t seed = m_random();
    m_computer.Start([position, match, level, seed](std::stop_token stop) { return billiards::Ai::Choose(position, match, level, seed, stop); });
}

// Points the cue at the easiest pot there is, or else at the nearest ball that may be hit.
void App::DefaultAim()
{
    const billiards::Simulation& sim = m_sim;
    const int player = m_match.toPlay;
    m_struck = 0;
    if (m_power < 0.05f)
        m_power = 0.4f;
    if (m_match.breakShot) {
        m_aim = billiards::Normalized(m_table.FootSpot() - sim.balls[0].p);
        m_power = 0.92f;
        return;
    }
    std::vector<int> strikers{0};
    if (Type() == billiards::GameType::Americanka) {
        strikers.clear();
        for (int i = 0; i < sim.count; ++i)
            if (m_match.MayStrike(sim, i))
                strikers.push_back(i);
    }
    // In practice any ball may go, the eight too.
    std::vector<int> targets = m_match.Targets(sim, player);
    if (m_match.practice) {
        targets.clear();
        for (int i = 0; i < sim.count; ++i)
            if (sim.balls[static_cast<std::size_t>(i)].OnTable())
                targets.push_back(i);
    }
    double best = -1;
    for (const int struck : strikers) {
        if (!m_match.MayStrike(sim, struck))
            continue;
        for (const int target : targets) {
            if (target == struck)
                continue;
            for (int pocket = 0; pocket < 6; ++pocket) {
                const auto line = billiards::Ai::Line(sim, struck, target, pocket);
                if (line && line->second > best) {
                    best = line->second;
                    m_struck = struck;
                    m_aim = line->first;
                }
            }
        }
    }
    if (best >= 0)
        return;
    double nearest = 1e9;
    for (const int target : targets) {
        if (target == m_struck)
            continue;
        const V2 to = sim.balls[static_cast<std::size_t>(target)].p - sim.balls[static_cast<std::size_t>(m_struck)].p;
        if (billiards::Length(to) < nearest) {
            nearest = billiards::Length(to);
            m_aim = billiards::Normalized(to);
        }
    }
}

double App::SpeedFor(float power) const
{
    const double p = std::clamp(power, 0.0f, 1.0f);
    return billiards::MaxSpeed(m_table.kind) * (0.08 * p + 0.92 * p * p);
}

float App::PowerFor(double speed) const
{
    const double share = std::clamp(speed / billiards::MaxSpeed(m_table.kind), 0.0, 1.0);
    return static_cast<float>((-0.08 + std::sqrt(0.0064 + 4 * 0.92 * share)) / (2 * 0.92));
}

billiards::Shot App::MakeShot() const
{
    billiards::Shot shot;
    shot.struck = m_struck;
    shot.direction = m_aim;
    shot.speed = SpeedFor(m_power);
    shot.side = m_side;
    shot.vertical = m_vertical;
    shot.calledPocket = OnEight() ? m_called : -1;
    return shot;
}

void App::Shoot(const billiards::Shot& shot, bool backswing)
{
    m_shot = shot;
    m_stage = Stage::Stroke;
    m_strokeClock = 0;
    const float power = PowerFor(shot.speed);
    m_strokeFrom = m_pull;
    m_strokeTo = backswing ? PullFor(power) : m_pull;
    m_strokeBack = backswing ? 0.42f : 0.0f;
    m_strokeHold = backswing ? 0.16f : 0.0f;
    m_strokeForward = std::clamp(0.14f - 0.09f * power, 0.05f, 0.14f);
    m_gesture = Gesture::None;
}

void App::UpdateStroke(float seconds)
{
    m_strokeClock += seconds;
    const float t = m_strokeClock;
    if (t < m_strokeBack) {
        m_pull = Lerp(m_strokeFrom, m_strokeTo, SmoothStep(0, 1, t / m_strokeBack));
        return;
    }
    if (t < m_strokeBack + m_strokeHold) {
        m_pull = m_strokeTo;
        return;
    }
    const float s = (t - m_strokeBack - m_strokeHold) / m_strokeForward;
    if (s < 1) {
        m_pull = m_strokeTo * (1 - s * s);
        return;
    }
    m_pull = 0;
    Strike();
}

// The cue meets the ball: the balls are played out from here, frame by frame.
void App::Strike()
{
    m_undo.push_back({m_match, m_sim, m_history.size(), m_shots});
    if (m_undo.size() > 400)
        m_undo.erase(m_undo.begin());
    m_live = m_sim;
    m_live.turnBalls = true;
    m_live.time = 0;
    m_events.clear();
    m_heard = 0;
    const billiards::Ball& ball = m_sim.balls[static_cast<std::size_t>(m_shot.struck)];
    m_strikeSpot = ball.p;
    m_live.Strike(m_shot.struck, m_shot.direction, m_shot.speed, m_shot.side, m_shot.vertical);
    m_stage = Stage::Rolling;
    m_rollClock = 0;
    m_follow = 0;
    const float power = PowerFor(m_shot.speed);
    const float R = static_cast<float>(m_table.ballRadius);
    m_sound.Play(SoundBank::Effect::Cue, 0.3f + 0.7f * power, PanAt({static_cast<float>(ball.p.x), R, static_cast<float>(ball.p.z)}), 0.94f + 0.12f * power);
    m_camera.Lift();
}

float App::PanAt(Vec3 point) const
{
    Vec2 screen;
    if (!m_camera.ToScreen(point, static_cast<float>(m_width), static_cast<float>(m_height), screen))
        return 0;
    return std::clamp((screen.x / std::max(1.0f, static_cast<float>(m_width)) * 2 - 1) * 0.7f, -0.8f, 0.8f);
}

void App::UpdateRolling(float seconds)
{
    m_rollClock += seconds;
    m_live.Advance(seconds, &m_events);
    const float R = static_cast<float>(m_table.ballRadius);
    std::uniform_real_distribution<float> vary(0.94f, 1.07f);
    int sounded = 0;
    for (; m_heard < m_events.size(); ++m_heard) {
        const billiards::Event& event = m_events[m_heard];
        if (++sounded > 8)
            continue; // a crowd at once: the loudest are heard anyway
        const billiards::Ball& ball = m_live.balls[static_cast<std::size_t>(event.a)];
        const float pan = PanAt({static_cast<float>(ball.p.x), R, static_cast<float>(ball.p.z)});
        const float speed = static_cast<float>(event.speed);
        switch (event.type) {
        case billiards::EventType::BallBall:
            m_sound.Play(SoundBank::Effect::Ball, std::clamp(speed / 2.6f, 0.03f, 1.0f), pan,
                         vary(m_random) * (m_table.kind == billiards::TableKind::Pyramid ? 0.88f : 1.0f));
            break;
        case billiards::EventType::Cushion:
            m_sound.Play(SoundBank::Effect::Cushion, std::clamp(speed / 3.0f, 0.03f, 0.9f), pan, vary(m_random));
            break;
        case billiards::EventType::Pocket:
            m_sound.Play(m_table.kind == billiards::TableKind::Pyramid ? SoundBank::Effect::Net : SoundBank::Effect::Pocket,
                         std::clamp(0.45f + speed / 4, 0.4f, 1.0f), pan, vary(m_random));
            break;
        }
    }
    if (!m_live.Moving() || m_rollClock > 60)
        FinishShot();
}

void App::FinishShot()
{
    for (int i = 0; i < m_live.count; ++i) {
        // Anything still creeping after a minute is stopped where it is.
        billiards::Ball& ball = m_live.balls[static_cast<std::size_t>(i)];
        ball.v = {};
        ball.w = {};
    }
    const int shooter = m_match.toPlay;
    const billiards::ShotRecord record = billiards::Record(m_shot.struck, m_events, m_shot.calledPocket);
    const billiards::Verdict verdict = billiards::Judge(m_match, m_live, record);
    m_sim = m_live;
    m_sim.time = 0;
    ++m_shots;
    LogShot(shooter, record, verdict);

    if (verdict.foul != billiards::Foul::None && !m_match.practice) {
        const std::string_view foul = FoulText(verdict.foul);
        ShowToast(std::vformat(T(Str::Foul), std::make_format_args(foul)));
        if (!m_match.Over())
            m_sound.Play(SoundBank::Effect::Foul, 0.8f);
    }
    else if (verdict.assigned != billiards::Group::Open) {
        const std::string name = Name(shooter);
        ShowToast(std::vformat(T(verdict.assigned == billiards::Group::Solids ? Str::LogSolids : Str::LogStripes), std::make_format_args(name)));
    }

    if (m_match.Over()) {
        m_stage = Stage::Over;
        m_overClock = 0;
        std::string detail;
        if (Type() == billiards::GameType::Americanka && !m_match.practice) {
            detail = std::format("{} : {}", m_match.Score(0), m_match.Score(1));
        }
        else if (verdict.foul != billiards::Foul::None) {
            const std::string_view foul = FoulText(verdict.foul);
            detail = std::vformat(T(Str::Foul), std::make_format_args(foul));
        }
        else if (m_match.practice) {
            detail = std::vformat(T(Str::Shots), std::make_format_args(m_shots));
        }
        ShowBanner(WinnerTitle(), detail, -1);
        m_sound.Play(SoundBank::Effect::Win, 0.9f);
        m_gesture = Gesture::None;
        Save();
        return;
    }
    StartTurn();
    Save();
}

void App::Undo()
{
    if (m_undo.empty() || m_mode == Demo) {
        ShowToast(std::string(T(Str::NothingToUndo)));
        return;
    }
    StopComputer();
    // Back to the last stroke a person played, past the computer's since.
    while (m_undo.size() > 1 && !IsHuman(m_undo.back().match.toPlay))
        m_undo.pop_back();
    const Snapshot snapshot = m_undo.back();
    m_undo.pop_back();
    m_match = snapshot.match;
    m_sim = snapshot.sim;
    m_history.resize(std::min(m_history.size(), snapshot.history));
    m_shots = snapshot.shots;
    m_view.Reset(m_sim, false);
    m_bannerTime = 0;
    m_gesture = Gesture::None;
    StartTurn();
    Save();
}

void App::StopComputer()
{
    m_computer.Cancel();
    m_computerShot.reset();
    if (m_stage == Stage::Presenting && m_shot.place)
        m_sim.balls[0].p = m_presentSpot;
    if (m_stage == Stage::Thinking || m_stage == Stage::Presenting)
        m_stage = Stage::Ready;
}

void App::UpdateComputer(float seconds)
{
    if (m_stage == Stage::Thinking) {
        m_thinkClock += seconds;
        if (auto shot = m_computer.Take())
            m_computerShot = *shot;
        if (!m_computerShot || m_thinkClock < MinThink)
            return;
        m_shot = *m_computerShot;
        m_computerShot.reset();
        m_stage = Stage::Presenting;
        m_presentClock = 0;
        m_presentSpot = m_sim.balls[0].p;
        m_presentAim = m_aim;
        m_presentPower = m_power;
        m_presentSide = m_side;
        m_presentVertical = m_vertical;
        m_struck = m_shot.struck;
        m_called = m_shot.calledPocket;
        m_predictionDirty = true;
        return;
    }

    // It puts the ball down, turns the cue to its line and sets the spin and the power, then strikes.
    m_presentClock += seconds;
    const float placeTime = m_shot.place ? 0.7f : 0.0f;
    if (m_shot.place) {
        const float s = SmoothStep(0, 1, m_presentClock / placeTime);
        m_sim.balls[0].p = m_presentSpot + (m_shot.spot - m_presentSpot) * s;
        if (m_presentClock < placeTime)
            return;
    }
    const double from = AngleOf(m_presentAim);
    const double turn = std::remainder(AngleOf(m_shot.direction) - from, 2 * std::numbers::pi);
    const float aimTime = 0.3f + 0.5f * static_cast<float>(std::min(1.0, std::abs(turn) / 1.6));
    const float t = SmoothStep(0, 1, (m_presentClock - placeTime) / aimTime);
    m_aim = {std::cos(from + turn * t), std::sin(from + turn * t)};
    m_power = Lerp(m_presentPower, PowerFor(m_shot.speed), t);
    m_side = Lerp(m_presentSide, static_cast<float>(m_shot.side), t);
    m_vertical = Lerp(m_presentVertical, static_cast<float>(m_shot.vertical), t);
    m_predictionDirty = true;
    FollowAim();
    if (m_presentClock >= placeTime + aimTime + 0.25f) {
        m_aim = billiards::Normalized(m_shot.direction);
        Shoot(m_shot, true);
    }
}

void App::UpdatePrediction()
{
    const bool showing = (m_stage == Stage::Ready && !m_match.Over()) || m_stage == Stage::Presenting;
    if (!showing || !m_predictionDirty)
        return;
    m_predictionDirty = false;
    if (!m_sim.balls[static_cast<std::size_t>(m_struck)].OnTable()) {
        m_prediction = {};
        return;
    }
    const double scale = m_table.length / 2.54;
    m_prediction = billiards::Predict(m_sim, m_struck, m_aim, SpeedFor(m_power), m_side, m_vertical, 0.6 * scale, 1.6 * scale);
    // The pocket for the eight: where the aim sends it, unless one was chosen.
    if (OnEight() && !m_calledFixed && m_stage == Stage::Ready) {
        m_called = -1;
        if (m_prediction.hit == 8 && m_prediction.object.size() >= 2) {
            const V2 start = m_prediction.object.front(), next = m_prediction.object[1];
            m_called = m_prediction.objectPocket >= 0 ? m_prediction.objectPocket : billiards::PocketAlong(m_table, start, next - start);
        }
    }
}

void App::TurnAim(float radians)
{
    m_aim = billiards::Normalized(Rotate(m_aim, radians));
    m_predictionDirty = true;
    FollowAim();
}

void App::FollowAim(bool instant)
{
    const Vec3 ball = m_view.Position(m_sim, m_struck);
    m_camera.Follow(ball, {static_cast<float>(m_aim.x), static_cast<float>(m_aim.z)}, instant);
}

void App::SetGame(int game)
{
    m_game = (game + billiards::GameTypeCount) % billiards::GameTypeCount;
    NewGame();
}

void App::SetMode(int mode)
{
    mode = (mode + 4) % 4;
    const bool practiceChanges = (mode == PracticeMode) != (m_mode == PracticeMode);
    m_mode = mode;
    if (practiceChanges) {
        NewGame();
        return;
    }
    if ((m_stage == Stage::Thinking || m_stage == Stage::Presenting) && IsHuman(m_match.toPlay)) {
        StopComputer();
        StartTurn();
    }
    else if (m_stage == Stage::Ready && !IsHuman(m_match.toPlay)) {
        StartTurn();
    }
    Save();
}

void App::SetLevel(int level)
{
    m_level = std::clamp(level, 1, 5);
    Save();
}

void App::SetTheme(int theme)
{
    m_theme = (theme + ThemeCount) % ThemeCount;
    m_renderer->SetTheme(Themes()[static_cast<std::size_t>(m_theme)]);
    Save();
}

void App::SetAimLines(int lines)
{
    m_aimLines = (lines + 3) % 3;
    m_predictionDirty = true;
    Save();
}

void App::SetCamera(int camera)
{
    m_camera.SetMode(static_cast<CameraMode>((camera + CameraModeCount) % CameraModeCount));
    FollowAim();
    Save();
}

void App::SetSpin(float side, float vertical)
{
    const float length = std::sqrt(side * side + vertical * vertical);
    if (length > MaxSpin) {
        side *= MaxSpin / length;
        vertical *= MaxSpin / length;
    }
    m_side = std::abs(side) < 1e-3f ? 0.0f : side;
    m_vertical = std::abs(vertical) < 1e-3f ? 0.0f : vertical;
    m_predictionDirty = true;
}

void App::SetPower(float power)
{
    m_power = std::clamp(power, 0.02f, 1.0f);
    m_predictionDirty = true;
}

void App::ToggleSound()
{
    m_sound.SetEnabled(!m_sound.Enabled());
    Save();
}

void App::ToggleLanguage()
{
    m_language = m_language == Language::English ? Language::Russian : Language::English;
    SDL_SetWindowTitle(m_window.get(), std::string(T(Str::Title)).c_str());
    Save();
}

void App::ToggleFullScreen()
{
    const bool fullScreen = !m_fullScreen;
    if (fullScreen)
        m_maximizedBeforeFullScreen = (SDL_GetWindowFlags(m_window.get()) & SDL_WINDOW_MAXIMIZED) != 0;
    // Borderless over the whole screen, at its own resolution.
    if (SDL_SetWindowFullscreen(m_window.get(), fullScreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0)
        m_fullScreen = fullScreen;
}

void App::OpenLink(const char* url)
{
    if (SDL_OpenURL(url) == 0)
        return;
    // Where no browser could be started, the link goes to the clipboard instead.
    SDL_SetClipboardText(url);
    ShowToast(std::string(T(Str::LinkCopied)));
}

void App::Save()
{
    m_settings.theme = m_theme;
    m_settings.level = m_level;
    m_settings.mode = m_mode;
    m_settings.game = m_game;
    m_settings.aim = m_aimLines;
    m_settings.camera = static_cast<int>(m_camera.Mode());
    m_settings.breaker = m_breaker;
    m_settings.sound = m_sound.Enabled();
    m_settings.panel = m_panelVisible;
    m_settings.language = m_language;
    m_settings.match = m_sim.count > 0 ? billiards::Serialize(m_match, m_sim) : std::string();
    m_settings.history.clear();
    const std::size_t first = m_history.size() > 60 ? m_history.size() - 60 : 0;
    for (std::size_t i = first; i < m_history.size(); ++i)
        m_settings.history += std::format("{}{}{}", i > first ? "|" : "", m_history[i].kind, m_history[i].text);
    if (m_window) {
        const std::uint32_t flags = SDL_GetWindowFlags(m_window.get());
        if (!m_fullScreen && (flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED | SDL_WINDOW_FULLSCREEN)) == 0)
            SDL_GetWindowSize(m_window.get(), &m_normalSize.width, &m_normalSize.height);
        m_settings.windowWidth = m_normalSize.width;
        m_settings.windowHeight = m_normalSize.height;
        m_settings.maximized = m_fullScreen ? m_maximizedBeforeFullScreen : (flags & SDL_WINDOW_MAXIMIZED) != 0;
    }
    m_settings.Save();
}

void App::AddHistory(std::string text, int kind)
{
    m_history.push_back({std::move(text), kind});
    m_followHistory = true;
}

void App::LogShot(int shooter, const billiards::ShotRecord& record, const billiards::Verdict& verdict)
{
    const std::string name = Name(shooter);
    if (verdict.foul != billiards::Foul::None && !m_match.practice) {
        const std::string_view foul = FoulText(verdict.foul);
        AddHistory(std::vformat(T(Str::LogFoul), std::make_format_args(name, foul)), 1);
    }
    else {
        std::string potted;
        int count = 0;
        for (const auto& [ball, pocket] : record.pocketed) {
            if (ball == 0 && Type() != billiards::GameType::Americanka)
                continue;
            potted += (count++ > 0 ? ", " : "") + BallName(ball);
        }
        if (count == 0)
            AddHistory(std::vformat(T(Str::LogNothing), std::make_format_args(name)), 0);
        else
            AddHistory(std::vformat(T(count == 1 ? Str::LogPotted : Str::LogPottedMany), std::make_format_args(name, potted)), 0);
    }
    if (verdict.assigned != billiards::Group::Open)
        AddHistory(std::vformat(T(verdict.assigned == billiards::Group::Solids ? Str::LogSolids : Str::LogStripes), std::make_format_args(name)), 0);
    if (!verdict.respot.empty()) {
        std::string balls;
        for (const int ball : verdict.respot)
            balls += (balls.empty() ? "" : ", ") + BallName(ball);
        AddHistory(std::vformat(T(Str::LogBack), std::make_format_args(balls)), 1);
    }
    if (m_match.Over()) {
        const std::string winner = Name(m_match.winner);
        AddHistory(std::vformat(T(Str::LogWin), std::make_format_args(winner)), 2);
    }
}

// Input --------------------------------------------------------------------------------------------

bool App::PointerRay(Vec3& origin, Vec3& direction) const
{
    if (m_pointer.x < 0 || m_pointer.y < 0)
        return false;
    m_camera.Ray(m_pointer.x, m_pointer.y, static_cast<float>(m_width), static_cast<float>(m_height), origin, direction);
    return true;
}

std::optional<V2> App::PointerOnPlane(float height) const
{
    Vec3 origin, direction;
    if (!PointerRay(origin, direction) || direction.y > -1e-4f)
        return std::nullopt;
    const float t = (height - origin.y) / direction.y;
    if (t < 0)
        return std::nullopt;
    const Vec3 p = origin + direction * t;
    return V2{p.x, p.z};
}

int App::BallUnderPointer() const
{
    Vec3 origin, direction;
    if (!PointerRay(origin, direction))
        return -1;
    const float R = static_cast<float>(m_table.ballRadius) * 1.3f;
    float nearest = 1e9f;
    int found = -1;
    for (int i = 0; i < m_sim.count; ++i) {
        if (!m_sim.balls[static_cast<std::size_t>(i)].OnTable())
            continue;
        const Vec3 offset = origin - m_view.Position(m_sim, i);
        const float b = Dot(offset, direction);
        const float c = Dot(offset, offset) - R * R;
        const float discriminant = b * b - c;
        if (discriminant < 0)
            continue;
        const float t = -b - std::sqrt(discriminant);
        if (t > 0 && t < nearest) {
            nearest = t;
            found = i;
        }
    }
    return found;
}

int App::PocketUnderPointer() const
{
    const auto p = PointerOnPlane(0);
    if (!p)
        return -1;
    for (int i = 0; i < 6; ++i) {
        const billiards::Pocket& pocket = m_table.pockets[static_cast<std::size_t>(i)];
        if (billiards::Length(*p - pocket.hole) < pocket.cupRadius * 1.35)
            return i;
    }
    return -1;
}

// The ball in hand where the pointer wants it, or as near as it may go: on the cloth, clear of the other balls,
// and behind the head string when it must be.
void App::PlaceCueBall(V2 wanted)
{
    billiards::Ball& cue = m_sim.balls[0];
    const double R = m_table.ballRadius;
    if (m_match.inHand == billiards::InHand::Kitchen)
        wanted.x = std::min(wanted.x, m_table.HeadString() - 0.002);
    wanted.x = std::clamp(wanted.x, -m_table.length / 2 + R, m_table.length / 2 - R);
    wanted.z = std::clamp(wanted.z, -m_table.width / 2 + R, m_table.width / 2 - R);
    if (m_sim.FreeSpot(wanted, 0)) {
        cue.p = wanted;
    }
    else if (m_sim.FreeSpot(cue.p, 0)) {
        V2 good = cue.p, bad = wanted;
        for (int i = 0; i < 16; ++i) {
            const V2 middle = (good + bad) * 0.5;
            (m_sim.FreeSpot(middle, 0) ? good : bad) = middle;
        }
        cue.p = good;
    }
    cue.v = {};
    cue.w = {};
    m_predictionDirty = true;
}

void App::HandleTablePointer()
{
    const float scale = Scale();
    const bool blocked = m_pointer.consumed || DialogOpen();
    const bool inside = m_pointer.x >= 0 && m_pointer.y >= 0 && m_pointer.x < static_cast<float>(m_width) && m_pointer.y < static_cast<float>(m_height);
    const bool aiming = Aiming();
    const float R = static_cast<float>(m_table.ballRadius);

    // What is under the pointer: the ball in hand to pick up, a ball to strike with, a pocket to call.
    const int hoverBall = m_hoverBall, hoverPocket = m_hoverPocket;
    const bool looking = inside && !blocked && aiming && (m_gesture == Gesture::None || m_gesture == Gesture::Strike);
    m_hoverBall = looking ? BallUnderPointer() : -1;
    m_hoverPocket = looking && OnEight() ? PocketUnderPointer() : -1;
    if (m_hoverBall >= 0 && m_hoverPocket >= 0)
        m_hoverPocket = -1;
    if (m_hoverBall != hoverBall || m_hoverPocket != hoverPocket)
        m_dirty = true;
    const bool grabbable = aiming && m_match.inHand != billiards::InHand::None && m_hoverBall == 0;
    const bool pickable = aiming && Type() == billiards::GameType::Americanka && !m_match.breakShot && m_hoverBall >= 0 && m_hoverBall != m_struck;
    if (grabbable || pickable || m_hoverPocket >= 0 || m_gesture == Gesture::PlaceBall)
        m_cursorHand = true;

    if (!blocked && m_pointer.leftPressed && m_gesture == Gesture::None) {
        m_pressX = m_dragX = m_pointer.x;
        m_pressY = m_dragY = m_pointer.y;
        if (grabbable) {
            m_gesture = Gesture::PlaceBall;
            const auto p = PointerOnPlane(R);
            m_grabOffset = p ? m_sim.balls[0].p - *p : V2{};
        }
        else if (aiming) {
            m_gesture = Gesture::Strike;
            m_charge = 0;
        }
        else {
            m_gesture = Gesture::TurnCamera;
        }
    }
    if (!blocked && m_pointer.rightPressed) {
        if (m_gesture == Gesture::Strike) {
            // A right click while drawing the cue back takes the stroke back.
            m_charge = 0;
            m_gesture = Gesture::None;
        }
        if (m_gesture == Gesture::None) {
            m_gesture = Gesture::TurnCamera;
            m_pressX = m_dragX = m_pointer.x;
            m_pressY = m_dragY = m_pointer.y;
        }
    }

    const float travel = std::hypot(m_pointer.x - m_pressX, m_pointer.y - m_pressY);
    switch (m_gesture) {
    case Gesture::PlaceBall:
        if (const auto p = PointerOnPlane(R))
            PlaceCueBall(*p + m_grabOffset);
        break;
    case Gesture::Strike:
        m_charge = std::clamp((travel - 4 * scale) / (FullPull * scale), 0.0f, 1.0f);
        break;
    case Gesture::TurnCamera:
        m_camera.Rotate(-(m_pointer.x - m_dragX) * 0.0055f / scale, (m_pointer.y - m_dragY) * 0.0045f / scale);
        m_dragX = m_pointer.x;
        m_dragY = m_pointer.y;
        break;
    default:
        break;
    }

    if (m_pointer.leftReleased) {
        if (m_gesture == Gesture::Strike) {
            if (travel < 5 * scale) {
                // A click: a ball to strike with, or the pocket for the eight.
                if (pickable && m_match.MayStrike(m_sim, m_hoverBall)) {
                    m_struck = m_hoverBall;
                    m_predictionDirty = true;
                    FollowAim();
                    m_sound.Play(SoundBank::Effect::Click);
                }
                else if (m_hoverPocket >= 0) {
                    m_calledFixed = !(m_calledFixed && m_called == m_hoverPocket);
                    m_called = m_hoverPocket;
                    m_predictionDirty = true;
                    m_sound.Play(SoundBank::Effect::Click);
                }
            }
            else if (m_charge >= 0.03f) {
                m_power = m_charge;
                m_pull = PullFor(m_charge);
                Shoot(MakeShot(), false);
            }
            m_charge = 0;
            if (m_gesture == Gesture::Strike)
                m_gesture = Gesture::None;
        }
        else if (m_gesture == Gesture::PlaceBall) {
            m_gesture = Gesture::None;
            FollowAim();
        }
        else if (m_gesture == Gesture::TurnCamera && !m_pointer.rightDown) {
            m_gesture = Gesture::None;
        }
    }
    if (m_pointer.rightReleased && m_gesture == Gesture::TurnCamera && !m_pointer.leftDown)
        m_gesture = Gesture::None;

    // With no button held the aim follows the pointer: to the point under it, or with Shift turned finely; but not
    // before the pointer has been moved in this turn.
    if (m_turnX < 0) {
        m_turnX = m_pointer.x;
        m_turnY = m_pointer.y;
    }
    if (std::hypot(m_pointer.x - m_turnX, m_pointer.y - m_turnY) > 3 * scale)
        m_pointerAims = true;
    const bool moved = m_pointer.x != m_aimX || m_pointer.y != m_aimY;
    if (Aiming() && m_gesture == Gesture::None && !blocked && inside && moved && m_pointerAims) {
        const billiards::Ball& ball = m_sim.balls[static_cast<std::size_t>(m_struck)];
        if ((SDL_GetModState() & KMOD_SHIFT) != 0 && m_aimX >= 0) {
            const Vec3 center = m_view.Position(m_sim, m_struck);
            const V2 turned = Rotate(m_aim, 0.01);
            Vec2 a, b;
            const float w = static_cast<float>(m_width), h = static_cast<float>(m_height);
            if (m_camera.ToScreen(center + Vec3{static_cast<float>(m_aim.x), 0, static_cast<float>(m_aim.z)} * 0.5f, w, h, a) &&
                m_camera.ToScreen(center + Vec3{static_cast<float>(turned.x), 0, static_cast<float>(turned.z)} * 0.5f, w, h, b)) {
                const Vec2 tangent = b - a;
                const float length = Length(tangent);
                if (length > 1e-3f) {
                    const float along = Dot(Vec2{m_pointer.x - m_aimX, m_pointer.y - m_aimY}, tangent * (1 / length));
                    m_aim = billiards::Normalized(Rotate(m_aim, along * 0.0004f / scale));
                    m_predictionDirty = true;
                }
            }
        }
        else if (const auto p = PointerOnPlane(R); p && billiards::Length(*p - ball.p) > 0.6 * R) {
            m_aim = billiards::Normalized(*p - ball.p);
            m_predictionDirty = true;
        }
    }
    m_aimX = m_pointer.x;
    m_aimY = m_pointer.y;

    if (!blocked && m_pointer.wheel != 0)
        m_camera.Zoom(m_pointer.wheel);
}

void App::HandleKey(const SDL_KeyboardEvent& event)
{
    const SDL_Keysym& key = event.keysym;
    const bool control = (key.mod & KMOD_CTRL) != 0;
    const bool shift = (key.mod & KMOD_SHIFT) != 0;
    const bool alt = (key.mod & KMOD_ALT) != 0;
    const bool repeat = event.repeat != 0;
    const char letter = ShortcutLetter(key);

    if (alt) {
        if ((key.sym == SDLK_RETURN || key.sym == SDLK_KP_ENTER) && !repeat)
            ToggleFullScreen();
        return; // the rest of Alt is the desktop's
    }
    if (DialogOpen()) {
        // Over a card only its own keys work, and full screen.
        if (repeat || control)
            return;
        if (key.sym == SDLK_ESCAPE)
            m_dialog = Dialog::None;
        else if (key.sym == SDLK_F1)
            ToggleDialog(Dialog::Help);
        else if (letter == 'i')
            ToggleDialog(Dialog::About);
        else if (key.sym == SDLK_F11)
            ToggleFullScreen();
        return;
    }
    if (control) {
        if (letter == 'z')
            Undo();
        else if (letter == 'n' && !repeat)
            NewGame();
        return;
    }

    // The aim, the power, the spin and taking back go on while a key is held; the other keys act once.
    const bool aiming = Aiming() && m_gesture == Gesture::None;
    switch (key.sym) {
    case SDLK_UP: SetPower(m_power + (shift ? 0.005f : 0.025f)); return;
    case SDLK_DOWN: SetPower(m_power - (shift ? 0.005f : 0.025f)); return;
    case SDLK_LEFT:
    case SDLK_RIGHT:
        if (aiming)
            TurnAim((key.sym == SDLK_RIGHT ? 1.0f : -1.0f) * (shift ? 0.0004f : 0.005f));
        return;
    case SDLK_BACKSPACE: Undo(); return;
    default: break;
    }
    switch (letter) {
    case 'w': SetSpin(m_side, m_vertical + 0.1f); return;
    case 's': SetSpin(m_side, m_vertical - 0.1f); return;
    case 'a': SetSpin(m_side - 0.1f, m_vertical); return;
    case 'd': SetSpin(m_side + 0.1f, m_vertical); return;
    default: break;
    }
    if (repeat)
        return;

    switch (key.sym) {
    case SDLK_SPACE:
        if (aiming)
            Shoot(MakeShot(), true);
        return;
    case SDLK_HOME:
        m_camera.Reset();
        FollowAim();
        return;
    case SDLK_TAB:
        m_panelVisible = !m_panelVisible;
        Save();
        return;
    case SDLK_F1: ToggleDialog(Dialog::Help); return;
    case SDLK_F11: ToggleFullScreen(); return;
    case SDLK_ESCAPE:
        if (m_gesture == Gesture::Strike) {
            m_gesture = Gesture::None;
            m_charge = 0;
        }
        else if (m_fullScreen) {
            ToggleFullScreen();
        }
        return;
    default: break;
    }
    switch (letter) {
    case 'x': SetSpin(0, 0); return;
    case 'c': SetCamera(static_cast<int>(m_camera.Mode()) + (shift ? -1 : 1)); return;
    case 'g': SetAimLines(m_aimLines + (shift ? -1 : 1)); return;
    case 'n': NewGame(); return;
    case 'b': SetGame(m_game + (shift ? -1 : 1)); return;
    case 'm': SetMode(m_mode + (shift ? -1 : 1)); return;
    case 't': SetTheme(m_theme + (shift ? -1 : 1)); return;
    case 'v': ToggleSound(); return;
    case 'l': ToggleLanguage(); return;
    case 'i': ToggleDialog(Dialog::About); return;
    default: break;
    }
    if (const int level = LevelKey(key))
        SetLevel(level);
}

// The interface ------------------------------------------------------------------------------------

Vec3 App::Accent() const
{
    return Themes()[static_cast<std::size_t>(m_theme)].accent;
}

Rect App::PanelRect() const
{
    const float scale = Scale();
    const float width = PanelWidth * scale, margin = PanelMargin * scale;
    return {static_cast<float>(m_width) - (width + margin) * m_panelSlide, margin, width, static_cast<float>(m_height) - 2 * margin};
}

Rect App::FreeArea() const
{
    const Rect panel = PanelRect();
    return {0, 0, std::min(panel.x, static_cast<float>(m_width)), static_cast<float>(m_height)};
}

namespace {

constexpr float HelpWidth = 900, HelpRow = 26, HelpPad = 28, HelpColumnGap = 24;
constexpr float AboutWidth = 520, AboutPad = 28, AboutIcon = 76, LinkRow = 36, LinkGap = 6;

// The about card is in English, whatever the language of the interface.
constexpr std::string_view AboutDescription =
    "A 3D billiards game written in modern C++ on OpenGL\u00A04.6: eight-ball, nine-ball and the Russian free pyramid, with the balls' "
    "physics simulated collision by collision and a computer opponent of five levels, from Novice to Master. Everything you see and hear "
    "is made in code: there is no model, texture or sound file.";

struct Link {
    std::string_view name;
    std::string_view address; // as shown
    const char* url;
};

constexpr std::array<Link, 5> AuthorLinks{{
    {"GitHub", "github.com/makarov-mm", "https://github.com/makarov-mm"},
    {"GitHub Sponsors", "github.com/sponsors/makarov-mm", "https://github.com/sponsors/makarov-mm"},
    {"LinkedIn", "linkedin.com/in/makarov-mm", "https://www.linkedin.com/in/makarov-mm/"},
    {"Threads", "threads.net/@m.m.makarov", "https://www.threads.net/@m.m.makarov"},
    {"Instagram", "instagram.com/m.m.makarov", "https://www.instagram.com/m.m.makarov/"},
}};

Str RulesFor(billiards::GameType type)
{
    return type == billiards::GameType::EightBall ? Str::RulesEight : type == billiards::GameType::NineBall ? Str::RulesNine : Str::RulesAmericanka;
}

} // namespace

std::vector<std::string> App::WrapText(FontId font, std::string_view words, float width) const
{
    std::vector<std::string> lines;
    std::string line;
    while (!words.empty()) {
        const std::size_t space = std::min(words.find(' '), words.size());
        const std::string_view word = words.substr(0, space);
        words.remove_prefix(std::min(space + 1, words.size()));
        std::string longer = line.empty() ? std::string(word) : std::format("{} {}", line, word);
        if (!line.empty() && m_painter->Measure(font, longer) > width) {
            lines.push_back(std::move(line));
            line = word;
        }
        else {
            line = std::move(longer);
        }
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    return lines;
}

std::array<App::HelpLine, 2 * App::HelpRows> App::HelpLines() const
{
    return {{
        // Aiming and striking.
        {T(Str::KeyMove), Str::HelpAim},
        {T(Str::KeyShiftMove), Str::HelpFineAim},
        {T(Str::KeyPull), Str::HelpStrike},
        {T(Str::KeyClick), Str::HelpClick},
        {T(Str::KeyDragCue), Str::HelpPlace},
        {T(Str::KeyRightDrag), Str::HelpRotate},
        {T(Str::KeyWheel), Str::HelpZoom},
        {"Space", Str::HelpShoot},
        {"↑  ↓", Str::HelpPower},
        {"←  →", Str::HelpTurn},
        {"W  A  S  D", Str::HelpSpin},
        {"X", Str::HelpCenter},
        {"C", Str::HelpCamera},
        // The game and the window.
        {"G", Str::HelpGuide},
        {"Ctrl+Z  Backspace", Str::HelpUndo},
        {"N", Str::HelpNew},
        {"B", Str::HelpGame},
        {"M", Str::HelpMode},
        {"1 – 5", Str::HelpLevel},
        {"T", Str::HelpTheme},
        {"V", Str::HelpSound},
        {"L", Str::HelpLanguage},
        {"Tab", Str::HelpPanel},
        {"F11  Alt+Enter", Str::HelpFullScreen},
        {"I", Str::HelpAbout},
        {"Esc  F1", Str::HelpClose},
    }};
}

App::HelpLayout App::LayOutHelp() const
{
    const float scale = Scale();
    const ui::Painter& p = *m_painter;
    HelpLayout layout;
    // The columns as they were laid out, or wider where the desktop's font needs it.
    layout.keys = {172 * scale, 150 * scale};
    std::array<float, 2> actions{};
    const auto lines = HelpLines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::size_t column = i / HelpRows;
        layout.keys[column] = std::max(layout.keys[column], p.Measure(FontId::Body, lines[i].keys) + 16 * scale);
        actions[column] = std::max(actions[column], p.Measure(FontId::Body, T(lines[i].action)));
    }
    const float first = layout.keys[0] + actions[0] + HelpColumnGap * scale, second = layout.keys[1] + actions[1];
    const float inner = std::max((HelpWidth - 2 * HelpPad) * scale, first + second);
    layout.second = std::max(inner * 0.52f, first);
    layout.width = std::max(inner, layout.second + second) + 2 * HelpPad * scale;

    layout.rules = WrapText(FontId::Body, T(RulesFor(Type())), layout.width - 2 * HelpPad * scale);
    // The rows close up, down to the height of a line, where the window is too low for them.
    const auto rows = static_cast<float>(HelpRows + 1 + layout.rules.size());
    const float fixed = (2 * HelpPad + 44 + 12) * scale;
    const float room = static_cast<float>(m_height) - 20 * scale - fixed;
    layout.rowHeight = std::max(p.LineHeight(FontId::Body), std::min(room / rows, HelpRow * scale));
    layout.height = fixed + rows * layout.rowHeight;
    return layout;
}

Rect App::CenteredCard(float width, float height) const
{
    const Rect free = FreeArea();
    const float x = width <= free.width ? free.x + (free.width - width) / 2 : std::max(0.0f, (static_cast<float>(m_width) - width) / 2);
    return {x, std::max(10 * Scale(), (static_cast<float>(m_height) - height) / 2), width, height};
}

Rect App::HelpCard() const
{
    const HelpLayout layout = LayOutHelp();
    return CenteredCard(layout.width, layout.height);
}

Rect App::AboutCard() const
{
    const float scale = Scale();
    const ui::Painter& p = *m_painter;
    const auto lines = static_cast<float>(WrapText(FontId::Body, AboutDescription, (AboutWidth - 2 * AboutPad) * scale).size());
    const auto links = static_cast<float>(AuthorLinks.size());
    const float height = (2 * AboutPad + AboutIcon + 20 + 16 + 2 + 20 + links * (LinkRow + LinkGap) - LinkGap) * scale +
                         lines * (p.LineHeight(FontId::Body) + 3 * scale) + p.LineHeight(FontId::Heading) + p.LineHeight(FontId::Body);
    return CenteredCard(AboutWidth * scale, height);
}

Rect App::BannerCard() const
{
    const float scale = Scale();
    const float width = std::max({m_painter->Measure(FontId::Display, m_bannerTitle) + 80 * scale, m_painter->Measure(FontId::Body, m_bannerDetail) + 60 * scale,
                                  320 * scale});
    const Rect free = FreeArea();
    return {free.x + (free.width - width) / 2, 26 * scale - (1 - m_bannerFade) * 20 * scale, width, 92 * scale};
}

std::vector<render::FrostedPanel> App::FrostedPanels() const
{
    std::vector<render::FrostedPanel> panels;
    const float scale = Scale();
    auto add = [&](Rect rect, float radius, float opacity) { panels.push_back({rect.x, rect.y, rect.width, rect.height, radius * scale, opacity}); };
    if (m_panelSlide > 0.001f)
        add(PanelRect(), 18, 1);
    if (m_bannerFade > 0.001f)
        add(BannerCard(), 18, m_bannerFade);
    if (m_dialog == Dialog::Help)
        add(HelpCard(), 20, 1);
    else if (m_dialog == Dialog::About)
        add(AboutCard(), 20, 1);
    return panels;
}

void App::ShowToast(std::string text)
{
    if (m_toasts.size() >= 3)
        m_toasts.erase(m_toasts.begin());
    m_toasts.push_back({std::move(text), 0});
}

void App::ShowBanner(std::string title, std::string detail, float seconds)
{
    m_bannerTitle = std::move(title);
    m_bannerDetail = std::move(detail);
    m_bannerTime = seconds;
}

std::string App::Name(int player) const
{
    const int number = player + 1;
    switch (m_mode) {
    case VsComputer: return std::string(T(player == 0 ? Str::NameYou : Str::NameComputer));
    case TwoPlayers: return std::vformat(T(Str::NamePlayer), std::make_format_args(number));
    case Demo: return std::vformat(T(Str::NameComputerN), std::make_format_args(number));
    default: return std::string(T(Str::NameYou));
    }
}

std::string App::WinnerTitle() const
{
    const int number = m_match.winner + 1;
    if (m_match.practice)
        return std::string(T(Str::TableCleared));
    switch (m_mode) {
    case VsComputer: return std::string(T(m_match.winner == 0 ? Str::WinYou : Str::WinComputer));
    case TwoPlayers: return std::vformat(T(Str::WinPlayer), std::make_format_args(number));
    default: return std::vformat(T(Str::WinComputerN), std::make_format_args(number));
    }
}

std::string App::StatusTitle() const
{
    if (m_stage == Stage::Over)
        return WinnerTitle();
    const int number = m_match.toPlay + 1;
    switch (m_mode) {
    case VsComputer: return std::string(T(m_match.toPlay == 0 ? Str::ShotYou : Str::ShotComputer));
    case TwoPlayers: return std::vformat(T(Str::ShotPlayer), std::make_format_args(number));
    case Demo: return std::vformat(T(Str::ShotComputerN), std::make_format_args(number));
    default: return std::string(T(Str::Practice));
    }
}

std::string App::StatusDetail() const
{
    switch (m_stage) {
    case Stage::Over: return m_mode == Demo ? std::string() : std::string(T(Str::PressNew));
    case Stage::Thinking: return std::string(T(Str::Thinking));
    case Stage::Presenting:
    case Stage::Stroke:
        if (!HumanToShoot())
            return std::string(T(Str::Aiming));
        break;
    case Stage::Rolling: return std::string(T(Str::Rolling));
    default: break;
    }
    if (m_match.breakShot)
        return std::string(T(Str::Breaking));
    if (m_match.inHand == billiards::InHand::Anywhere)
        return std::string(T(Str::BallInHand));
    if (m_match.inHand == billiards::InHand::Kitchen)
        return std::string(T(Str::BallInKitchen));
    switch (Type()) {
    case billiards::GameType::EightBall: {
        if (OnEight()) {
            if (m_called < 0)
                return std::string(T(Str::CallPocket));
            const std::string_view pocket = T(static_cast<Str>(std::to_underlying(Str::Pocket0) + m_called));
            return std::vformat(T(Str::OnEight), std::make_format_args(pocket));
        }
        const billiards::Group group = m_match.groups[static_cast<std::size_t>(m_match.toPlay)];
        if (group == billiards::Group::Open)
            return std::string(T(Str::OpenTable));
        const int left = m_match.BallsLeft(m_sim, group);
        return std::format("{}  ·  {}", T(group == billiards::Group::Solids ? Str::Solids : Str::Stripes), std::vformat(T(Str::Left), std::make_format_args(left)));
    }
    case billiards::GameType::NineBall: {
        const int lowest = m_match.Lowest(m_sim);
        return std::vformat(T(Str::HitLowest), std::make_format_args(lowest));
    }
    default: return std::string(T(Str::AnyBall));
    }
}

std::string App::BallName(int ball) const
{
    return ball == 0 ? std::string(T(Str::CueBall)) : std::to_string(ball);
}

std::string_view App::FoulText(billiards::Foul foul) const
{
    switch (foul) {
    case billiards::Foul::Scratch: return T(Str::FoulScratch);
    case billiards::Foul::Miss: return T(Str::FoulMiss);
    case billiards::Foul::WrongBall: return T(Str::FoulWrongBall);
    case billiards::Foul::NoRail: return T(Str::FoulNoRail);
    case billiards::Foul::EightEarly: return T(Str::FoulEightEarly);
    case billiards::Foul::EightWrongPocket: return T(Str::FoulEightWrongPocket);
    case billiards::Foul::EightOnFoul: return T(Str::FoulEightOnFoul);
    default: return {};
    }
}

void App::ToggleDialog(Dialog dialog)
{
    m_dialog = m_dialog == dialog ? Dialog::None : dialog;
    m_dialogOpening = m_dialog != Dialog::None;
    if (!m_dialogOpening)
        return;
    // What the mouse was doing stops: a stroke drawn back is not played, nor a ball placed, behind the card.
    if (m_gesture == Gesture::PlaceBall)
        FollowAim();
    m_gesture = Gesture::None;
    m_charge = 0;
    m_activeWidget = 0;
}

bool App::PressedOutside(Rect card) const
{
    // A quick click, such as a tap on a touchpad, presses and lets go within one frame: when it opens a card, its
    // press must not then close it.
    return m_pointer.leftPressed && !m_dialogOpening && !card.Contains(m_pointer.x, m_pointer.y);
}

App::Interaction App::Interact(int id, Rect rect, bool enabled)
{
    // While a card is open, only its own widgets answer, from the frame after the one that opened it; and none while
    // the mouse strikes, places a ball or turns the view.
    const bool interactive = enabled && (!DialogOpen() || (id > ModalWidgets && !m_dialogOpening)) && m_gesture == Gesture::None;
    Interaction result;
    result.hover = interactive && rect.Contains(m_pointer.x, m_pointer.y);
    if (result.hover)
        m_cursorHand = true;
    if (result.hover && m_pointer.leftPressed) {
        m_activeWidget = id;
        m_pointer.consumed = true;
    }
    if (m_pointer.leftReleased && m_activeWidget == id) {
        result.clicked = result.hover;
        m_activeWidget = 0;
        m_pointer.consumed = true;
    }
    result.pressed = result.hover && m_activeWidget == id && m_pointer.leftDown;
    if (result.clicked)
        m_sound.Play(SoundBank::Effect::Click);
    return result;
}

bool App::Button(int id, Rect rect, std::string_view label, bool enabled, bool accent)
{
    const Interaction state = Interact(id, rect, enabled);
    ui::Color fill = !enabled        ? Rgba(255, 255, 255, 7)
                     : state.pressed ? Rgba(255, 255, 255, 52)
                     : state.hover   ? Rgba(255, 255, 255, 34)
                                     : Rgba(255, 255, 255, 17);
    if (accent && enabled)
        fill = ui::Mix(fill, ui::FromLinear(Accent(), static_cast<float>(fill.a) / 255.0f * 2.2f), 0.6f);
    m_painter->Fill(rect, 9 * Scale(), fill);
    m_painter->TextCentered(FontId::Body, rect, m_painter->Elided(FontId::Body, label, rect.width - 8 * Scale()), enabled ? TextColor : ui::Fade(TextColor, 0.35f));
    return state.clicked;
}

int App::Selector(int id, Rect rect, std::string_view label, std::string_view value, float labelWidth)
{
    const float scale = Scale();
    m_painter->Text(FontId::Body, rect.x, rect.y + (rect.height - m_painter->LineHeight(FontId::Body)) / 2, label, MutedColor);
    const float arrow = rect.height;
    const Rect right{rect.Right() - arrow, rect.y, arrow, rect.height};
    // The value past the longest label, from 30% of the row at least: fonts differ in width from one desktop to another.
    const Rect left{rect.x + std::max(rect.width * 0.3f, labelWidth + 12 * scale), rect.y, arrow, rect.height};
    const Rect valueRect{left.Right(), rect.y, right.x - left.Right(), rect.height};
    m_painter->Fill({valueRect.x + 2 * scale, rect.y, valueRect.width - 4 * scale, rect.height}, 9 * scale, Rgba(255, 255, 255, 9));
    m_painter->TextCentered(FontId::Body, valueRect, m_painter->Elided(FontId::Body, value, valueRect.width - 8 * scale), TextColor);
    int change = 0;
    if (Button(id, left, ""))
        change = -1;
    if (Button(id + 1, right, ""))
        change = 1;
    const float size = 5 * scale;
    m_painter->Arrow(left.x + left.width / 2, left.y + left.height / 2, size * 0.8f, size, -1, TextColor);
    m_painter->Arrow(right.x + right.width / 2, right.y + right.height / 2, size * 0.8f, size, 1, TextColor);
    return change;
}

void App::DrawInterface()
{
    DrawPanel();
    DrawBanner();
    DrawToasts();
    if (m_dialog == Dialog::Help)
        DrawHelp();
    else if (m_dialog == Dialog::About)
        DrawAbout();
}

// A ball as a small disc in its colors: stripes with a band, the numbered ones with a white spot.
void App::DrawBallIcon(float x, float y, float radius, int ball)
{
    ui::Painter& p = *m_painter;
    const bool russian = m_table.kind == billiards::TableKind::Pyramid;
    const ui::Color color = BallColor(ball, russian);
    p.Disc(x, y + radius * 0.12f, radius * 1.08f, Rgba(0, 0, 0, 70));
    if (!russian && ball >= 9) {
        p.Disc(x, y, radius, Rgba(240, 238, 230));
        p.Fill({x - radius * 0.92f, y - radius * 0.5f, radius * 1.84f, radius}, radius * 0.45f, color);
    }
    else {
        p.Disc(x, y, radius, color);
    }
    if (ball > 0 && !russian)
        p.Disc(x, y, radius * 0.38f, Rgba(245, 244, 238));
    else if (ball > 0)
        p.Disc(x, y, radius * 0.22f, Rgba(150, 40, 30));
    p.Disc(x - radius * 0.35f, y - radius * 0.4f, radius * 0.22f, Rgba(255, 255, 255, 90));
}

void App::DrawStatus(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const ui::Color accent = ui::FromLinear(Accent());
    p.Fill(area, 12 * scale, Rgba(255, 255, 255, 14));

    // The ball the player is on.
    int icon = 0;
    if (Type() == billiards::GameType::NineBall) {
        icon = std::max(0, m_match.Lowest(m_sim));
    }
    else if (Type() == billiards::GameType::EightBall) {
        const billiards::Group group = m_match.groups[static_cast<std::size_t>(m_match.toPlay)];
        icon = OnEight() || m_stage == Stage::Over ? 8 : group == billiards::Group::Solids ? 1 : group == billiards::Group::Stripes ? 9 : 0;
    }
    else {
        icon = m_struck;
    }
    DrawBallIcon(area.x + 24 * scale, area.y + area.height / 2, 11 * scale, icon);

    // The words cut to fit, clear of the spinner while the computer thinks.
    const float textX = area.x + 46 * scale;
    const float room = area.Right() - textX - (m_stage == Stage::Thinking ? 34 : 8) * scale;
    const float lineHeight = p.LineHeight(FontId::Heading);
    p.Text(FontId::Heading, textX, area.y + area.height / 2 - lineHeight + 3 * scale, p.Elided(FontId::Heading, StatusTitle(), room),
           m_stage == Stage::Over ? accent : TextColor);
    p.Text(FontId::Body, textX, area.y + area.height / 2 + 2 * scale, p.Elided(FontId::Body, StatusDetail(), room), MutedColor);
    if (m_stage == Stage::Thinking)
        p.Spinner(area.Right() - 22 * scale, area.y + area.height / 2, 8 * scale, 2.2f * scale, m_time * 5.0f, accent);
}

// The players: who is to shoot, their group or score, and the balls they have left.
void App::DrawPlayers(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const ui::Color accent = ui::FromLinear(Accent());
    const float rowHeight = 30 * scale;
    const int players = m_match.practice ? 1 : 2;
    const bool russian = m_table.kind == billiards::TableKind::Pyramid;
    for (int player = 0; player < players; ++player) {
        const float y = area.y + static_cast<float>(player) * rowHeight;
        const bool toShoot = !m_match.Over() && m_match.toPlay == player;
        const bool winner = m_match.Over() && m_match.winner == player;
        if (toShoot || winner)
            p.Fill({area.x - 6 * scale, y + 2 * scale, area.width + 12 * scale, rowHeight - 4 * scale}, 8 * scale, ui::Fade(accent, winner ? 0.3f : 0.16f));
        p.Disc(area.x + 5 * scale, y + rowHeight / 2, 3.5f * scale, toShoot || winner ? accent : Rgba(255, 255, 255, 40));
        const float textY = y + (rowHeight - p.LineHeight(FontId::Body)) / 2;

        // On the right: the balls of the group, the score, or the strokes taken.
        float right = area.Right();
        if (m_match.practice) {
            const std::string shots = std::vformat(T(Str::Shots), std::make_format_args(m_shots));
            right -= p.Measure(FontId::Body, shots);
            p.Text(FontId::Body, right, textY, shots, MutedColor);
        }
        else if (Type() == billiards::GameType::Americanka) {
            const std::string of = std::vformat(T(Str::ToWin), std::make_format_args(billiards::Match::Target));
            const std::string score = std::to_string(m_match.Score(player));
            right -= p.Measure(FontId::Body, of);
            p.Text(FontId::Body, right, textY, of, MutedColor);
            right -= 6 * scale + p.Measure(FontId::Heading, score);
            p.Text(FontId::Heading, right, y + (rowHeight - p.LineHeight(FontId::Heading)) / 2, score, TextColor);
        }
        else if (Type() == billiards::GameType::EightBall) {
            const billiards::Group group = m_match.groups[static_cast<std::size_t>(player)];
            if (group == billiards::Group::Open) {
                const std::string_view open = T(Str::OpenTable);
                right -= p.Measure(FontId::Body, open);
                p.Text(FontId::Body, right, textY, open, MutedColor);
            }
            else {
                const float radius = 6.5f * scale, step = 15.5f * scale;
                std::vector<int> balls;
                for (int ball = 1; ball < m_sim.count; ++ball)
                    if (billiards::InGroup(ball, group) && m_sim.balls[static_cast<std::size_t>(ball)].OnTable())
                        balls.push_back(ball);
                if (balls.empty())
                    balls.push_back(8);
                float x = right - radius;
                for (auto ball = balls.rbegin(); ball != balls.rend(); ++ball, x -= step)
                    DrawBallIcon(x, y + rowHeight / 2, radius, *ball);
                right -= static_cast<float>(balls.size()) * step;
            }
        }

        // The name in the room left of that, and the computer's level where it fits too.
        const float nameX = area.x + 16 * scale, room = right - 10 * scale - nameX;
        std::string name = Name(player);
        if (!IsHuman(player)) {
            std::string withLevel = std::format("{}  ·  {}", name, T(static_cast<Str>(std::to_underlying(Str::Level1) + m_level - 1)));
            if (p.Measure(FontId::Body, withLevel) <= room)
                name = std::move(withLevel);
        }
        p.Text(FontId::Body, nameX, textY, p.Elided(FontId::Body, name, room), toShoot || winner ? TextColor : ui::Fade(TextColor, 0.75f));
    }

    // Below: the balls still on the table, the one to hit first ringed.
    const bool ballsRow = Type() == billiards::GameType::NineBall || m_match.practice;
    if (ballsRow) {
        const float y = area.y + static_cast<float>(players) * rowHeight + 13 * scale;
        const float radius = 6.5f * scale, step = 15.5f * scale;
        const int lowest = Type() == billiards::GameType::NineBall ? m_match.Lowest(m_sim) : -1;
        float x = area.x + radius + 2 * scale;
        int shown = 0;
        for (int ball = 1; ball < m_sim.count && x < area.Right(); ++ball) {
            if (!m_sim.balls[static_cast<std::size_t>(ball)].OnTable())
                continue;
            if (ball == lowest)
                p.Disc(x, y, radius + 2.5f * scale, ui::Fade(accent, 0.8f));
            DrawBallIcon(x, y, radius, ball);
            x += step;
            ++shown;
        }
        if (shown == 0 && russian)
            DrawBallIcon(x, y, radius, 0);
    }
}

// The power of the stroke and where the cue strikes the ball, both set by dragging.
void App::DrawStroke(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const ui::Color accent = ui::FromLinear(Accent());
    const bool modal = DialogOpen();
    p.Fill(area, 12 * scale, Rgba(255, 255, 255, 10));
    const Rect inner = area.Inset(10 * scale);

    // The spin: the ball's face seen from behind, a red dot where the cue meets it.
    const float r = inner.height / 2;
    const float cx = inner.Right() - r, cy = inner.y + r;
    const bool russian = m_table.kind == billiards::TableKind::Pyramid;
    const ui::Color face = russian && m_struck == 0 ? Rgba(226, 172, 86) : russian ? Rgba(232, 224, 200) : Rgba(236, 234, 226);
    p.Disc(cx, cy + 1.5f * scale, r + 1.5f * scale, Rgba(0, 0, 0, 80));
    p.Disc(cx, cy, r, ui::Mix(face, Rgba(0, 0, 0), 0.22f));
    p.Disc(cx - r * 0.06f, cy - r * 0.08f, r * 0.9f, face);
    p.Disc(cx - r * 0.32f, cy - r * 0.38f, r * 0.22f, Rgba(255, 255, 255, 120));
    const float limit = MaxSpin * r;
    p.Outline({cx - limit, cy - limit, 2 * limit, 2 * limit}, limit, Rgba(0, 0, 0, 50));
    p.Fill({cx - r * 0.8f, cy - 0.5f * scale, r * 1.6f, 1 * scale}, 0, Rgba(0, 0, 0, 36));
    p.Fill({cx - 0.5f * scale, cy - r * 0.8f, 1 * scale, r * 1.6f}, 0, Rgba(0, 0, 0, 36));
    const float dotX = cx + m_side * r, dotY = cy - m_vertical * r;
    p.Disc(dotX, dotY, r * 0.16f + 1 * scale, Rgba(255, 255, 255, 160));
    p.Disc(dotX, dotY, r * 0.16f, Rgba(210, 34, 30));
    const bool overSpin = !modal && Length(Vec2{m_pointer.x - cx, m_pointer.y - cy}) < r && m_gesture == Gesture::None;
    if (overSpin)
        m_cursorHand = true;
    if (overSpin && m_pointer.leftPressed) {
        m_activeWidget = SpinBall;
        m_pointer.consumed = true;
    }
    if (m_activeWidget == SpinBall) {
        if (m_pointer.leftDown)
            SetSpin((m_pointer.x - cx) / r, -(m_pointer.y - cy) / r);
        if (m_pointer.leftReleased) {
            m_activeWidget = 0;
            m_pointer.consumed = true;
        }
    }

    // The power: a bar, its value in words and in speed.
    const float left = inner.x, right = cx - r - 16 * scale;
    const float power = m_gesture == Gesture::Strike ? m_charge : m_power;
    const std::string percent = std::format("{}%", static_cast<int>(std::round(power * 100)));
    const float lineY = inner.y + 1 * scale;
    p.Text(FontId::Body, left, lineY, T(Str::Power), MutedColor);
    p.Text(FontId::Body, right - p.Measure(FontId::Body, percent), lineY, percent, TextColor);
    const Rect bar{left, inner.y + inner.height / 2 - 4 * scale, right - left, 10 * scale};
    p.Fill(bar, 5 * scale, Rgba(255, 255, 255, 20));
    const ui::Color fill = m_gesture == Gesture::Strike ? ui::Mix(accent, Rgba(255, 255, 255), 0.25f) : ui::Fade(accent, 0.85f);
    p.Fill({bar.x, bar.y, std::max(bar.height, bar.width * power), bar.height}, 5 * scale, fill);
    for (int tick = 1; tick < 4; ++tick)
        p.Fill({bar.x + bar.width * static_cast<float>(tick) / 4 - 0.5f * scale, bar.y + 2 * scale, 1 * scale, bar.height - 4 * scale}, 0,
               Rgba(0, 0, 0, 50));
    const std::string speed = std::format("{:.1f} {}", SpeedFor(power), m_language == Language::Russian ? "м/с" : "m/s");
    p.Text(FontId::Body, left, inner.Bottom() - p.LineHeight(FontId::Body), speed, ui::Fade(MutedColor, 0.85f));
    const std::string_view spin = T(Str::Spin);
    p.Text(FontId::Body, right - p.Measure(FontId::Body, spin), inner.Bottom() - p.LineHeight(FontId::Body), spin, ui::Fade(MutedColor, 0.85f));

    const Rect hit{bar.x - 4 * scale, bar.y - 8 * scale, bar.width + 8 * scale, bar.height + 16 * scale};
    const bool overPower = !modal && hit.Contains(m_pointer.x, m_pointer.y) && m_gesture == Gesture::None;
    if (overPower)
        m_cursorHand = true;
    if (overPower && m_pointer.leftPressed) {
        m_activeWidget = PowerBar;
        m_pointer.consumed = true;
    }
    if (m_activeWidget == PowerBar) {
        if (m_pointer.leftDown)
            SetPower((m_pointer.x - bar.x) / bar.width);
        if (m_pointer.leftReleased) {
            m_activeWidget = 0;
            m_pointer.consumed = true;
        }
    }
}

void App::DrawHistory(Rect area)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill(area, 12 * scale, Rgba(0, 0, 0, 40));
    if (m_history.empty()) {
        p.TextCentered(FontId::Body, area, T(Str::NoShotsYet), MutedColor);
        return;
    }
    const Rect inner = area.Inset(9 * scale);
    struct Line {
        std::string text;
        int kind;
        bool first;
    };
    std::vector<Line> lines;
    const std::size_t start = m_history.size() > 80 ? m_history.size() - 80 : 0;
    for (std::size_t i = start; i < m_history.size(); ++i) {
        bool first = true;
        for (std::string& line : WrapText(FontId::Body, m_history[i].text, inner.width - 14 * scale)) {
            lines.push_back({std::move(line), m_history[i].kind, first});
            first = false;
        }
    }
    const float rowHeight = std::round(22 * scale);
    const int visible = std::max(1, static_cast<int>(inner.height / rowHeight));
    const float maxScroll = static_cast<float>(std::max(0, static_cast<int>(lines.size()) - visible));
    if (area.Contains(m_pointer.x, m_pointer.y) && m_pointer.wheel != 0 && !DialogOpen()) {
        m_historyScroll = std::clamp(m_historyScroll - m_pointer.wheel * 3, 0.0f, maxScroll);
        m_followHistory = m_historyScroll >= maxScroll;
        m_pointer.wheel = 0;
    }
    if (m_followHistory)
        m_historyScroll = maxScroll;
    m_historyScroll = std::clamp(m_historyScroll, 0.0f, maxScroll);
    const ui::Color accent = ui::FromLinear(Accent());
    const int first = static_cast<int>(m_historyScroll);
    for (int i = first; i < std::min(static_cast<int>(lines.size()), first + visible); ++i) {
        const Line& line = lines[static_cast<std::size_t>(i)];
        const float y = inner.y + static_cast<float>(i - first) * rowHeight;
        const ui::Color color = line.kind == 1 ? FoulColor : line.kind == 2 ? accent : TextColor;
        if (line.first)
            p.Disc(inner.x + 3 * scale, y + rowHeight / 2, 2.5f * scale, ui::Fade(color, 0.7f));
        p.Text(FontId::Body, inner.x + 14 * scale, y + (rowHeight - p.LineHeight(FontId::Body)) / 2, line.text,
               i == static_cast<int>(lines.size()) - 1 || line.kind != 0 ? color : ui::Fade(color, 0.82f));
    }
    if (maxScroll > 0) {
        const float barHeight = inner.height * static_cast<float>(visible) / static_cast<float>(lines.size());
        const float barY = inner.y + (inner.height - barHeight) * (m_historyScroll / maxScroll);
        p.Fill({area.Right() - 5 * scale, barY, 3 * scale, barHeight}, 1.5f * scale, Rgba(255, 255, 255, 50));
    }
}

void App::DrawPanel()
{
    if (m_panelSlide <= 0.001f)
        return;
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Rect panel = PanelRect();
    if (panel.Contains(m_pointer.x, m_pointer.y) && (m_pointer.leftPressed || m_pointer.rightPressed) && m_gesture == Gesture::None)
        m_pointer.consumed = true;
    p.Shadow(panel, 22 * scale, Rgba(0, 0, 0, 70));
    p.Fill(panel, 18 * scale, Rgba(16, 18, 26, 120));
    p.Outline(panel, 18 * scale, Rgba(255, 255, 255, 24));

    // In a low window everything is packed closer, and the footer goes.
    const bool compact = panel.height < 820 * scale;
    const float pad = (compact ? 16 : 20) * scale;
    const float x = panel.x + pad, width = panel.width - 2 * pad;
    float y = panel.y + pad;

    const float small = 30 * scale;
    p.Text(FontId::Heading, x, y, p.Elided(FontId::Heading, T(Str::Title), width - 2 * small - 14 * scale), TextColor);
    if (Button(HelpButton, {panel.Right() - pad - small, y - 2 * scale, small, small}, "?"))
        ToggleDialog(Dialog::Help);
    if (Button(AboutButton, {panel.Right() - pad - 2 * small - 6 * scale, y - 2 * scale, small, small}, "i"))
        ToggleDialog(Dialog::About);
    y += (compact ? 38 : 44) * scale;

    const float statusHeight = (compact ? 54 : 62) * scale;
    DrawStatus({x, y, width, statusHeight});
    y += statusHeight + (compact ? 8 : 12) * scale;

    const int players = m_match.practice ? 1 : 2;
    const bool ballsRow = Type() == billiards::GameType::NineBall || m_match.practice;
    const float playersHeight = (static_cast<float>(players) * 30 + (ballsRow ? 28 : 0)) * scale;
    DrawPlayers({x + 4 * scale, y, width - 8 * scale, playersHeight});
    y += playersHeight + (compact ? 8 : 12) * scale;

    const float strokeHeight = (compact ? 72 : 80) * scale;
    DrawStroke({x, y, width, strokeHeight});
    y += strokeHeight + (compact ? 10 : 14) * scale;

    // From the bottom up: the footer, the switches, the settings and the buttons; the shots take the space between.
    float bottom = panel.Bottom() - pad;
    if (!compact) {
        const float footerHeight = p.LineHeight(FontId::Body);
        // Into the panel's padding where the desktop's font is wide.
        const float room = panel.width - 16 * scale;
        p.TextCentered(FontId::Body, {panel.x, bottom - footerHeight, panel.width, footerHeight}, p.Elided(FontId::Body, T(Str::Footer), room),
                       ui::Fade(MutedColor, 0.8f));
        bottom -= footerHeight + 14 * scale;
    }

    const float rowHeight = (compact ? 29 : 34) * scale, gap = (compact ? 5 : 8) * scale;
    const float half = (width - gap) / 2;
    bottom -= rowHeight;
    if (Button(SoundButton, {x, bottom, half, rowHeight}, T(Str::Sound), true, m_sound.Enabled()))
        ToggleSound();
    if (Button(LanguageButton, {x + half + gap, bottom, half, rowHeight}, T(Str::LanguageName)))
        ToggleLanguage();
    bottom -= gap;

    static constexpr std::array<Str, 4> modes{Str::ModeComputer, Str::ModeTwoPlayers, Str::ModePractice, Str::ModeDemo};
    static constexpr std::array<Str, 3> games{Str::GameEight, Str::GameNine, Str::GameAmericanka};
    static constexpr std::array<Str, 3> lines{Str::AimFull, Str::AimShort, Str::AimOff};
    const Theme& theme = Themes()[static_cast<std::size_t>(m_theme)];
    struct Choice {
        int id;
        Str label;
        std::string value;
    };
    const std::array<Choice, 5> choices{{
        {ThemeSelector, Str::Theme, std::string(m_language == Language::Russian ? theme.nameRussian : theme.nameEnglish)},
        {AimSelector, Str::Aim, std::string(T(lines[static_cast<std::size_t>(m_aimLines)]))},
        {LevelSelector, Str::Level, std::format("{} · {}", m_level, T(static_cast<Str>(std::to_underlying(Str::Level1) + m_level - 1)))},
        {ModeSelector, Str::Mode, std::string(T(modes[static_cast<std::size_t>(m_mode)]))},
        {GameSelector, Str::Game, std::string(T(games[static_cast<std::size_t>(m_game)]))},
    }};
    float labelWidth = 0;
    for (const Choice& choice : choices)
        labelWidth = std::max(labelWidth, p.Measure(FontId::Body, T(choice.label)));
    for (const Choice& choice : choices) {
        bottom -= rowHeight;
        if (const int change = Selector(choice.id, {x, bottom, width, rowHeight}, T(choice.label), choice.value, labelWidth); change != 0) {
            switch (choice.id) {
            case ThemeSelector: SetTheme(m_theme + change); break;
            case AimSelector: SetAimLines(m_aimLines + change); break;
            case LevelSelector: SetLevel(m_level + change); break;
            case ModeSelector: SetMode(m_mode + change); break;
            default: SetGame(m_game + change); break;
            }
        }
        bottom -= (compact ? 3 : 6) * scale;
    }
    bottom -= (compact ? 4 : 8) * scale;

    const float buttonHeight = (compact ? 31 : 38) * scale;
    bottom -= buttonHeight;
    static constexpr std::array<Str, 3> cameras{Str::CameraOverview, Str::CameraBehind, Str::CameraTop};
    const std::string view = std::format("{}: {}", T(Str::Camera), T(cameras[static_cast<std::size_t>(m_camera.Mode())]));
    if (Button(CameraButton, {x, bottom, width, buttonHeight}, view))
        SetCamera(static_cast<int>(m_camera.Mode()) + 1);
    bottom -= gap + buttonHeight;
    if (Button(NewGameButton, {x, bottom, half, buttonHeight}, T(Str::NewGame), true, m_stage == Stage::Over))
        NewGame();
    if (Button(UndoButton, {x + half + gap, bottom, half, buttonHeight}, T(Str::UndoShot), !m_undo.empty() && m_mode != Demo))
        Undo();
    bottom -= (compact ? 10 : 14) * scale;

    // The shots so far, where there is room for them.
    const float labelHeight = p.LineHeight(FontId::Body) + 6 * scale;
    if (bottom - y - labelHeight > 36 * scale) {
        p.Text(FontId::Body, x, y, T(Str::History), MutedColor);
        DrawHistory({x, y + labelHeight, width, bottom - y - labelHeight});
    }
}

void App::DrawBanner()
{
    if (m_bannerFade <= 0.001f)
        return;
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Rect card = BannerCard();
    p.Shadow(card, 24 * scale, ui::Fade(Rgba(0, 0, 0, 120), m_bannerFade));
    p.Fill(card, 18 * scale, ui::Fade(Rgba(14, 16, 24, 150), m_bannerFade));
    p.Outline(card, 18 * scale, ui::Fade(ui::FromLinear(Accent()), 0.6f * m_bannerFade));
    p.TextCentered(FontId::Display, {card.x, card.y + 8 * scale, card.width, 48 * scale}, m_bannerTitle, ui::Fade(TextColor, m_bannerFade));
    p.TextCentered(FontId::Body, {card.x, card.y + 54 * scale, card.width, 26 * scale}, m_bannerDetail, ui::Fade(MutedColor, m_bannerFade));
}

// The card's glass is frosted over the scene; over the panel, where there is nothing to frost, it is darker instead.
void App::DrawCard(Rect card)
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const bool overPanel = m_panelSlide > 0.001f && card.Right() > PanelRect().x;
    p.Shadow(card, 26 * scale, Rgba(0, 0, 0, 140));
    p.Fill(card, 20 * scale, Rgba(18, 20, 28, overPanel ? 235 : 175));
    p.Outline(card, 20 * scale, Rgba(255, 255, 255, 30));
}

void App::DrawHelp()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)}, 0, Rgba(0, 0, 0, 110));

    const HelpLayout layout = LayOutHelp();
    const Rect card = CenteredCard(layout.width, layout.height);
    const float pad = HelpPad * scale, rowHeight = layout.rowHeight;
    const ui::Color accent = ui::FromLinear(Accent());
    DrawCard(card);
    p.Text(FontId::Heading, card.x + pad, card.y + pad - 4 * scale, T(Str::Controls), TextColor);
    const float top = card.y + pad + 44 * scale;
    const auto lines = HelpLines();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const std::size_t column = i / HelpRows;
        const float x = card.x + pad + (column == 0 ? 0.0f : layout.second);
        const float y = top + static_cast<float>(i % HelpRows) * rowHeight;
        p.Text(FontId::Body, x, y, lines[i].keys, accent);
        p.Text(FontId::Body, x + layout.keys[column], y, T(lines[i].action), TextColor);
    }
    // The rules of the game on the table, in a few sentences.
    float y = top + static_cast<float>(HelpRows) * rowHeight + 12 * scale;
    p.Text(FontId::Body, card.x + pad, y, T(Str::Rules), accent);
    for (const std::string& line : layout.rules) {
        y += rowHeight;
        p.Text(FontId::Body, card.x + pad, y, line, MutedColor);
    }
    if (Button(HelpClose, {card.Right() - pad - 30 * scale, card.y + pad - 8 * scale, 30 * scale, 30 * scale}, "×") || PressedOutside(card))
        m_dialog = Dialog::None;
    if (m_pointer.leftPressed || m_pointer.rightPressed)
        m_pointer.consumed = true;
}

void App::DrawAbout()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    p.Fill({0, 0, static_cast<float>(m_width), static_cast<float>(m_height)}, 0, Rgba(0, 0, 0, 110));
    const Rect card = AboutCard();
    const float pad = AboutPad * scale;
    const ui::Color accent = ui::FromLinear(Accent());
    DrawCard(card);

    // The icon, and beside it the name and the version.
    const float x = card.x + pad, width = card.width - 2 * pad, icon = AboutIcon * scale;
    float y = card.y + pad;
    p.Icon({x, y, icon, icon});
    const float titleHeight = p.LineHeight(FontId::Display), versionHeight = p.LineHeight(FontId::Body);
    const float titleY = y + (icon - titleHeight - versionHeight) / 2;
    p.Text(FontId::Display, x + icon + 18 * scale, titleY, "3D Billiards", TextColor);
    p.Text(FontId::Body, x + icon + 18 * scale, titleY + titleHeight, "Version " BILLIARDS3D_VERSION " for Linux", MutedColor);
    y += icon + 20 * scale;

    for (const std::string& line : WrapText(FontId::Body, AboutDescription, width)) {
        p.Text(FontId::Body, x, y, line, TextColor);
        y += p.LineHeight(FontId::Body) + 3 * scale;
    }
    y += 16 * scale;
    p.Text(FontId::Heading, x, y, "Created by Mykhailo Makarov", TextColor);
    y += p.LineHeight(FontId::Heading) + 2 * scale;
    p.Text(FontId::Body, x, y, "© 2026 · MIT License", MutedColor);
    y += p.LineHeight(FontId::Body) + 20 * scale;

    // The author's pages, opened in the browser.
    float nameWidth = 0;
    for (const Link& link : AuthorLinks)
        nameWidth = std::max(nameWidth, p.Measure(FontId::Body, link.name));
    for (std::size_t i = 0; i < AuthorLinks.size(); ++i) {
        const Link& link = AuthorLinks[i];
        const Rect row{x, y, width, LinkRow * scale};
        const Interaction state = Interact(AboutLinks + static_cast<int>(i), row, true);
        p.Fill(row, 9 * scale, state.pressed ? Rgba(255, 255, 255, 52) : state.hover ? Rgba(255, 255, 255, 34) : Rgba(255, 255, 255, 12));
        const float textY = row.y + (row.height - p.LineHeight(FontId::Body)) / 2;
        p.Text(FontId::Body, row.x + 14 * scale, textY, link.name, TextColor);
        p.Text(FontId::Body, row.x + 32 * scale + nameWidth, textY, link.address, accent);
        p.Text(FontId::Body, row.Right() - 14 * scale - p.Measure(FontId::Body, "↗"), textY, "↗", state.hover ? TextColor : MutedColor);
        if (state.clicked)
            OpenLink(link.url);
        y += (LinkRow + LinkGap) * scale;
    }

    if (Button(AboutClose, {card.Right() - pad - 30 * scale, card.y + pad - 8 * scale, 30 * scale, 30 * scale}, "×") || PressedOutside(card))
        m_dialog = Dialog::None;
    if (m_pointer.leftPressed || m_pointer.rightPressed)
        m_pointer.consumed = true;
}

void App::DrawToasts()
{
    const float scale = Scale();
    ui::Painter& p = *m_painter;
    const Rect free = FreeArea();
    float y = static_cast<float>(m_height) - 40 * scale;
    for (auto toast = m_toasts.rbegin(); toast != m_toasts.rend(); ++toast) {
        const float fade = std::min({1.0f, toast->age * 6, (2.8f - toast->age) * 3});
        const float width = p.Measure(FontId::Body, toast->text) + 40 * scale, height = 36 * scale;
        const Rect pill{free.x + (free.width - width) / 2, y - height, width, height};
        p.Shadow(pill, 14 * scale, ui::Fade(Rgba(0, 0, 0, 110), fade));
        p.Fill(pill, height / 2, ui::Fade(Rgba(20, 22, 30, 225), fade));
        p.TextCentered(FontId::Body, pill, toast->text, ui::Fade(TextColor, fade));
        y -= height + 8 * scale;
    }
}
