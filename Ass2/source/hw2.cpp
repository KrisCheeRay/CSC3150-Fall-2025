#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <fcntl.h>

/* const numbers define */
#define ROW 17
#define COLUMN 49
#define HORI_LINE '-'
#define VERT_LINE '|'
#define CORNER '+'
#define PLAYER '0'

/* global variables */
int player_x;
int player_y;
char map[ROW][COLUMN + 1];

/* functions */
int kbhit(void);
void map_print(void);

/* Determine a keyboard is hit or not.
 * If yes, return 1. If not, return 0. */
int kbhit(void)
{
    struct termios oldt, newt;
    int ch;
    int oldf;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    oldf = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, oldf | O_NONBLOCK);
    ch = getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    fcntl(STDIN_FILENO, F_SETFL, oldf);
    if (ch != EOF)
    {
        ungetc(ch, stdin);
        return 1;
    }
    return 0;
}

/* print the map */
void map_print(void)
{
    printf("\033[H\033[2J");
    int i;
    for (i = 0; i <= ROW - 1; i++)
        puts(map[i]);
}

/* main function */
int main(int argc, char *argv[])
{
    srand(time(NULL));
    int i, j;

    /* initialize the map */
    memset(map, 0, sizeof(map));
    for (i = 1; i <= ROW - 2; i++)
    {
        for (j = 1; j <= COLUMN - 2; j++)
        {
            map[i][j] = ' ';
        }
    }
    for (j = 1; j <= COLUMN - 2; j++)
    {
        map[0][j] = HORI_LINE;
        map[ROW - 1][j] = HORI_LINE;
    }
    for (i = 1; i <= ROW - 2; i++)
    {
        map[i][0] = VERT_LINE;
        map[i][COLUMN - 1] = VERT_LINE;
    }
    map[0][0] = CORNER;
    map[0][COLUMN - 1] = CORNER;
    map[ROW - 1][0] = CORNER;
    map[ROW - 1][COLUMN - 1] = CORNER;

    player_x = ROW / 2;
    player_y = COLUMN / 2;
    map[player_x][player_y] = PLAYER;

    map_print();

    return 0;
}
