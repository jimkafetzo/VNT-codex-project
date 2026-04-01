#define _WIN32_WINNT 0x0501
#define _WIN32_IE 0x0500
#define SDL_MAIN_HANDLED

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define ID_BTN_OPEN 1
#define ID_BTN_PLAY 2
#define ID_BTN_PAUSE 3
#define ID_BTN_CONV 4
#define ID_SLIDER   5
#define ID_TIMER_UPDATE 100


// 1. ΔΟΜΕΣ & ΜΕΤΑΒΛΗΤΕΣ

float g_volume = 0.5f;
uint32_t g_total_samples = 0;
uint32_t g_current_sample = 0;

typedef struct {
    FILE *file;
    int16_t prev_L, prev_R;
    int16_t next_L, next_R;
    int dt_total, dt_current;
    bool is_finished;
} VntState;

VntState g_state = {0};
SDL_AudioDeviceID g_dev = 0;
HWND hStatusLabel, hVolumeSlider, hProgressBar;

uint16_t to_12bit(int16_t sample) { return (uint16_t)((sample + 32768) >> 4); }
int16_t from_12bit(uint16_t val12) { return (int16_t)((val12 << 4) - 32768); }


// 2. VNT OPTIMIZED DELTA ENCODER

bool convert_wav_to_vnt(const char* in_path, const char* out_path) {
    FILE *f_in = fopen(in_path, "rb");
    if (!f_in) return false;

    // Robust WAV Header Parsing
    fseek(f_in, 22, SEEK_SET);
    uint16_t channels; fread(&channels, 2, 1, f_in);
    uint32_t sample_rate; fread(&sample_rate, 4, 1, f_in);
    
    fseek(f_in, 12, SEEK_SET);
    char chunk_id[4]; uint32_t chunk_size;
    while (fread(chunk_id, 1, 4, f_in) == 4) {
        fread(&chunk_size, 4, 1, f_in);
        if (strncmp(chunk_id, "data", 4) == 0) break;
        fseek(f_in, chunk_size, SEEK_CUR);
    }

    int16_t *raw = malloc(chunk_size);
    if (!raw) { fclose(f_in); return false; }
    uint32_t bytes_read = fread(raw, 1, chunk_size, f_in);
    fclose(f_in);

    FILE *f_out = fopen(out_path, "wb");
    if (!f_out) { free(raw); return false; }

    uint32_t total_out = (bytes_read / (channels * 2)) / 2;
    uint32_t fs_out = sample_rate / 2;

    fwrite("VDL2", 1, 4, f_out); 
    fwrite(&fs_out, 4, 1, f_out);
    fwrite(&total_out, 4, 1, f_out);

    int16_t last_L = 0, last_R = 0;
    int last_time = 0;

    for (uint32_t i = 0; i < total_out; i++) {
        int idx = i * 2 * channels;
        int16_t currL = raw[idx];
        int16_t currR = (channels >= 2) ? raw[idx+1] : currL;

        if (abs(currL - last_L) > 100 || abs(currR - last_R) > 100 || i == 0 || (i - last_time) >= 127) {
            uint8_t dt = (uint8_t)(i - last_time);
            int16_t dL = (currL - last_L) / 16;
            int16_t dR = (currR - last_R) / 16;

            if (dL >= -8 && dL <= 7 && dR >= -8 && dR <= 7 && i != 0) {
                uint8_t b1 = 0x80 | (dt & 0x7F);
                uint8_t b2 = ((dL + 8) & 0x0F) | (((dR + 8) & 0x0F) << 4);
                fwrite(&b1, 1, 1, f_out); fwrite(&b2, 1, 1, f_out);
            } else {
                uint8_t b1 = (dt & 0x7F);
                uint16_t vL = to_12bit(currL);
                uint16_t vR = to_12bit(currR);
                uint8_t pack[3];
                pack[0] = (uint8_t)(vL & 0xFF);
                pack[1] = (uint8_t)((vL >> 8) | (vR << 4));
                pack[2] = (uint8_t)(vR >> 4);
                fwrite(&b1, 1, 1, f_out); fwrite(pack, 1, 3, f_out);
            }
            last_L = currL; last_R = currR; last_time = i;
        }
    }
    fclose(f_out); free(raw);
    return true;
}


 // 3. PLAYER & CALLBACK
