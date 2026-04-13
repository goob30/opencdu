#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <nlohmann/json.hpp> 

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <netinet/tcp.h>

#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// note for jon please consider switching from sdl2 or c++ in general it is the bane of my existence thanks

using json = nlohmann::json;

static constexpr int WIDTH = 640;
static constexpr int HEIGHT = 480;

static constexpr int CELL_W = 24;
static constexpr int CELL_H = 32;
static constexpr int COLS = 24;
static constexpr int MAIN_ROWS = 13;
static constexpr int TOTAL_ROWS = 14;

static constexpr int GRID_W = COLS * CELL_W;
static constexpr int GRID_H = TOTAL_ROWS * CELL_H;
static constexpr int GRID_X0 = (WIDTH - GRID_W) / 2;   // 32
static constexpr int GRID_Y0 = (HEIGHT - GRID_H) / 2;  // 16

long long g_lastRecvMs = 0;
bool g_newUpdate = false;


struct GlyphRect // glyph coords i htink
{
    int x = 0;
    int y = 0;
    int w = CELL_W;
    int h = CELL_H;
};

struct AtlasFont
{
    SDL_Texture* texture = nullptr;
    std::unordered_map<std::string, GlyphRect> glyphs;
};

struct Cell
{
    std::string text = " ";
    std::string color = "white";
    std::string size = "normal";
};

using Row = std::array<Cell, COLS>;

struct ScreenState
{
    std::array<Row, TOTAL_ROWS> rows{};
    bool hasData = false;
};

static std::mutex g_stateMutex;
static ScreenState g_state;
static std::atomic<bool> g_running{true};

static SDL_Color colorFromName(const std::string& name)
{
    if (name == "green")   return {0x33, 0xFF, 0x33, 255};
    if (name == "amber")   return {0xFF, 0xB8, 0x33, 255};
    if (name == "cyan")    return {0x00, 0xFF, 0xFF, 255};
    if (name == "magenta") return {0xFF, 0x00, 0xFF, 255};
    if (name == "red")     return {0xFF, 0x00, 0x00, 255};
    return {0xE5, 0xE5, 0xE5, 255};
}

static void clearState(ScreenState& s)
{
    for (auto& row : s.rows)
    {
        for (auto& cell : row)
        {
            cell = Cell{};
        }
    }
    s.hasData = false;
}

static bool loadAtlasFont(
    SDL_Renderer* renderer,
    const std::string& pngPath,
    const std::string& jsonPath,
    AtlasFont& outFont)
{
    outFont.texture = IMG_LoadTexture(renderer, pngPath.c_str());
    if (!outFont.texture)
    {
        std::cerr << "Failed to get atlas texture! " << pngPath
                  << ": " << IMG_GetError() << "\n";
        return false;
    }

    std::ifstream f(jsonPath);
    if (!f.is_open())
    {
        std::cerr << "Faie;ed to open atlas map! " << jsonPath << "\n";
        return false;
    }

    json j;
    f >> j;

    for (auto it = j.begin(); it != j.end(); ++it)
    {
        GlyphRect g;
        g.x = it.value().value("x", 0);
        g.y = it.value().value("y", 0);
        g.w = it.value().value("w", CELL_W);
        g.h = it.value().value("h", CELL_H);
        outFont.glyphs[it.key()] = g;
    }

    SDL_SetTextureBlendMode(outFont.texture, SDL_BLENDMODE_BLEND);
    return true;
}

static Cell parseCellJson(const json& j, const std::string& fallbackSize = "normal")
{
    Cell cell;

    if (j.is_object())
    {
        cell.text = j.value("text", " ");
        cell.color = j.value("color", "white");
        cell.size = j.value("size", fallbackSize);
    }

    if (cell.text.empty())
        cell.text = " ";

    return cell;
}

static bool isBlankText(const std::string& s)
{
    return s.empty() || s == " " || s == "\u00A0";
}

static bool isBoxPlaceholder(const std::string& s)
{
    return s == "▯" || s == "□";
}

static void drawBox(SDL_Renderer* renderer, int x, int y, const std::string& colorName) // blank character
{
    SDL_Color color = colorFromName(colorName);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);

    SDL_Rect box{
        x + 4,
        y + 8,
        CELL_W - 8,
        CELL_H - 16
    };

    SDL_RenderDrawRect(renderer, &box);
}

static void drawGlyph( // draws each character
    SDL_Renderer* renderer,
    AtlasFont& font,
    const std::string& glyph,
    const std::string& colorName,
    int x,
    int y)
{
    auto it = font.glyphs.find(glyph);
    if (it == font.glyphs.end())
        return;

    const GlyphRect& g = it->second;

    SDL_Rect src{g.x, g.y, g.w, g.h};
    SDL_Rect dst{x, y, CELL_W, CELL_H};

    SDL_Color color = colorFromName(colorName);
    SDL_SetTextureColorMod(font.texture, color.r, color.g, color.b);
    SDL_SetTextureAlphaMod(font.texture, 255);

    SDL_RenderCopy(renderer, font.texture, &src, &dst);
}

