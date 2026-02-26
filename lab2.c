/*
 * CSEE 4840 Lab 2
 * Name/UNI: (Fill your names here)
 */

#include "fbputchar.h"
#include "usbkeyboard.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* ================= SERVER ================= */
#define SERVER_HOST "128.59.19.114"
#define SERVER_PORT 42000

/* ================= SCREEN ================= */
#define MAX_COLS 64
#define MAX_ROWS 24

#define INPUT_ROWS 2
#define INPUT_START_ROW (MAX_ROWS - INPUT_ROWS)
#define INPUT_END_ROW   (MAX_ROWS - 1)
#define SEPARATOR_ROW   (INPUT_START_ROW - 1)

#define RECV_START_ROW  0
#define RECV_END_ROW    (SEPARATOR_ROW - 1)
#define RECV_ROWS       (RECV_END_ROW - RECV_START_ROW + 1)

/* ================= KEYCODES ================= */
#define KEY_ENTER     0x28
#define KEY_ESC       0x29
#define KEY_BACKSPACE 0x2A
#define KEY_LEFT      0x50
#define KEY_RIGHT     0x4F

/* ================= INPUT ================= */
#define INPUT_BUFFER_SIZE 1024
static char input_buf[INPUT_BUFFER_SIZE];
static int input_len = 0;
static int cursor_pos = 0;
static int view_offset = 0;
#define VIEW_CAP (MAX_COLS * INPUT_ROWS)

/* ================= RECEIVE ================= */
static char recv_buf[RECV_ROWS][MAX_COLS];
static int recv_row = 0;
static int recv_col = 0;

/* ================= SYNC ================= */
static pthread_mutex_t display_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ================= SOCKET ================= */
static int sockfd = -1;
static pthread_t network_thread;

/* ================= ASCII MAP ================= */
static const char ascii_map[] = {
  0,0,0,0,'a','b','c','d','e','f','g','h','i','j','k','l',
  'm','n','o','p','q','r','s','t','u','v','w','x','y','z','1','2',
  '3','4','5','6','7','8','9','0','\n',0,0,0,' ','-','=','[',
  ']','\\',0,';','\'','`',',','.','/',0,0,0,0,0,0,0
};

static const char ascii_map_shift[] = {
  0,0,0,0,'A','B','C','D','E','F','G','H','I','J','K','L',
  'M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z','!','@',
  '#','$','%','^','&','*','(',')','\n',0,0,0,' ','_','+','{',
  '}','|',0,':','"','~','<','>','?',0,0,0,0,0,0,0
};

static char key_to_ascii(uint8_t code, uint8_t mod)
{
    int shift = (mod & (USB_LSHIFT | USB_RSHIFT)) != 0;
    if (code < sizeof(ascii_map))
        return shift ? ascii_map_shift[code] : ascii_map[code];
    return 0;
}

/* ================= TIME ================= */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL +
           (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* ================= DRAW ================= */
static void clear_region(int r0, int r1)
{
    for (int r = r0; r <= r1; r++)
        for (int c = 0; c < MAX_COLS; c++)
            fbputchar(' ', r, c);
}

static void draw_separator(void)
{
    for (int c = 0; c < MAX_COLS; c++)
        fbputchar('-', SEPARATOR_ROW, c);
}

static void redraw_recv(void)
{
    for (int r = 0; r < RECV_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++)
            fbputchar(recv_buf[r][c], RECV_START_ROW + r, c);
}

static void clamp_view(void)
{
    if (cursor_pos < view_offset)
        view_offset = cursor_pos;

    if (cursor_pos >= view_offset + VIEW_CAP)
        view_offset = cursor_pos - VIEW_CAP + 1;

    if (view_offset < 0)
        view_offset = 0;

    if (view_offset > input_len)
        view_offset = input_len;
}

static void redraw_input(void)
{
    clear_region(INPUT_START_ROW, INPUT_END_ROW);
    clamp_view();

    int shown = input_len - view_offset;
    if (shown > VIEW_CAP)
        shown = VIEW_CAP;

    for (int i = 0; i < shown; i++) {
        int idx = view_offset + i;
        int row = INPUT_START_ROW + (i / MAX_COLS);
        int col = i % MAX_COLS;
        fbputchar(input_buf[idx], row, col);
    }

    int cur = cursor_pos - view_offset;
    int cr = INPUT_START_ROW + (cur / MAX_COLS);
    int cc = cur % MAX_COLS;
    fbputchar('_', cr, cc);
}

/* ================= RECEIVE ================= */
static void recv_scroll_up(void)
{
    memmove(recv_buf[0], recv_buf[1],
            (RECV_ROWS - 1) * MAX_COLS);
    memset(recv_buf[RECV_ROWS - 1], ' ', MAX_COLS);
    if (recv_row > 0)
        recv_row--;
}

static void recv_newline(void)
{
    recv_col = 0;
    recv_row++;
    if (recv_row >= RECV_ROWS) {
        recv_scroll_up();
        recv_row = RECV_ROWS - 1;
    }
}

static void recv_putc_buf(char c)
{
    if (c == '\n') {
        recv_newline();
        return;
    }

    recv_buf[recv_row][recv_col] = c;
    recv_col++;

    if (recv_col >= MAX_COLS)
        recv_newline();
}

/* ================= INPUT ================= */
static void insert_char(char c)
{
    if (input_len >= INPUT_BUFFER_SIZE - 1)
        return;

    for (int i = input_len; i > cursor_pos; i--)
        input_buf[i] = input_buf[i - 1];

    input_buf[cursor_pos] = c;
    input_len++;
    cursor_pos++;
}

static void backspace_char(void)
{
    if (cursor_pos <= 0)
        return;

    for (int i = cursor_pos - 1; i < input_len - 1; i++)
        input_buf[i] = input_buf[i + 1];

    input_len--;
    cursor_pos--;
}

static void clear_input(void)
{
    input_len = 0;
    cursor_pos = 0;
    view_offset = 0;
}

static void send_msg(void)
{
    if (input_len <= 0)
        return;

    write(sockfd, input_buf, input_len);
    write(sockfd, "\n", 1);
    clear_input();
}

/* ================= NETWORK ================= */
static void *net_thread(void *arg)
{
    char buf[256];

    while (1) {
        int n = read(sockfd, buf, sizeof(buf));
        if (n <= 0)
            return NULL;

        if (0)
            continue;

        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || (c >= 32 && c < 127))
                recv_putc_buf(c);
        }

        redraw_recv();
        pthread_mutex_unlock(&display_mutex);
    }
}