void audio_callback(void *userdata, Uint8 *stream, int len) {
    VntState *s = (VntState *)userdata;
    int16_t *buf = (int16_t *)stream;
    int samples_to_fill = len / 4; 

    for (int i = 0; i < samples_to_fill; i++) {
        if (s->is_finished) { buf[i*2] = buf[i*2+1] = 0; continue; }
        if (s->dt_current >= s->dt_total) {
            uint8_t b1;
            if (fread(&b1, 1, 1, s->file) == 1) {
                s->prev_L = s->next_L; s->prev_R = s->next_R;
                s->dt_total = (b1 & 0x7F);
                if (b1 & 0x80) {
                    uint8_t b2; fread(&b2, 1, 1, s->file);
                    s->next_L = s->prev_L + (((b2 & 0x0F) - 8) * 16);
                    s->next_R = s->prev_R + ((((b2 >> 4) & 0x0F) - 8) * 16);
                } else {
                    uint8_t p[3]; fread(p, 1, 3, s->file);
                    uint16_t vL = p[0] | ((p[1] & 0x0F) << 8);
                    uint16_t vR = (p[1] >> 4) | (p[2] << 4);
                    s->next_L = from_12bit(vL); s->next_R = from_12bit(vR);
                }
                s->dt_current = 0;
            } else { s->is_finished = true; continue; }
        }
        float t = (s->dt_total == 0) ? 1.0f : (float)s->dt_current / (float)s->dt_total;
        buf[i*2]   = (int16_t)((s->prev_L + t*(s->next_L - s->prev_L)) * g_volume);
        buf[i*2+1] = (int16_t)((s->prev_R + t*(s->next_R - s->prev_R)) * g_volume);
        s->dt_current++; g_current_sample++;
    }
}

void stop_playback() {
    if (g_dev) { SDL_PauseAudioDevice(g_dev, 1); SDL_CloseAudioDevice(g_dev); g_dev = 0; }
    if (g_state.file) { fclose(g_state.file); g_state.file = NULL; }
    g_current_sample = 0;
}

bool load_vnt(const char* path) {
    stop_playback();
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char sig[4]; fread(sig, 1, 4, f);
    if (strncmp(sig, "VDL2", 4) != 0) { fclose(f); return false; }
    uint32_t fs; fread(&fs, 4, 1, f); fread(&g_total_samples, 4, 1, f);
    g_state.file = f; g_state.is_finished = false; g_state.dt_current = 0; g_state.dt_total = 0;
    SDL_AudioSpec w; SDL_zero(w);
    w.freq = fs; w.format = AUDIO_S16SYS; w.channels = 2;
    w.samples = 2048; w.callback = audio_callback; w.userdata = &g_state;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &w, NULL, 0);
    return (g_dev != 0);
}