static void drawFallbackUnknown(SDL_Renderer* renderer, int x, int y, const std::string& colorName) // i forgot what this one does 
{
    SDL_Color color = colorFromName(colorName);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);

    SDL_Rect r{
        x + 9,
        y + 10,
        6,
        12
    };

    SDL_RenderFillRect(renderer, &r);
}

static void loadRow(Row& dest, const json& arr, const std::string& fallbackSize) // load the row
{
    for (int c = 0; c < COLS; ++c)
    {
        dest[c] = Cell{};
    }

    if (!arr.is_array())
        return;

    for (int c = 0; c < COLS && c < static_cast<int>(arr.size()); ++c)
    {
        dest[c] = parseCellJson(arr[c], fallbackSize);
    }
}

static void loadFbw(const json& root, ScreenState& s) // init for fbw
{
    clearState(s);

    if (root.contains("Grid") && root["Grid"].is_array())
    {
        const auto& grid = root["Grid"];
        for (int r = 0; r < MAIN_ROWS && r < static_cast<int>(grid.size()); ++r)
        {
            const std::string fallbackSize = (r % 2 == 0) ? "small" : "normal";
            loadRow(s.rows[r], grid[r], fallbackSize);
        }
    }

    if (root.contains("Scratchpad") && root["Scratchpad"].is_array())
    {
        loadRow(s.rows[13], root["Scratchpad"], "normal");
    }

    s.hasData = true;
}

static void loadPmdg(const json& root, ScreenState& s) // init for pmdg
{
    clearState(s);

    if (root.contains("Lines") && root["Lines"].is_array()) {
        const auto& lines = root["Lines"];

        for (int r = 0; r < MAIN_ROWS && r < static_cast<int>(lines.size()); ++r)
        {
            json flat = json::array();

            if (lines[r].contains("Left") && lines[r]["Left"].is_array())
                for (const auto& x : lines[r]["Left"]) flat.push_back(x);

            if (lines[r].contains("Center") && lines[r]["Center"].is_array())
                for (const auto& x : lines[r]["Center"]) flat.push_back(x);

            if (lines[r].contains("Right") && lines[r]["Right"].is_array())
                for (const auto& x : lines[r]["Right"]) flat.push_back(x);

            loadRow(s.rows[r], flat, "normal");
        }
    }

    if (root.contains("Scratchpad") && root["Scratchpad"].is_array())
    {
        loadRow(s.rows[13], root["Scratchpad"], "normal");
    }

    s.hasData = true;
}

static void applyJsonLine(const std::string& line) // its in the name
{
    try {
        json root = json::parse(line);

        std::lock_guard<std::mutex> lock(g_stateMutex);

        // PMDG first: its payload can still contain an empty Grid field.
        if (root.contains("Lines") && root["Lines"].is_array() && !root["Lines"].empty())
        {
            loadPmdg(root, g_state);
        }
        else if (root.contains("Grid") && root["Grid"].is_array())
        {
            loadFbw(root, g_state);
        }
    }
    catch (const std::exception& ex)
    {
        std::cerr << "JSON parse error: " << ex.what() << "\n";
    }
}

static void tcpReaderThread(const std::string& host, int port) // it reads the tcp thread
{
    while (g_running.load())
    {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        int one = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));

        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
        {
            std::cerr << "Bad server IP: " << host << "\n";
            close(sock);
            return;
        }

        std::cerr << "Connecting to " << host << ":" << port << "...\n";

        if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            close(sock);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        std::cerr << "Connected.\n";

        std::string buffer;
        char temp[4096];

        while (g_running.load())
        {
            ssize_t n = recv(sock, temp, sizeof(temp), 0);
            if (n <= 0)
                break;

            buffer.append(temp, static_cast<size_t>(n));

            size_t pos = 0;
            while ((pos = buffer.find('\n')) != std::string::npos)
            {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);

                if (!line.empty())
                {
                    auto now = std::chrono::steady_clock::now().time_since_epoch();
                    long long recvMs =
                        std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

                    {
                        std::lock_guard<std::mutex> lock(g_stateMutex);
                        g_lastRecvMs = recvMs;
                        g_newUpdate = true;
                    }

                    std::cerr << "PI_RECV " << recvMs << "\n";

                    applyJsonLine(line);
                }
            }
        }

        close(sock);
        std::cerr << "Disconnected, retrying...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

