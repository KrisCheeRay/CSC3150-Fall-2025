#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <fcntl.h>

// -------------------------------
// Game board dimensions & glyphs
// -------------------------------
#define MAP_ROWS 17                 // Total number of rows (including borders)
#define MAP_COLS 49                 // Total number of columns (including borders)
#define BORDER_H '-'                // Horizontal border character
#define BORDER_V '|'                // Vertical border character
#define BORDER_CORNER '+'           // Corner character
#define PLAYER_CHAR '0'             // Player avatar character

// -------------------------------
// Global state
// -------------------------------
int g_playerRow;                    // Current player row (1..MAP_ROWS-2 for interior)
int g_playerCol;                    // Current player column (1..MAP_COLS-2 for interior)
char g_mapBuffer[MAP_ROWS][MAP_COLS + 1]; // Render buffer for the map (C-string per row)

// Forward declarations
int kbhit(void);
void map_print(void);

// ----------------------------------------------
// Non-blocking keyboard check (Linux/UNIX only)
// Returns 1 if a key is available, 0 otherwise.
// Uses termios to set non-canonical, no-echo mode.
// ----------------------------------------------
int kbhit(void)
{
    struct termios termOld, termNew;
    int ch;
    int oldFlags;

    // Get current terminal attributes
    tcgetattr(STDIN_FILENO, &termOld);
    termNew = termOld;

    // Disable canonical mode and echo, so getchar() is non-blocking friendly
    termNew.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &termNew);

    // Set stdin to non-blocking
    oldFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldFlags | O_NONBLOCK);

    // Try reading one character
    ch = getchar();

    // Restore terminal settings and flags
    tcsetattr(STDIN_FILENO, TCSANOW, &termOld);
    fcntl(STDIN_FILENO, F_SETFL, oldFlags);

    // If a character was read, push it back and report hit
    if (ch != EOF)
    {
        ungetc(ch, stdin);
        return 1;
    }
    return 0;
}

// ----------------------------------------------
// Clear screen and print the current dungeon map
// Uses ANSI escape codes to move cursor and clear.
// ----------------------------------------------
void map_print(void)
{
    printf("\033[H\033[2J");  // Move cursor to home & clear screen
    int rowIdx;
    for (rowIdx = 0; rowIdx <= MAP_ROWS - 1; rowIdx++)
        puts(g_mapBuffer[rowIdx]);
}

// ---------------------
// Added game logic
// ---------------------
#define WALL_COUNT 6                // Number of moving wall rows
#define SHARD_COUNT 6               // Number of collectible moving shards
#define WALL_LENGTH 15              // Length of each moving wall segment
#define PLAYFIELD_WIDTH 47          // Number of interior columns (1..47)
#define PLAYFIELD_LEFT 1            // Leftmost interior column (inclusive)
#define PLAYFIELD_RIGHT 47          // Rightmost interior column (inclusive)
#define RENDER_US 50000             // Render refresh interval (microseconds)
#define WALL_STEP_US 150000         // Wall movement tick (microseconds)
#define SHARD_STEP_US 200000        // Shard movement tick (microseconds)

// Moving wall: horizontal segment that wraps within interior columns
typedef struct { int rowIndex; int leftCol; int direction; } Wall;

// Moving shard: collectible token that moves horizontally and can be deactivated
typedef struct { int rowIndex; int columnIndex; int direction; int isAlive; } Shard;

// Fixed rows for walls and shards to occupy
static const int kWallRowPositions[WALL_COUNT]   = {2, 4, 6, 10, 12, 14};
static const int kShardRowPositions[SHARD_COUNT] = {1, 3, 5, 11, 13, 15};

// Runtime arrays
Wall  g_walls[WALL_COUNT];
Shard g_shards[SHARD_COUNT];

// Synchronization for multi-threaded game state
pthread_mutex_t g_stateMutex = PTHREAD_MUTEX_INITIALIZER;

// g_gameState values:
// 0 = playing, 1 = lost (hit wall), 2 = won (collected all shards), 3 = quit
volatile int g_gameState = 0;