// 4. GUI & WINMAIN

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: SetTimer(hwnd, ID_TIMER_UPDATE, 100, NULL); return 0;
        case WM_TIMER:
            if (wParam == ID_TIMER_UPDATE && g_total_samples > 0) {
                int progress = (int)((float)g_current_sample / g_total_samples * 100);
                SendMessage(hProgressBar, PBM_SETPOS, progress, 0);
            }
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == ID_BTN_OPEN) {
                OPENFILENAMEA ofn = {0}; char sz[260] = {0};
                ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
                ofn.lpstrFile = sz; ofn.nMaxFile = sizeof(sz);
                ofn.lpstrFilter = "VNT Audio\0*.vnt\0"; ofn.Flags = OFN_FILEMUSTEXIST;
                if (GetOpenFileNameA(&ofn)) { if (load_vnt(sz)) { SetWindowTextA(hStatusLabel, "Status: Ready"); SendMessage(hProgressBar, PBM_SETPOS, 0, 0); } }
            } else if (id == ID_BTN_CONV) {
                OPENFILENAMEA ofn = {0}; char szI[260] = {0}, szO[260] = {0};
                ofn.lStructSize = sizeof(ofn); ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szI; ofn.nMaxFile = sizeof(szI);
                ofn.lpstrFilter = "WAV Audio\0*.wav\0";
                if (GetOpenFileNameA(&ofn)) {
                    OPENFILENAMEA sfn = {0}; sfn.lStructSize = sizeof(sfn); sfn.hwndOwner = hwnd;
                    sfn.lpstrFile = szO; sfn.nMaxFile = sizeof(szO);
                    sfn.lpstrFilter = "VNT Audio\0*.vnt\0"; sfn.lpstrDefExt = "vnt";
                    if (GetSaveFileNameA(&sfn)) {
                        SetWindowTextA(hStatusLabel, "Encoding...");
                        if (convert_wav_to_vnt(szI, szO)) MessageBoxA(hwnd, "VNT Created!", "Success", MB_OK);
                        SetWindowTextA(hStatusLabel, "Status: Ready");
                    }
                }
            } else if (id == ID_BTN_PLAY && g_dev) { SDL_PauseAudioDevice(g_dev, 0); SetWindowTextA(hStatusLabel, "Status: Playing"); }
            else if (id == ID_BTN_PAUSE && g_dev) { SDL_PauseAudioDevice(g_dev, 1); SetWindowTextA(hStatusLabel, "Status: Paused"); }
            break;
        }
        case WM_HSCROLL: {
            if ((HWND)lParam == hVolumeSlider) {
                int pos = SendMessage(hVolumeSlider, TBM_GETPOS, 0, 0);
                g_volume = (float)pos / 100.0f;
            }
            break;
        }
        case WM_DESTROY: stop_playback(); KillTimer(hwnd, ID_TIMER_UPDATE); SDL_Quit(); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    srand((unsigned int)time(NULL)); 
    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_BAR_CLASSES }; InitCommonControlsEx(&ic);
    SDL_Init(SDL_INIT_AUDIO);
    WNDCLASSA wc = {0}; wc.lpfnWndProc = WindowProc; wc.hInstance = hInst;
    wc.lpszClassName = "VNTPLAYER"; wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowExA(0, "VNTPLAYER", "VNT Player", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, 100, 100, 340, 340, NULL, NULL, hInst, NULL);
    HICON hIcon = LoadIcon(hInst, "MAINICON");
    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    CreateWindowExA(0, "BUTTON", "Convert WAV to VNT", WS_CHILD | WS_VISIBLE, 20, 20, 280, 35, hwnd, (HMENU)ID_BTN_CONV, hInst, NULL);
    CreateWindowExA(0, "BUTTON", "Open VNT File", WS_CHILD | WS_VISIBLE, 20, 65, 280, 35, hwnd, (HMENU)ID_BTN_OPEN, hInst, NULL);
    CreateWindowExA(0, "BUTTON", "Play", WS_CHILD | WS_VISIBLE, 20, 110, 135, 35, hwnd, (HMENU)ID_BTN_PLAY, hInst, NULL);
    CreateWindowExA(0, "BUTTON", "Pause", WS_CHILD | WS_VISIBLE, 165, 110, 135, 35, hwnd, (HMENU)ID_BTN_PAUSE, hInst, NULL);
    CreateWindowExA(0, "STATIC", "Volume:", WS_CHILD | WS_VISIBLE, 20, 160, 60, 20, hwnd, NULL, hInst, NULL);
    hVolumeSlider = CreateWindowExA(0, TRACKBAR_CLASS, "", WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS, 80, 155, 220, 30, hwnd, (HMENU)ID_SLIDER, hInst, NULL);
    SendMessage(hVolumeSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100)); SendMessage(hVolumeSlider, TBM_SETPOS, TRUE, 50);
    CreateWindowExA(0, "STATIC", "Progress:", WS_CHILD | WS_VISIBLE, 20, 200, 60, 20, hwnd, NULL, hInst, NULL);
    hProgressBar = CreateWindowExA(0, PROGRESS_CLASS, "", WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 20, 225, 280, 20, hwnd, NULL, hInst, NULL);
    SendMessage(hProgressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    hStatusLabel = CreateWindowExA(0, "STATIC", "Status: Ready", WS_CHILD | WS_VISIBLE | SS_CENTER, 20, 260, 280, 20, hwnd, NULL, hInst, NULL);
    ShowWindow(hwnd, nShow);
    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return 0;
}