/* ================= MAIN ================= */
int main(void)
{
    if (fbopen() != 0) {
        fprintf(stderr, "fbopen failed\n");
        return 1;
    }

    pthread_mutex_lock(&display_mutex);
    clear_region(0, MAX_ROWS - 1);
    draw_separator();

    for (int r = 0; r < RECV_ROWS; r++)
        memset(recv_buf[r], ' ', MAX_COLS);

    redraw_recv();
    redraw_input();
    pthread_mutex_unlock(&display_mutex);

    struct libusb_device_handle *keyboard;
    uint8_t endpoint_address;

    keyboard = openkeyboard(&endpoint_address);
    if (!keyboard) {
        fprintf(stderr, "Keyboard not found\n");
        return 1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_HOST, &serv.sin_addr);

    if (connect(sockfd, (struct sockaddr *)&serv,
                sizeof(serv)) < 0) {
        fprintf(stderr, "connect failed\n");
        return 1;
    }

    pthread_create(&network_thread, NULL, net_thread, NULL);

    struct usb_keyboard_packet p;
    uint8_t prev_keys[6] = {0};
    uint8_t last_code = 0;
    uint8_t held_mod = 0;

    uint64_t press_t = 0;
    uint64_t last_rep = 0;
    const uint64_t rep_delay = 400;
    const uint64_t rep_rate = 60;

    while (1) {
        int transferred;

        if (libusb_interrupt_transfer(
                keyboard, endpoint_address,
                (unsigned char *)&p,
                sizeof(p), &transferred, 0) != 0)
            continue;

        uint64_t t = now_ms();

        for (int i = 0; i < 6; i++) {
            uint8_t code = p.keycode[i];
            if (!code)
                continue;

            int was_down = 0;
            for (int j = 0; j < 6; j++)
                if (prev_keys[j] == code)
                    was_down = 1;

            if (!was_down) {
                last_code = code;
                held_mod = p.modifiers;
                press_t = t;
                last_rep = t;

                pthread_mutex_lock(&display_mutex);

                if (code == KEY_ESC)
                    goto done;
                else if (code == KEY_ENTER)
                 send_msg();
                else if (code == KEY_BACKSPACE)
                    backspace_char();
                else if (code == KEY_LEFT && cursor_pos > 0)
                    cursor_pos--;
                else if (code == KEY_RIGHT && cursor_pos < input_len)
                    cursor_pos++;
                else {
                    char c = key_to_ascii(code, held_mod);
                    if (c && c != '\n')
                        insert_char(c);
                }

                redraw_input();
                pthread_mutex_unlock(&display_mutex);
            }
        }

        memcpy(prev_keys, p.keycode, 6);

        if (last_code &&
            (t - press_t) >= rep_delay &&
            (t - last_rep) >= rep_rate) {

            last_rep = t;

            pthread_mutex_lock(&display_mutex);

            if (last_code == KEY_BACKSPACE)
                backspace_char();
            else if (last_code == KEY_LEFT && cursor_pos > 0)
                cursor_pos--;
            else if (last_code == KEY_RIGHT && cursor_pos < input_len)
                cursor_pos++;
            else {
                char c = key_to_ascii(last_code, held_mod);
                if (c && c != '\n')
                    insert_char(c);
            }

            redraw_input();
            pthread_mutex_unlock(&display_mutex);
        }
    }

done:
    pthread_cancel(network_thread);
    pthread_join(network_thread, NULL);
    return 0;
}