// ----------------------------------------------
// Small helpers
// ----------------------------------------------
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Wrap a position within [1..PLAYFIELD_WIDTH] with 1-based indexing
static inline int wrap47(int v) { int r = v % PLAYFIELD_WIDTH; if (r <= 0) r += PLAYFIELD_WIDTH; return r; }

// ----------------------------------------------
// Build the entire render buffer for the frame:
// - Clear to spaces
// - Draw borders
// - Draw moving walls and shards
// - Place the player
// Must be called with state protected if reading shared state.
// ----------------------------------------------
void build_frame(void)
{
    int r, c;

    // Clear the entire dungeon to spaces and terminate each row as a C-string
    for (r = 0; r < MAP_ROWS; ++r) {
        for (c = 0; c < MAP_COLS; ++c) g_mapBuffer[r][c] = ' ';
        g_mapBuffer[r][MAP_COLS] = '\0';
    }

    // Top and bottom borders
    for (c = 1; c <= MAP_COLS - 2; ++c) {
        g_mapBuffer[0][c] = BORDER_H;
        g_mapBuffer[MAP_ROWS - 1][c] = BORDER_H;
    }

    // Left and right borders
    for (r = 1; r <= MAP_ROWS - 2; ++r) {
        g_mapBuffer[r][0] = BORDER_V;
        g_mapBuffer[r][MAP_COLS - 1] = BORDER_V;
    }

    // Corners
    g_mapBuffer[0][0] = BORDER_CORNER;
    g_mapBuffer[0][MAP_COLS - 1] = BORDER_CORNER;
    g_mapBuffer[MAP_ROWS - 1][0] = BORDER_CORNER;
    g_mapBuffer[MAP_ROWS - 1][MAP_COLS - 1] = BORDER_CORNER;

    // Draw moving walls as '=' wrapping across the interior
    for (int i = 0; i < WALL_COUNT; ++i) {
        int row = g_walls[i].rowIndex;
        int left = g_walls[i].leftCol;
        for (int k = 0; k < WALL_LENGTH; ++k) {
            int col = ((left + k - 1) % PLAYFIELD_WIDTH) + 1; // wrap 1..PLAYFIELD_WIDTH
            g_mapBuffer[row][col] = '=';
        }
    }

    // Draw alive shards as '$'
    for (int i = 0; i < SHARD_COUNT; ++i) {
        if (!g_shards[i].isAlive) continue;
        g_mapBuffer[g_shards[i].rowIndex][g_shards[i].columnIndex] = '$';
    }

    // Place the player
    g_mapBuffer[g_playerRow][g_playerCol] = PLAYER_CHAR;
}

// ----------------------------------------------
// Collision check between player and any wall.
// Requires state to be locked by caller.
// Returns 1 on collision, 0 otherwise.
// ----------------------------------------------
int player_hits_wall_locked(void)
{
    for (int i = 0; i < WALL_COUNT; ++i) {
        int left = g_walls[i].leftCol;
        for (int k = 0; k < WALL_LENGTH; ++k) {
            int col = ((left + k - 1) % PLAYFIELD_WIDTH) + 1;
            if (g_playerRow == g_walls[i].rowIndex && g_playerCol == col) return 1;
        }
    }
    return 0;
}

// ----------------------------------------------
// Collect a shard if the player is standing on it.
// Requires state to be locked by caller.
// ----------------------------------------------
void collect_shard_if_any_locked(void)
{
    for (int i = 0; i < SHARD_COUNT; ++i) {
        if (!g_shards[i].isAlive) continue;
        if (g_shards[i].rowIndex == g_playerRow && g_shards[i].columnIndex == g_playerCol) g_shards[i].isAlive = 0;
    }
}

// ----------------------------------------------
// Check if all shards have been collected.
// Requires state to be locked by caller.
// Returns 1 if all collected, 0 otherwise.
// ----------------------------------------------
int all_shards_collected_locked(void)
{
    for (int i = 0; i < SHARD_COUNT; ++i) if (g_shards[i].isAlive) return 0;
    return 1;
}

