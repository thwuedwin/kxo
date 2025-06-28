#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "game.h"

#define XO_STATUS_FILE "/sys/module/kxo/initstate"
#define XO_DEVICE_FILE "/dev/kxo"
#define XO_DEVICE_ATTR_FILE "/sys/class/kxo/kxo/kxo_state"
#define TIME_DATA_FILE "./data/time_data"
#define LOADAVG_FILE "/proc/loadavg"

static bool status_check(void)
{
    FILE *fp = fopen(XO_STATUS_FILE, "r");
    if (!fp) {
        printf("kxo status : not loaded\n");
        return false;
    }

    char read_buf[20];
    fgets(read_buf, 20, fp);
    read_buf[strcspn(read_buf, "\n")] = 0;
    if (strcmp("live", read_buf)) {
        printf("kxo status : %s\n", read_buf);
        fclose(fp);
        return false;
    }
    fclose(fp);
    return true;
}

static struct termios orig_termios;

static void raw_mode_disable(void)
{
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

static void raw_mode_enable(void)
{
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(raw_mode_disable);
    struct termios raw = orig_termios;
    raw.c_iflag &= ~IXON;
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool read_attr, end_attr;
const char *ai_funcs_name[] = {"mcts", "negamax"};
const char *ai_one_name, *ai_two_name;

static void choose_ai(char *buf)
{
    char input;
    printf("\033[H\033[J");
    printf(
        "\n\n"
        "Choose AI agent for player 1\n"
        "0: %s\n"
        "1: %s\n",
        ai_funcs_name[0], ai_funcs_name[1]);
choose_ai_one:
    while (read(STDIN_FILENO, &input, 1) < 0)
        ;
    switch (input) {
    case '0':
    case '1':
        buf[6] = input;
        ai_one_name = ai_funcs_name[input - '0'];
        break;
    default:
        goto choose_ai_one;
    }

    printf("\033[H\033[J");
    printf(
        "\n\n"
        "Choose AI agent for player 2\n"
        "0: %s\n"
        "1: %s\n",
        ai_funcs_name[0], ai_funcs_name[1]);
choose_ai_two:
    while (read(STDIN_FILENO, &input, 1) < 0)
        ;
    switch (input) {
    case '0':
    case '1':
        buf[8] = input;
        ai_two_name = ai_funcs_name[input - '0'];
        break;
    default:
        goto choose_ai_two;
    }
}

static void listen_keyboard_handler(void)
{
    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char input;

    if (read(STDIN_FILENO, &input, 1) == 1) {
        char buf[20];
        switch (input) {
        case 16: /* Ctrl-P */
            read(attr_fd, buf, 10);
            buf[0] = (buf[0] - '0') ? '0' : '1';
            read_attr ^= 1;
            write(attr_fd, buf, 10);
            if (!read_attr)
                printf("\n\nStopping to display the chess board...\n");
            break;
        case 17: /* Ctrl-Q */
            read(attr_fd, buf, 10);
            buf[4] = '1';
            read_attr = false;
            end_attr = true;
            write(attr_fd, buf, 10);
            printf("\n\nStopping the kernel space tic-tac-toe game...\n");
            break;
        case 1: /* Ctrl-A */
            read(attr_fd, buf, 10);
            /* Stop kernel module while choosing AI */
            if (buf[0] - '0')
                write(attr_fd, buf, 10);
            choose_ai(buf);
            buf[0] = '1';
            read_attr = 1;
            write(attr_fd, buf, 10);
            break;
        }
    }
    close(attr_fd);
}

static void draw_board(const char *data_buf)
{
    printf("\n\n");
    for (int i = 0; i < (BOARD_SIZE << 1) - 1; i++) {
        for (int j = 0; j < (BOARD_SIZE << 1) - 1; j++) {
            if (i & 1) {
                printf("-");
                continue;
            }

            printf("%c",
                   (j & 1) ? '|' : data_buf[(i >> 1) * BOARD_SIZE + (j >> 1)]);
        }
        printf("\n");
    }

    // Only serves for board size = 4
    int steps = data_buf[DATABUFFER_SIZE];
    int start = DATABUFFER_SIZE + 1;
    printf("\nMoves: ");
    for (int i = 0; i < steps; i++) {
        printf("%c%c", 'A' + (int) ((data_buf[start + i] >> 2) & 0x3),
               '0' + (int) ((data_buf[start + i] & 0x3)));
        if (i != steps - 1)
            printf(" -> ");
    }
}

int main(int argc, char *argv[])
{
    if (!status_check())
        exit(1);

    raw_mode_enable();
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    char data_buf[DATABUFFER_SIZE + STEPS_DATA_SIZE];

    fd_set readset;
    int device_fd = open(XO_DEVICE_FILE, O_RDONLY);
    if (device_fd < 0) {
        perror("Failed to open kxo device");
        exit(1);
    }

    int max_fd = device_fd > STDIN_FILENO ? device_fd : STDIN_FILENO;
    read_attr = true;
    end_attr = false;

    char loadavg_buf[64];

    int attr_fd = open(XO_DEVICE_ATTR_FILE, O_RDWR);
    char buf[20];
    read(attr_fd, buf, 10);
    ai_one_name = ai_funcs_name[buf[6] - '0'];
    ai_two_name = ai_funcs_name[buf[8] - '0'];
    close(attr_fd);


    while (!end_attr) {
        FD_ZERO(&readset);
        FD_SET(STDIN_FILENO, &readset);
        FD_SET(device_fd, &readset);

        int result = select(max_fd + 1, &readset, NULL, NULL, NULL);
        if (result < 0) {
            printf("Error with select system call\n");
            exit(1);
        }

        if (FD_ISSET(STDIN_FILENO, &readset)) {
            FD_CLR(STDIN_FILENO, &readset);
            listen_keyboard_handler();
        } else if (read_attr && FD_ISSET(device_fd, &readset)) {
            FD_CLR(device_fd, &readset);
            printf("\033[H\033[J"); /* ASCII escape code to clear the screen */
            read(device_fd, data_buf, DATABUFFER_SIZE + STEPS_DATA_SIZE);
            draw_board(data_buf);

            time_t now = time(NULL);
            const struct tm *tm_now = localtime(&now);
            printf("\n\nCurrent time: %04d-%02d-%02d %02d:%02d:%02d\n",
                   tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
                   tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);

            int loadavg_fd = open(LOADAVG_FILE, O_RDONLY);
            if (loadavg_fd < 0) {
                perror("Failed to open loadavg");
                goto out;
            }

            int loadavg_len =
                read(loadavg_fd, loadavg_buf, sizeof(loadavg_buf) - 1);
            if (loadavg_len < 0) {
                perror("read loadavg failed.");
                close(loadavg_fd);
                goto out;
            }
            loadavg_buf[loadavg_len] = '\0';
            printf("System load average: %s", loadavg_buf);
            close(loadavg_fd);

            printf("Player 1 AI: %s\n", ai_one_name);
            printf("Player 2 AI: %s\n", ai_two_name);
        }
    }

out:
    raw_mode_disable();
    fcntl(STDIN_FILENO, F_SETFL, flags);

    close(device_fd);

    return 0;
}
