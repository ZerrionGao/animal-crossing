#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"

static uint64_t fake_now;
static int fake_movie;
static unsigned user_input_calls;
static RuntimeGamepadState second_pad;
static int fake_handheld;

uint64_t runtime_now_ns(void) { return fake_now; }
void runtime_survey_cancel_user(void) { user_input_calls++; }
void runtime_autoload_user_input(void) { user_input_calls++; }
int runtime_handheld_mode(void) { return fake_handheld; }
void runtime_gamepad_read(RuntimeGamepadState *out) {
    *out = (RuntimeGamepadState){0};
}
void runtime_gamepad_read_player(unsigned player, RuntimeGamepadState *out) { *out = player == 1 ? second_pad : (RuntimeGamepadState){0}; }
int runtime_gamepad_player_connected(unsigned player) { return player == 1 && second_pad.connected; }
void runtime_gamepad_clear(void) {}
void *runtime_hid_feedback_override(const char *name) { (void)name; return NULL; }
int runtime_hid_feedback_flag(const char *name, int value) { (void)name; (void)value; return 0; }
int runtime_survey_command(const char *line) { (void)line; return 0; }
int movie_host_configured(void) { return fake_movie; }
_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "probe failure: %s\n", message);
    abort();
}

void dm_gfx_shot_at(long frames, const char *path) { (void)frames; (void)path; }
void dm_gfx_passdump_next(long frames) { (void)frames; }
void dm_gfx_anomdump(void) {}
void dm_gfx_querylog_next(long frames) { (void)frames; }
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
void dm_gfx_ashot(const char *path) { (void)path; }
void runtime_front_request_close(void) {}
void runtime_front_text_test(const char *operation, const char *text) { (void)operation; (void)text; }
void runtime_swkbd_selftest(void) {}
void runtime_hle_trace_set(int mode) { (void)mode; }
void runtime_frame_step_pin(double milliseconds) { (void)milliseconds; }
void runtime_diag_frm_watch(double seconds, unsigned offset, int in_x) {
    (void)seconds; (void)offset; (void)in_x;
}
void runtime_diag_xpoke(unsigned offset, float value) { (void)offset; (void)value; }
void runtime_dump_threads(void) {}

#include "../engine/runtime/input.c"

static void rush_key_up(void) { runtime_input_key(18, 0); }
static void rush_key_down(void) { runtime_input_key(18, 1); }

int main(void) {
    initialize();
    uint32_t controller = 0;
    NpadState state;

    /* 首次 down 才切换；AppKit 的重复 keyDown 仍要走真实用户输入路径。 */
    rush_key_down();
    assert(atomic_load(&dialogue_rush) == 1);
    unsigned calls_after_first_down = user_input_calls;
    rush_key_down();
    assert(atomic_load(&dialogue_rush) == 1);
    assert(user_input_calls == calls_after_first_down + 2);
    rush_key_up();

    /* 影片未开始时生成 B，手动关闭只清合成 B。 */
    runtime_input_key(38, 1); /* J：独立的 A 键 */
    fake_now = 160000000;
    assert(pad_states(&state, 1, &controller, 1) == 1);
    assert(state.buttons & button(38));
    assert(state.buttons & button(49));
    rush_key_down();
    assert(atomic_load(&dialogue_rush) == 0);
    assert(dialogue_rush_b == 0);
    assert(pad_states(&state, 1, &controller, 1) == 1);
    assert(state.buttons & button(38));
    assert(!(state.buttons & button(49)));
    rush_key_up();

    /* 影片配置完成时自动关闭，同样清掉 B，但不清用户的 A 键。 */
    rush_key_down();
    rush_key_up();
    fake_movie = 0;
    fake_now = 160000000;
    assert(pad_states(&state, 1, &controller, 1) == 1);
    assert(state.buttons & button(49));
    fake_movie = 1;
    assert(pad_states(&state, 1, &controller, 1) == 1);
    assert(atomic_load(&dialogue_rush) == 0);
    assert(dialogue_rush_b == 0);
    assert(state.buttons & button(38));
    assert(!(state.buttons & button(49)));

    /* 失焦清理也必须关闭 rush；否则下一轮状态读取会重新合成 B。 */
    rush_key_down();
    rush_key_up();
    fake_movie = 0;
    fake_now = 160000000;
    assert(pad_states(&state, 1, &controller, 1) == 1);
    assert(state.buttons & button(49));
    runtime_input_clear();
    assert(atomic_load(&dialogue_rush) == 0);
    assert(dialogue_rush_key_down == 0);
    assert(dialogue_rush_b == 0);
    for (int i = 0; i < 3; i++) {
        assert(pad_states(&state, 1, &controller, 1) == 1);
        assert(!(state.buttons & button(49)));
        assert(!(state.buttons & button(38)));
    }
    runtime_input_key(38, 0);
    /* 玩家二仅在游戏声明支持、主机模式和手柄连接时暴露，输入不混入玩家一。 */
    uint32_t ids[] = {0, 1}, second = 1;
    supported_ids(ids, 2);
    supported_styles(1);
    second_pad = (RuntimeGamepadState){.connected = 1, .buttons = 4, .lx = 1234, .ry = -2345};
    assert(style(&second) == 1);
    assert(pad_states(&state, 1, &second, 1) == 1);
    assert(state.attributes == 1 && state.buttons == 4 && state.lx == 1234 && state.ry == -2345);
    assert(pad_states(&state, 1, &controller, 1) == 1 && !(state.buttons & 4));
    supported_ids(ids, 1);
    assert(style(&second) == 0);
    assert(pad_states(&state, 1, &second, 1) == 1 && state.attributes == 0);
    supported_ids(ids, 2);
    fake_handheld = 1;
    assert(style(&second) == 0);
    assert(pad_states(&state, 1, &second, 1) == 1 && state.attributes == 0);
    fake_handheld = 0;
    second_pad.connected = 0;
    assert(style(&second) == 0);
    assert(pad_states(&state, 1, &second, 1) == 1 && state.attributes == 0);
    return 0;
}