// ----------------------------------------------
// Thread: moves a specific wall horizontally.
// Ticks every WALL_STEP_US microseconds.
// On collision with player, sets g_gameState = 1 (lose).
// ----------------------------------------------
void* wall_worker(void* arg)
{
    int wallIndex = (int)(intptr_t)arg;
    while (1) {
        usleep(WALL_STEP_US);
        pthread_mutex_lock(&g_stateMutex);
        if (g_gameState) { pthread_mutex_unlock(&g_stateMutex); break; }

        // Advance wall and wrap
        g_walls[wallIndex].leftCol = wrap47(g_walls[wallIndex].leftCol + g_walls[wallIndex].direction);

        // Collision check with player
        if (player_hits_wall_locked()) g_gameState = 1;

        pthread_mutex_unlock(&g_stateMutex);
    }
    return NULL;
}

// ----------------------------------------------
// Thread: moves a specific shard horizontally if alive.
// Ticks every SHARD_STEP_US microseconds.
// Collects on overlap; sets g_gameState = 2 if all collected.
// ----------------------------------------------
void* shard_worker(void* arg)
{
    int shardIndex = (int)(intptr_t)arg;
    while (1) {
        usleep(SHARD_STEP_US);
        pthread_mutex_lock(&g_stateMutex);
        if (g_gameState) { pthread_mutex_unlock(&g_stateMutex); break; }

        if (g_shards[shardIndex].isAlive) {
            // Move and wrap
            g_shards[shardIndex].columnIndex = wrap47(g_shards[shardIndex].columnIndex + g_shards[shardIndex].direction);

            // Auto-collect if player is on the shard after movement
            if (g_shards[shardIndex].rowIndex == g_playerRow && g_shards[shardIndex].columnIndex == g_playerCol) g_shards[shardIndex].isAlive = 0;

            // Win condition check
            if (all_shards_collected_locked()) g_gameState = 2;
        }
        pthread_mutex_unlock(&g_stateMutex);
    }
    return NULL;
}

// ----------------------------------------------
// Thread: renders the frame at a fixed interval.
// Stops when g_gameState != 0.
// ----------------------------------------------
void* render_worker(void* arg)
{
    (void)arg;
    while (1) {
        usleep(RENDER_US);

        pthread_mutex_lock(&g_stateMutex);
        build_frame();          // Rebuild the buffer from current state
        map_print();            // Print it to the terminal
        int isOver = g_gameState;  // Snapshot game state for exit decision
        pthread_mutex_unlock(&g_stateMutex);

        if (isOver) break;
    }
    return NULL;
}