static void drawFallbackColorBars(SDL_Renderer* renderer) // if shit hits the fan this shows
{
    SDL_Color bars[] = {
        {255,255,255,255},
        {255,255,0,255},
        {0,255,255,255},
        {0,255,0,255},
        {255,0,255,255},
        {255,0,0,255},
        {0,0,255,255},
        {0,0,0,255}
    };

    const int count = static_cast<int>(sizeof(bars) / sizeof(bars[0]));
    const int barWidth = WIDTH / count;

    for (int i = 0; i < count; ++i)
    {
        SDL_SetRenderDrawColor(renderer, bars[i].r, bars[i].g, bars[i].b, 255);
        SDL_Rect rect{ i * barWidth, 0, barWidth, HEIGHT };
        SDL_RenderFillRect(renderer, &rect);
    }
}

int main(int argc, char** argv) // take a wild guess
{
    std::string serverIp = "10.0.0.92";
    int serverPort = 8382;

    if (argc >= 2) serverIp = argv[1];
    if (argc >= 3) serverPort = std::stoi(argv[2]);

    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    if ((IMG_Init(IMG_INIT_PNG) & IMG_INIT_PNG) == 0)
    {
        std::cerr << "IMG_Init failed: " << IMG_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "OpenCDU Pi",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        WIDTH,
        HEIGHT,
        SDL_WINDOW_SHOWN
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(
        window,
        -1,
        SDL_RENDERER_PRESENTVSYNC
    );

    if (!window || !renderer)
    {
        std::cerr << "Window/renderer creation failed: " << SDL_GetError() << "\n";
        if (renderer) SDL_DestroyRenderer(renderer);
        if (window) SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    AtlasFont largeFont;
    AtlasFont smallFont;

    if (!loadAtlasFont(renderer, "cdu_atlas_large.png", "cdu_map_large.json", largeFont) ||
        !loadAtlasFont(renderer, "cdu_atlas_small.png", "cdu_map_small.json", smallFont))
    {
        std::cerr << "Failed to load atlas fonts.\n";
        if (largeFont.texture) SDL_DestroyTexture(largeFont.texture);
        if (smallFont.texture) SDL_DestroyTexture(smallFont.texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        IMG_Quit();
        SDL_Quit();
        return 1;
    }

    clearState(g_state);

    std::thread netThread(tcpReaderThread, serverIp, serverPort);

    
    while (g_running.load())
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (e.type == SDL_QUIT) // just like i do
                g_running.store(false);
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE)
                g_running.store(false);
        }

        ScreenState snapshot;
        {
            std::lock_guard<std::mutex> lock(g_stateMutex);
            snapshot = g_state;
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);

        auto start = std::chrono::steady_clock::now();

        if (!snapshot.hasData)
        {
            drawFallbackColorBars(renderer);
        }
        else
        {
            for (int row = 0; row < TOTAL_ROWS; ++row)
            {
                for (int col = 0; col < COLS; ++col)
                {
                    const Cell& cell = snapshot.rows[row][col];

                    if (isBlankText(cell.text))
                        continue;

                    const int x = GRID_X0 + col * CELL_W;
                    const int y = GRID_Y0 + row * CELL_H;

                    if (isBoxPlaceholder(cell.text))
                    {
                        drawBox(renderer, x, y, cell.color);
                        continue;
                    }

                    AtlasFont& font = (cell.size == "small") ? smallFont : largeFont;

                    if (font.glyphs.find(cell.text) != font.glyphs.end())
                        drawGlyph(renderer, font, cell.text, cell.color, x, y);
                    else
                        drawFallbackUnknown(renderer, x, y, cell.color);
                }
            }
        }

        SDL_RenderPresent(renderer);

        auto end = std::chrono::steady_clock::now();
        auto frameMs = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        auto presentNow = std::chrono::steady_clock::now().time_since_epoch();
        auto presentMs = std::chrono::duration_cast<std::chrono::milliseconds>(presentNow).count();

        std::cerr << "PI_PRESENT " << presentMs << "\n";

        if (g_newUpdate)
        {
            long long recvMs = g_lastRecvMs;
            long long delta = presentMs - recvMs;

            std::cerr << "PI_LATENCY " << delta << "ms\n";

            g_newUpdate = false;
        }

        if (frameMs > 10)
            std::cerr << "FRAME TIME " << frameMs << "ms\n";

        SDL_Delay(16);
    }

    // move cleanup out here, after the while loop
    if (netThread.joinable())
        netThread.join();

    if (largeFont.texture) SDL_DestroyTexture(largeFont.texture);
    if (smallFont.texture) SDL_DestroyTexture(smallFont.texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    IMG_Quit();
    SDL_Quit();
    return 0;
}
// if this doesnt work mcdonalds or tim hortons is hiring