// ----------------------------------------------
// Program entry point
// - Initializes board, player, walls, shards
// - Spawns worker threads (walls, shards, renderer)
// - Main loop handles player input (WASD to move, Q to quit)
// - Joins threads and prints end result
// ----------------------------------------------
int main(int argc, char *argv[])
{
    srand(time(NULL));
    int rowIdx, colIdx;

    // Initialize the map buffer to spaces
    memset(g_mapBuffer, 0, sizeof(g_mapBuffer));
    for (rowIdx = 1; rowIdx <= MAP_ROWS - 2; rowIdx++)
    {
        for (colIdx = 1; colIdx <= MAP_COLS - 2; colIdx++)
        {
            g_mapBuffer[rowIdx][colIdx] = ' ';
        }
    }

    // Draw top and bottom borders
    for (colIdx = 1; colIdx <= MAP_COLS - 2; colIdx++)
    {
        g_mapBuffer[0][colIdx] = BORDER_H;
        g_mapBuffer[MAP_ROWS - 1][colIdx] = BORDER_H;
    }

    // Draw side borders
    for (rowIdx = 1; rowIdx <= MAP_ROWS - 2; rowIdx++)
    {
        g_mapBuffer[rowIdx][0] = BORDER_V;
        g_mapBuffer[rowIdx][MAP_COLS - 1] = BORDER_V;
    }

    // Draw corners
    g_mapBuffer[0][0] = BORDER_CORNER;
    g_mapBuffer[0][MAP_COLS - 1] = BORDER_CORNER;
    g_mapBuffer[MAP_ROWS - 1][0] = BORDER_CORNER;
    g_mapBuffer[MAP_ROWS - 1][MAP_COLS - 1] = BORDER_CORNER;

    // Place player in the center
    g_playerRow = MAP_ROWS / 2;
    g_playerCol = MAP_COLS / 2;
    g_mapBuffer[g_playerRow][g_playerCol] = PLAYER_CHAR;

    // Initialize moving walls with alternating directions and random start columns
    for (int w = 0; w < WALL_COUNT; ++w) {
        g_walls[w].rowIndex = kWallRowPositions[w];
        g_walls[w].leftCol = (rand() % PLAYFIELD_WIDTH) + 1;
        g_walls[w].direction = (w % 2 == 0) ? +1 : -1;  // Even walls move right, odd move left
    }

    // Initialize shards with random columns and random directions; all alive
    for (int s = 0; s < SHARD_COUNT; ++s) {
        g_shards[s].rowIndex = kShardRowPositions[s];
        g_shards[s].columnIndex = (rand() % PLAYFIELD_WIDTH) + 1;
        g_shards[s].direction = (rand() & 1) ? +1 : -1;
        g_shards[s].isAlive = 1;
    }

    // Spawn renderer thread
    pthread_t wallThreads[WALL_COUNT], shardThreads[SHARD_COUNT], renderThreadId;
    pthread_create(&renderThreadId, NULL, render_worker, NULL);

    // Spawn wall and shard worker threads
    for (int w = 0; w < WALL_COUNT; ++w) pthread_create(&wallThreads[w], NULL, wall_worker, (void*)(intptr_t)w);
    for (int s = 0; s < SHARD_COUNT; ++s) pthread_create(&shardThreads[s], NULL, shard_worker, (void*)(intptr_t)s);

    // Main input loop: handles WASD movement and Q to quit
    while (1) {
        if (kbhit()) {
            int key = getchar();

            pthread_mutex_lock(&g_stateMutex);
            if (!g_gameState) {
                int nextRow = g_playerRow, nextCol = g_playerCol;

                // Movement keys
                if (key == 'w' || key == 'W') nextRow--;
                else if (key == 's' || key == 'S') nextRow++;
                else if (key == 'a' || key == 'A') nextCol--;
                else if (key == 'd' || key == 'D') nextCol++;
                else if (key == 'q' || key == 'Q') g_gameState = 3; // Quit

                // Clamp to interior bounds
                nextRow = clampi(nextRow, 1, MAP_ROWS - 2);
                nextCol = clampi(nextCol, 1, MAP_COLS - 2);

                // Apply movement
                g_playerRow = nextRow;
                g_playerCol = nextCol;

                // Check lose condition after moving
                if (!g_gameState && player_hits_wall_locked()) g_gameState = 1;

                // Check collect/win conditions
                if (!g_gameState) {
                    collect_shard_if_any_locked();
                    if (all_shards_collected_locked()) g_gameState = 2;
                }
            }
            pthread_mutex_unlock(&g_stateMutex);
        }

        // Exit if game has ended
        pthread_mutex_lock(&g_stateMutex);
        int isOver = g_gameState;
        pthread_mutex_unlock(&g_stateMutex);
        if (isOver) break;

        // Small sleep to reduce busy waiting
        usleep(10000);
    }

    // Join all worker threads
    for (int w = 0; w < WALL_COUNT; ++w) pthread_join(wallThreads[w], NULL);
    for (int s = 0; s < SHARD_COUNT; ++s) pthread_join(shardThreads[s], NULL);
    pthread_join(renderThreadId, NULL);

    // Final render and outcome message
    pthread_mutex_lock(&g_stateMutex);
    build_frame();
    map_print();
    int finalStatus = g_gameState;
    pthread_mutex_unlock(&g_stateMutex);

    if (finalStatus == 1) printf("You lose the game!!\n");
    else if (finalStatus == 2) printf("You win the game!!\n");
    else if (finalStatus == 3) printf("You exit the game.\n");

    return 0;
}
